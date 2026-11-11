#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <pthread.h>

static int dump(int fd, int want_poll, const char *tag) {
  struct { struct nlmsghdr h; struct rtgenmsg g; } req = {0};
  req.h.nlmsg_len = sizeof req; req.h.nlmsg_type = RTM_GETLINK; req.h.nlmsg_flags = NLM_F_REQUEST|NLM_F_DUMP; req.h.nlmsg_seq = 7; req.g.rtgen_family = AF_UNSPEC;
  struct iovec iv = { &req, sizeof req }; struct sockaddr_nl to = { .nl_family = AF_NETLINK };
  struct msghdr m = { .msg_name=&to, .msg_namelen=sizeof to, .msg_iov=&iv, .msg_iovlen=1 };
  if (sendmsg(fd, &m, 0) < 0) { perror("sendmsg"); return -1; }
  int links = 0, done = 0;
  while (!done) {
    if (want_poll == 1) { struct pollfd p = { fd, POLLIN, 0 }; int r = poll(&p, 1, 2000); if (r != 1) { printf("%s: poll=%d (not readable!)\n", tag, r); return -1; } }
    if (want_poll == 2) { int ep = epoll_create1(0); struct epoll_event e = { .events = EPOLLIN }, o; epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e); int r = epoll_wait(ep, &o, 1, 2000); close(ep); if (r != 1) { printf("%s: epoll=%d (not readable!)\n", tag, r); return -1; } }
    char buf[256]; struct iovec rv = { buf, sizeof buf }; struct sockaddr_nl from; /* tiny buffer: forces several recvs */
    struct msghdr rm = { .msg_name=&from, .msg_namelen=sizeof from, .msg_iov=&rv, .msg_iovlen=1 };
    ssize_t n = recvmsg(fd, &rm, 0);
    if (n < 0) { printf("%s: recvmsg errno=%d\n", tag, errno); return -1; }
    for (struct nlmsghdr *h = (void*)buf; NLMSG_OK(h, (unsigned)n); h = NLMSG_NEXT(h, n)) {
      if (h->nlmsg_type == NLMSG_DONE) done = 1; else if (h->nlmsg_type == RTM_NEWLINK) links++;
    }
  }
  printf("%s: ok links=%d\n", tag, links); return 0;
}
static int mk(int flags) { int fd = socket(AF_NETLINK, SOCK_RAW|flags, NETLINK_ROUTE); struct sockaddr_nl a = { .nl_family = AF_NETLINK }; bind(fd, (void*)&a, sizeof a); return fd; }
static void *thr(void *arg) { dump(*(int*)arg, 0, "thread(shared fd)"); return 0; }
int main(void) {
  int fd = mk(SOCK_CLOEXEC); dump(fd, 0, "blocking"); close(fd);
  fd = mk(SOCK_NONBLOCK); dump(fd, 1, "nonblock+poll"); close(fd);
  fd = mk(SOCK_NONBLOCK); dump(fd, 2, "nonblock+epoll"); close(fd);
  fd = mk(0); int d = dup(fd); dump(d, 0, "dup'd fd"); close(d); close(fd);
  fd = mk(0); if (fork() == 0) { dump(fd, 0, "forked child"); fflush(stdout); _exit(0); } wait(NULL); close(fd);
  fd = mk(0); pthread_t t; pthread_create(&t, 0, thr, &fd); pthread_join(t, 0); close(fd);
  int s = socket(AF_INET, SOCK_DGRAM, 0); int mark = 5; printf("SO_MARK on UDP socket: rc=%d\n", setsockopt(s, SOL_SOCKET, SO_MARK, &mark, sizeof mark));
  return 0;
}
