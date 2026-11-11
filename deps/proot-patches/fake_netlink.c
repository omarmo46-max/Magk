/*
 * fake_netlink: PRoot extension for the Magk sandbox (--fake-netlink).
 *
 * Android refuses NETLINK_ROUTE sockets to apps. Everything that lists network
 * interfaces through rtnetlink - Go's net.Interfaces (static binaries such as
 * tailscale), busybox/iproute2 `ip`, libuv, ... - fails at startup. This
 * extension answers the rtnetlink *dump* requests itself:
 *
 *   rtnetlink -> loopback only (`lo`, 127.0.0.1/8 and ::1/128); SO_MARK -> no-op
 *
 * How it works (all in the tracer, nothing is installed in the guest):
 *  - socket(AF_NETLINK, *, NETLINK_ROUTE) is rewritten into opening a private
 *    FIFO (created by PRoot, opened O_RDWR by the tracee). The tracee gets a
 *    real file descriptor, so dup/fork/close/poll/epoll keep working; PRoot
 *    writes one byte into the FIFO while a reply is waiting, which is what
 *    makes poll()/epoll report the socket readable. A request reaches the FIFO
 *    either through PRoot (sendto/sendmsg are intercepted and the bytes are
 *    written into it) or through a plain write() by the tracee (busybox `ip`);
 *    it is parsed and answered lazily, when the tracee receives.
 *  - bind/getsockname/[gs]etsockopt/sendto/sendmsg/recvfrom/recvmsg on such a
 *    descriptor are executed by PRoot itself (the real syscall is voided and
 *    its result and output buffers are written by us).
 *  - a descriptor is recognised by the (device, inode) of the FIFO behind
 *    /proc/<tid>/fd/<fd>, so inheritance by fork or dup needs no bookkeeping.
 *  - setsockopt(SOL_SOCKET, SO_MARK) is answered with success on any socket
 *    (Android denies it; tailscale and friends set it on every socket).
 *
 * Only 64-bit tracees are handled (the tracee is left alone otherwise).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_arp.h>

#include <talloc.h>

#include "extension/extension.h"
#include "cli/note.h"
#include "syscall/syscall.h"
#include "tracee/abi.h"
#include "tracee/mem.h"
#include "tracee/reg.h"

#define MAX_SOCKS   64
#define MAX_REQUEST (64 * 1024)
#define MAX_ITEMS   64

#define FN_IFF_UP        0x1u
#define FN_IFF_LOOPBACK  0x8u
#define FN_IFF_RUNNING   0x40u
#define FN_IFF_LOWER_UP  0x10000u

typedef struct { uint8_t *data; size_t len; } Item;

typedef struct {
	int in_use;
	dev_t dev;
	ino_t ino;
	int fd;              /* PRoot's own descriptor on the FIFO */
	uint32_t nl_pid;     /* what getsockname() reports */
	int sig;             /* readiness bytes (written by PRoot) currently at the head of the FIFO */
	Item items[MAX_ITEMS];
	int head, count;
	unsigned long age;
} FakeSock;

typedef struct { pid_t pid; int in_use; int fd; char path[256]; } Pending;

static FakeSock socks[MAX_SOCKS];
static Pending pending[MAX_SOCKS];
static unsigned long clock_ticks;
static unsigned long fifo_counter;
static int active_socks;

/* ---------- tracee memory helpers (64-bit tracees only) ---------- */

typedef struct { uint64_t base, len; } Iov64;
typedef struct {
	uint64_t name; uint32_t namelen; uint32_t pad0;
	uint64_t iov;  uint64_t iovlen;
	uint64_t control; uint64_t controllen;
	int32_t flags; int32_t pad1;
} Msg64;

static int usable(const Tracee *tracee)
{
	return sizeof(word_t) == 8 && !is_32on64_mode(tracee);
}

static void voided(Tracee *tracee, long result)
{
	poke_reg(tracee, SYSARG_RESULT, (word_t) result);
	set_sysnum(tracee, PR_void);
}

/* ---------- fake socket table ---------- */

static FakeSock *find_by_id(dev_t dev, ino_t ino)
{
	for (int i = 0; i < MAX_SOCKS; i++)
		if (socks[i].in_use && socks[i].dev == dev && socks[i].ino == ino)
			return &socks[i];
	return NULL;
}

static FakeSock *lookup(const Tracee *tracee, word_t fd)
{
	char path[64];
	struct stat st;

	if (active_socks == 0 || (int64_t) fd < 0 || fd > 1000000)
		return NULL;
	snprintf(path, sizeof path, "/proc/%d/fd/%d", (int) tracee->pid, (int) fd);
	if (stat(path, &st) < 0 || !S_ISFIFO(st.st_mode))
		return NULL;
	FakeSock *s = find_by_id(st.st_dev, st.st_ino);
	if (s)
		s->age = ++clock_ticks;
	return s;
}

static void free_items(FakeSock *s)
{
	for (int i = 0; i < s->count; i++)
		free(s->items[(s->head + i) % MAX_ITEMS].data);
	s->head = s->count = 0;
}

static void drop(FakeSock *s)
{
	free_items(s);
	if (s->fd >= 0)
		close(s->fd);
	memset(s, 0, sizeof *s);
	s->fd = -1;
	active_socks--;
}

static FakeSock *new_slot(void)
{
	FakeSock *victim = NULL;

	for (int i = 0; i < MAX_SOCKS; i++) {
		if (!socks[i].in_use) {
			victim = &socks[i];
			break;
		}
		if (!victim || socks[i].age < victim->age)
			victim = &socks[i];
	}
	if (victim->in_use)   /* table full: the least recently used one was long closed */
		drop(victim);
	return victim;
}

/*
 * The FIFO carries (a) requests the tracee wrote and (b) at most one readiness
 * byte of ours, always at its head: we only add it right after draining.
 */
static void signal_ready(FakeSock *s)
{
	char b = 1;
	if (s->fd >= 0 && s->sig == 0 && write(s->fd, &b, 1) == 1)
		s->sig = 1;
}

/* Read everything the tracee wrote (minus our own readiness byte). */
static void pull_requests(FakeSock *s, uint8_t **req, size_t *len)
{
	uint8_t *buf = malloc(MAX_REQUEST);
	size_t n = 0;

	*req = NULL;
	*len = 0;
	if (!buf || s->fd < 0) {
		free(buf);
		return;
	}
	for (;;) {
		ssize_t r = read(s->fd, buf + n, MAX_REQUEST - n);
		if (r <= 0)
			break;
		n += (size_t) r;
		if (n >= MAX_REQUEST)
			break;
	}
	size_t skip = (size_t) s->sig < n ? (size_t) s->sig : n;
	s->sig = 0;
	*req = buf;
	*len = n - skip;
	memmove(buf, buf + skip, *len);
}

/* ---------- netlink message building ---------- */

typedef struct { uint8_t *p; size_t len, cap; } Buf;

static int bput(Buf *b, const void *d, size_t n)
{
	if (b->len + n > b->cap) {
		size_t c = b->cap ? b->cap : 256;
		while (c < b->len + n)
			c *= 2;
		uint8_t *np = realloc(b->p, c);
		if (!np)
			return -1;
		b->p = np;
		b->cap = c;
	}
	if (d)
		memcpy(b->p + b->len, d, n);
	else
		memset(b->p + b->len, 0, n);
	b->len += n;
	return 0;
}

static void bpad(Buf *b)
{
	size_t pad = NLMSG_ALIGN(b->len) - b->len;
	if (pad)
		bput(b, NULL, pad);
}

static void put_attr(Buf *b, uint16_t type, const void *d, size_t n)
{
	struct nlattr a = { .nla_len = (uint16_t) (NLA_HDRLEN + n), .nla_type = type };
	bput(b, &a, sizeof a);
	bput(b, d, n);
	bpad(b);
}

static void begin_msg(Buf *b, struct nlmsghdr *h, uint16_t type, uint16_t flags, uint32_t seq, uint32_t pid)
{
	memset(h, 0, sizeof *h);
	h->nlmsg_type = type;
	h->nlmsg_flags = flags;
	h->nlmsg_seq = seq;
	h->nlmsg_pid = pid;
	b->len = 0;
	bput(b, h, sizeof *h);
}

static void end_msg(Buf *b)
{
	bpad(b);
	((struct nlmsghdr *) b->p)->nlmsg_len = (uint32_t) b->len;
}

/* Queue the finished message as one datagram item. */
static int enqueue(FakeSock *s, Buf *b)
{
	if (s->count >= MAX_ITEMS || !b->p)
		return -1;
	Item *it = &s->items[(s->head + s->count) % MAX_ITEMS];
	it->data = malloc(b->len);
	if (!it->data)
		return -1;
	memcpy(it->data, b->p, b->len);
	it->len = b->len;
	s->count++;
	return 0;
}

static void reply_link(FakeSock *s, Buf *b, uint32_t seq, uint32_t pid, uint16_t flags)
{
	struct nlmsghdr h;
	struct ifinfomsg ifi;
	uint8_t mac[6] = {0};
	uint32_t mtu = 65536, txq = 1000;
	uint8_t oper = 0;

	begin_msg(b, &h, RTM_NEWLINK, flags, seq, pid);
	memset(&ifi, 0, sizeof ifi);
	ifi.ifi_family = AF_UNSPEC;
	ifi.ifi_type = ARPHRD_LOOPBACK;
	ifi.ifi_index = 1;
	ifi.ifi_flags = FN_IFF_UP | FN_IFF_LOOPBACK | FN_IFF_RUNNING | FN_IFF_LOWER_UP;
	ifi.ifi_change = 0;
	bput(b, &ifi, sizeof ifi);
	put_attr(b, IFLA_IFNAME, "lo", 3);
	put_attr(b, IFLA_MTU, &mtu, 4);
	put_attr(b, IFLA_QDISC, "noqueue", 8);
	put_attr(b, IFLA_TXQLEN, &txq, 4);
	put_attr(b, IFLA_OPERSTATE, &oper, 1);
	put_attr(b, IFLA_ADDRESS, mac, 6);
	put_attr(b, IFLA_BROADCAST, mac, 6);
	end_msg(b);
	enqueue(s, b);
}

static void reply_addr(FakeSock *s, Buf *b, int v6, uint32_t seq, uint32_t pid, uint16_t flags)
{
	struct nlmsghdr h;
	struct ifaddrmsg ifa;
	uint8_t a4[4] = {127, 0, 0, 1};
	uint8_t a6[16] = {0};
	a6[15] = 1;

	begin_msg(b, &h, RTM_NEWADDR, flags, seq, pid);
	memset(&ifa, 0, sizeof ifa);
	ifa.ifa_family = v6 ? AF_INET6 : AF_INET;
	ifa.ifa_prefixlen = v6 ? 128 : 8;
	ifa.ifa_flags = IFA_F_PERMANENT;
	ifa.ifa_scope = RT_SCOPE_HOST;
	ifa.ifa_index = 1;
	bput(b, &ifa, sizeof ifa);
	if (v6) {
		put_attr(b, IFA_ADDRESS, a6, 16);
	} else {
		put_attr(b, IFA_ADDRESS, a4, 4);
		put_attr(b, IFA_LOCAL, a4, 4);
		put_attr(b, IFA_LABEL, "lo", 3);
	}
	end_msg(b);
	enqueue(s, b);
}

static void reply_done(FakeSock *s, Buf *b, uint32_t seq, uint32_t pid)
{
	struct nlmsghdr h;
	int32_t zero = 0;

	begin_msg(b, &h, NLMSG_DONE, NLM_F_MULTI, seq, pid);
	bput(b, &zero, 4);
	end_msg(b);
	enqueue(s, b);
}

static void reply_error(FakeSock *s, Buf *b, const struct nlmsghdr *req, int err)
{
	struct nlmsghdr h;
	int32_t e = -err;

	begin_msg(b, &h, NLMSG_ERROR, 0, req->nlmsg_seq, s->nl_pid);
	bput(b, &e, 4);
	bput(b, req, sizeof *req);
	end_msg(b);
	enqueue(s, b);
}

/* Interpret the request datagram(s) the tracee sent and queue the answers. */
static void process_request(FakeSock *s, const uint8_t *req, size_t len)
{
	Buf b = {0};
	size_t off = 0;
	int had_reply = 0;

	while (off + sizeof(struct nlmsghdr) <= len) {
		struct nlmsghdr h;
		memcpy(&h, req + off, sizeof h);
		if (h.nlmsg_len < sizeof h || off + h.nlmsg_len > len)
			break;
		uint32_t pid = h.nlmsg_pid ? h.nlmsg_pid : s->nl_pid;
		int dump = (h.nlmsg_flags & NLM_F_DUMP) == NLM_F_DUMP;
		uint8_t family = 0;
		if (h.nlmsg_len >= sizeof h + 1)
			family = req[off + sizeof h];

		if (h.nlmsg_type < RTM_BASE) {
			/* NLMSG_NOOP / control messages: nothing to answer. */
		} else if (h.nlmsg_type == RTM_GETLINK) {
			reply_link(s, &b, h.nlmsg_seq, pid, dump ? NLM_F_MULTI : 0);
			if (dump)
				reply_done(s, &b, h.nlmsg_seq, pid);
			had_reply = 1;
		} else if (h.nlmsg_type == RTM_GETADDR) {
			uint16_t f = dump ? NLM_F_MULTI : 0;
			if (family == AF_UNSPEC || family == AF_INET)
				reply_addr(s, &b, 0, h.nlmsg_seq, pid, f);
			if (family == AF_UNSPEC || family == AF_INET6)
				reply_addr(s, &b, 1, h.nlmsg_seq, pid, f);
			reply_done(s, &b, h.nlmsg_seq, pid);
			had_reply = 1;
		} else if ((h.nlmsg_type & 3) == 2 /* RTM_GET* */) {
			/* routes, neighbours, rules, ...: an empty table */
			reply_done(s, &b, h.nlmsg_seq, pid);
			had_reply = 1;
		} else {
			/* RTM_NEWxxx / RTM_DELxxx: changing the network is not allowed */
			reply_error(s, &b, &h, EPERM);
			had_reply = 1;
		}
		off += NLMSG_ALIGN(h.nlmsg_len);
	}
	free(b.p);
	(void) had_reply;
}

/* ---------- syscall handlers ---------- */

/* Gather the bytes of a send: sendto(fd, buf, len, ...) / sendmsg(fd, msg, ...). */
static ssize_t gather_send(Tracee *tracee, int is_msg, uint8_t **out)
{
	word_t a2 = peek_reg(tracee, ORIGINAL, SYSARG_2);
	word_t a3 = peek_reg(tracee, ORIGINAL, SYSARG_3);
	uint8_t *buf;
	size_t total = 0;

	if (!is_msg) {
		if (a3 > MAX_REQUEST)
			return -EMSGSIZE;
		buf = malloc(a3 ? a3 : 1);
		if (!buf)
			return -ENOMEM;
		if (a3 && read_data(tracee, buf, a2, a3) < 0) {
			free(buf);
			return -EFAULT;
		}
		*out = buf;
		return (ssize_t) a3;
	}

	Msg64 m;
	if (read_data(tracee, &m, a2, sizeof m) < 0)
		return -EFAULT;
	if (m.iovlen > 64)
		return -EMSGSIZE;
	Iov64 iov[64];
	if (m.iovlen && read_data(tracee, iov, m.iov, m.iovlen * sizeof(Iov64)) < 0)
		return -EFAULT;
	for (uint64_t i = 0; i < m.iovlen; i++)
		total += iov[i].len;
	if (total > MAX_REQUEST)
		return -EMSGSIZE;
	buf = malloc(total ? total : 1);
	if (!buf)
		return -ENOMEM;
	size_t off = 0;
	for (uint64_t i = 0; i < m.iovlen; i++) {
		if (iov[i].len && read_data(tracee, buf + off, iov[i].base, iov[i].len) < 0) {
			free(buf);
			return -EFAULT;
		}
		off += iov[i].len;
	}
	*out = buf;
	return (ssize_t) total;
}

static void do_send(Tracee *tracee, FakeSock *s, int is_msg)
{
	uint8_t *buf = NULL;
	ssize_t n = gather_send(tracee, is_msg, &buf);

	if (n < 0) {
		voided(tracee, n);
		return;
	}
	/* Parsed when the tracee receives; the bytes also make the FIFO readable. */
	if (n > 0 && s->fd >= 0 && write(s->fd, buf, (size_t) n) < 0) {
		free(buf);
		voided(tracee, -ENOBUFS);
		return;
	}
	free(buf);
	voided(tracee, n);
}

static void write_sockaddr_nl(Tracee *tracee, word_t addr, word_t lenp, uint32_t pid)
{
	struct sockaddr_nl nl;
	uint32_t have = 0;

	memset(&nl, 0, sizeof nl);
	nl.nl_family = AF_NETLINK;
	nl.nl_pid = pid;
	if (!addr)
		return;
	if (lenp && read_data(tracee, &have, lenp, sizeof have) < 0)
		return;
	if (!lenp)
		have = sizeof nl;
	write_data(tracee, addr, &nl, have < sizeof nl ? have : sizeof nl);
	if (lenp) {
		uint32_t full = sizeof nl;
		write_data(tracee, lenp, &full, sizeof full);
	}
}

static void do_recv(Tracee *tracee, FakeSock *s, int is_msg)
{
	word_t a2 = peek_reg(tracee, ORIGINAL, SYSARG_2);
	word_t a3 = peek_reg(tracee, ORIGINAL, SYSARG_3);
	word_t flags = peek_reg(tracee, ORIGINAL, SYSARG_4);
	Msg64 m;
	Iov64 iov[64];
	uint64_t iovcnt = 0;
	size_t cap = 0;
	int peek = (flags & MSG_PEEK) != 0;

	if (is_msg) {
		flags = peek_reg(tracee, ORIGINAL, SYSARG_3);
		peek = (flags & MSG_PEEK) != 0;
		if (read_data(tracee, &m, a2, sizeof m) < 0 || m.iovlen > 64) {
			voided(tracee, -EFAULT);
			return;
		}
		iovcnt = m.iovlen;
		if (iovcnt && read_data(tracee, iov, m.iov, iovcnt * sizeof(Iov64)) < 0) {
			voided(tracee, -EFAULT);
			return;
		}
		for (uint64_t i = 0; i < iovcnt; i++)
			cap += iov[i].len;
	} else {
		cap = a3;
	}

	{
		uint8_t *req;
		size_t rlen;
		pull_requests(s, &req, &rlen);
		if (req && rlen)
			process_request(s, req, rlen);
		free(req);
	}
	if (s->count == 0) {
		voided(tracee, -EAGAIN);
		return;
	}

	/* Pack as many whole messages as fit; the first one is cut if it does not. */
	uint8_t *out = malloc(cap ? cap : 1);
	if (!out) {
		voided(tracee, -ENOMEM);
		return;
	}
	size_t used = 0, full = s->items[s->head].len;
	int taken = 0, truncated = 0;
	for (int i = 0; i < s->count; i++) {
		Item *it = &s->items[(s->head + i) % MAX_ITEMS];
		if (used + it->len > cap) {
			if (i == 0) {
				memcpy(out, it->data, cap);
				used = cap;
				taken = 1;
				truncated = it->len > cap;
			}
			break;
		}
		memcpy(out + used, it->data, it->len);
		used += it->len;
		taken++;
	}

	/* Copy out to the tracee buffer(s). */
	if (is_msg) {
		size_t off = 0;
		for (uint64_t i = 0; i < iovcnt && off < used; i++) {
			size_t n = iov[i].len < used - off ? iov[i].len : used - off;
			if (n)
				write_data(tracee, iov[i].base, out + off, n);
			off += n;
		}
		if (m.name && m.namelen >= sizeof(struct sockaddr_nl)) {
			struct sockaddr_nl nl = { .nl_family = AF_NETLINK };
			write_data(tracee, m.name, &nl, sizeof nl);
		}
		uint32_t nlen = sizeof(struct sockaddr_nl);
		m.namelen = m.name ? nlen : 0;
		m.controllen = 0;
		m.flags = truncated ? MSG_TRUNC : 0;
		write_data(tracee, a2, &m, sizeof m);
	} else {
		if (used)
			write_data(tracee, a2, out, used);
		word_t addr = peek_reg(tracee, ORIGINAL, SYSARG_5);
		word_t lenp = peek_reg(tracee, ORIGINAL, SYSARG_6);
		write_sockaddr_nl(tracee, addr, lenp, 0);
	}
	free(out);

	if (!peek) {
		for (int i = 0; i < taken; i++) {
			free(s->items[s->head].data);
			s->items[s->head].data = NULL;
			s->head = (s->head + 1) % MAX_ITEMS;
			s->count--;
		}
	}
	if (s->count > 0)
		signal_ready(s);   /* more replies waiting: stay readable */
	voided(tracee, (flags & MSG_TRUNC) && truncated ? (long) full : (long) used);
}

/*
 * Create the FIFO that stands in for the netlink socket. PROOT_TMP_DIR is
 * tried first (the app points it at its private directory), then TMPDIR,
 * /tmp and /data/local/tmp; the directory that worked is remembered. When
 * none works the real syscall runs unchanged (Android then refuses it as
 * before) and one warning explains why.
 */
static int make_fifo(Tracee *tracee, Pending *slot)
{
	static const char *cached;
	static int warned;
	const char *cands[5] = { cached, getenv("PROOT_TMP_DIR"), getenv("TMPDIR"), "/tmp", "/data/local/tmp" };
	int last_errno = 0;

	for (int i = 0; i < 5; i++) {
		const char *dir = cands[i];
		if (!dir || !*dir)
			continue;
		snprintf(slot->path, sizeof slot->path, "%s/.magk-nl-%d-%lu", dir, (int) getpid(), ++fifo_counter);
		unlink(slot->path);
		if (mkfifo(slot->path, 0600) < 0) {
			last_errno = errno;
			continue;
		}
		slot->fd = open(slot->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (slot->fd < 0) {
			last_errno = errno;
			unlink(slot->path);
			continue;
		}
		cached = dir;
		return 0;
	}
	if (!warned) {
		warned = 1;
		note(tracee, WARNING, USER,
		     "[fake_netlink] cannot create a FIFO in PROOT_TMP_DIR, TMPDIR, /tmp or /data/local/tmp (%s): "
		     "network interface listing will fail", strerror(last_errno ? last_errno : EACCES));
	}
	return -1;
}

static void do_socket(Tracee *tracee)
{
	word_t type = peek_reg(tracee, ORIGINAL, SYSARG_2);
	Pending *slot = NULL;

	for (int i = 0; i < MAX_SOCKS; i++)
		if (!pending[i].in_use) { slot = &pending[i]; break; }
	if (!slot)
		return;   /* let the kernel refuse as usual */

	if (make_fifo(tracee, slot) < 0)
		return;
	slot->pid = tracee->pid;
	slot->in_use = 1;

	/* socket(AF_NETLINK, type, NETLINK_ROUTE) -> openat(AT_FDCWD, fifo, O_RDWR|...) */
	word_t p = alloc_mem(tracee, (ssize_t) strlen(slot->path) + 1);
	if ((int64_t) p < 0 || write_data(tracee, p, slot->path, strlen(slot->path) + 1) < 0) {
		close(slot->fd);
		unlink(slot->path);
		slot->in_use = 0;
		return;
	}
	word_t oflags = O_RDWR;
	if (type & SOCK_CLOEXEC)
		oflags |= O_CLOEXEC;
	if (type & SOCK_NONBLOCK)
		oflags |= O_NONBLOCK;
	set_sysnum(tracee, PR_openat);
	poke_reg(tracee, SYSARG_1, (word_t) -100 /* AT_FDCWD */);
	poke_reg(tracee, SYSARG_2, p);
	poke_reg(tracee, SYSARG_3, oflags);
	poke_reg(tracee, SYSARG_4, 0);
}

static void finish_socket(Tracee *tracee)
{
	Pending *slot = NULL;
	long res = (long) peek_reg(tracee, CURRENT, SYSARG_RESULT);

	for (int i = 0; i < MAX_SOCKS; i++)
		if (pending[i].in_use && pending[i].pid == tracee->pid) { slot = &pending[i]; break; }
	if (!slot)
		return;
	struct stat st;
	if (res >= 0 && fstat(slot->fd, &st) == 0) {
		FakeSock *s = new_slot();
		memset(s, 0, sizeof *s);
		s->in_use = 1;
		s->dev = st.st_dev;
		s->ino = st.st_ino;
		s->fd = slot->fd;
		s->nl_pid = (uint32_t) tracee->pid;
		s->age = ++clock_ticks;
		active_socks++;
	} else {
		close(slot->fd);
	}
	unlink(slot->path);
	slot->in_use = 0;
}

int fake_netlink_callback(Extension *extension, ExtensionEvent event, intptr_t data1 UNUSED, intptr_t data2 UNUSED)
{
	switch (event) {
	case INITIALIZATION: {
		static FilteredSysnum filtered_sysnums[] = {
			{ PR_socket,     0 },
			{ PR_bind,       0 },
			{ PR_getsockname, 0 },
			{ PR_setsockopt, 0 },
			{ PR_getsockopt, 0 },
			{ PR_sendto,     0 },
			{ PR_sendmsg,    0 },
			{ PR_recvfrom,   0 },
			{ PR_recvmsg,    0 },
			FILTERED_SYSNUM_END
		};
		extension->filtered_sysnums = filtered_sysnums;
		Tracee *self = TRACEE(extension);
		VERBOSE(self, 1, "[fake_netlink] initialized (rtnetlink -> loopback only; SO_MARK -> no-op)");
		return 0;
	}

	case INHERIT_PARENT:
		return 0;   /* shared: the tables are global to the tracer */

	case SYSCALL_ENTER_END: {
		Tracee *tracee = TRACEE(extension);
		FakeSock *s;

		if (!usable(tracee))
			return 0;
		switch (get_sysnum(tracee, ORIGINAL)) {
		case PR_socket:
			if (peek_reg(tracee, ORIGINAL, SYSARG_1) == AF_NETLINK &&
			    (int) peek_reg(tracee, ORIGINAL, SYSARG_3) == NETLINK_ROUTE)
				do_socket(tracee);
			return 0;
		case PR_setsockopt:
			/* SO_MARK is denied to apps; every socket gets "success". */
			if ((int) peek_reg(tracee, ORIGINAL, SYSARG_2) == SOL_SOCKET &&
			    (int) peek_reg(tracee, ORIGINAL, SYSARG_3) == SO_MARK) {
				voided(tracee, 0);
				return 0;
			}
			if ((s = lookup(tracee, peek_reg(tracee, ORIGINAL, SYSARG_1))))
				voided(tracee, 0);
			return 0;
		case PR_bind:
			if ((s = lookup(tracee, peek_reg(tracee, ORIGINAL, SYSARG_1))))
				voided(tracee, 0);
			return 0;
		case PR_getsockname:
			if ((s = lookup(tracee, peek_reg(tracee, ORIGINAL, SYSARG_1)))) {
				write_sockaddr_nl(tracee, peek_reg(tracee, ORIGINAL, SYSARG_2),
						  peek_reg(tracee, ORIGINAL, SYSARG_3), s->nl_pid);
				voided(tracee, 0);
			}
			return 0;
		case PR_getsockopt:
			if ((s = lookup(tracee, peek_reg(tracee, ORIGINAL, SYSARG_1)))) {
				word_t optval = peek_reg(tracee, ORIGINAL, SYSARG_4);
				word_t optlen = peek_reg(tracee, ORIGINAL, SYSARG_5);
				uint32_t n = 0;
				if (optval && optlen && read_data(tracee, &n, optlen, sizeof n) == 0 && n <= 256) {
					uint8_t zero[256] = {0};
					write_data(tracee, optval, zero, n);
				}
				voided(tracee, 0);
			}
			return 0;
		case PR_sendto:
		case PR_sendmsg:
			if ((s = lookup(tracee, peek_reg(tracee, ORIGINAL, SYSARG_1))))
				do_send(tracee, s, get_sysnum(tracee, ORIGINAL) == PR_sendmsg);
			return 0;
		case PR_recvfrom:
		case PR_recvmsg:
			if ((s = lookup(tracee, peek_reg(tracee, ORIGINAL, SYSARG_1))))
				do_recv(tracee, s, get_sysnum(tracee, ORIGINAL) == PR_recvmsg);
			return 0;
		default:
			return 0;
		}
	}

	case SYSCALL_EXIT_END: {
		Tracee *tracee = TRACEE(extension);
		if (usable(tracee) && get_sysnum(tracee, ORIGINAL) == PR_socket)
			finish_socket(tracee);
		return 0;
	}

	default:
		return 0;
	}
}
