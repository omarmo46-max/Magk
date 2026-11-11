/*
 * magk-offload: guest-side client of the Magk native-offload service.
 *
 * Stock PRoot cannot intercept execve("<handler>") (that needs a patched
 * proot with --native-offload). This client gives the same result with any
 * PRoot: the /usr/local/bin/<handler> stub execs
 *
 *     magk-offload <handler> [args...]
 *
 * which sends argv/env/cwd to the app's NativeOffloadServer over its abstract
 * unix socket (name in $MAGK_OFFLOAD_SOCKET), then prints the handler output
 * (a file in the guest's /tmp) and exits with the handler's exit code.
 *
 * Wire format (little-endian), see NativeOffload.kt:
 *   request : 'NOFF' u32 | version=1 u32 | pid u32 | argc u32 | argv strings |
 *             envc u32 | "K=V" strings | cwd string        (string = u32 len + bytes)
 *   response: 'NOFR' u32 | exit_code i32 | guest tmpfile path string
 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAGIC_REQ 0x46464F4Eu
#define MAGIC_RSP 0x52464F4Eu
#define EX_UNAVAILABLE 69
#define MAX_STR (1u << 20)

extern char **environ;

static const char *g_name = "magk-offload";

static int fail(const char *msg) {
    fprintf(stderr, "%s: unavailable - %s\n", g_name, msg);
    return EX_UNAVAILABLE;
}

typedef struct { unsigned char *p; size_t len, cap; } buf_t;

static int b_put(buf_t *b, const void *d, size_t n) {
    if (b->len + n > b->cap) {
        size_t c = b->cap ? b->cap : 4096;
        while (c < b->len + n) c *= 2;
        unsigned char *np = realloc(b->p, c);
        if (!np) return -1;
        b->p = np; b->cap = c;
    }
    memcpy(b->p + b->len, d, n);
    b->len += n;
    return 0;
}
static int b_u32(buf_t *b, uint32_t v) {
    unsigned char t[4] = { v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255 };
    return b_put(b, t, 4);
}
static int b_str(buf_t *b, const char *s) {
    size_t n = strlen(s);
    if (n > MAX_STR) n = MAX_STR;
    return b_u32(b, (uint32_t)n) || (n ? b_put(b, s, n) : 0);
}

static int read_all(int fd, void *dst, size_t n) {
    unsigned char *p = dst;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int write_all(int fd, const void *src, size_t n) {
    const unsigned char *p = src;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= (size_t)r;
    }
    return 0;
}
static int read_u32(int fd, uint32_t *v) {
    unsigned char t[4];
    if (read_all(fd, t, 4)) return -1;
    *v = (uint32_t)t[0] | ((uint32_t)t[1] << 8) | ((uint32_t)t[2] << 16) | ((uint32_t)t[3] << 24);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: magk-offload <handler> [args...]\n"); return 64; }
    g_name = argv[1];

    const char *sock = getenv("MAGK_OFFLOAD_SOCKET");
    if (!sock || !*sock) return fail("MAGK_OFFLOAD_SOCKET is not set; restart the Magk app and try again.");
    size_t slen = strlen(sock);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (slen + 1 > sizeof addr.sun_path) return fail("socket name too long");
    memcpy(addr.sun_path + 1, sock, slen);  /* abstract namespace: leading NUL */
    socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + slen);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return fail("cannot create socket");
    if (connect(fd, (struct sockaddr *)&addr, alen) < 0)
        return fail("Magk could not be reached. Restart the Magk app and try again.");

    buf_t b = {0};
    int rc = 0;
    uint32_t nenv = 0;
    for (char **e = environ; e && *e; e++) nenv++;
    if (nenv > 4096) nenv = 4096;
    uint32_t nargs = (uint32_t)(argc - 1);
    if (nargs > 256) return fail("too many arguments");

    rc |= b_u32(&b, MAGIC_REQ) | b_u32(&b, 1) | b_u32(&b, (uint32_t)getpid()) | b_u32(&b, nargs);
    for (int i = 1; i < argc; i++) rc |= b_str(&b, argv[i]);
    rc |= b_u32(&b, nenv);
    for (uint32_t i = 0; i < nenv; i++) rc |= b_str(&b, environ[i]);
    char cwd[4096];
    if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "/");
    rc |= b_str(&b, cwd);
    if (rc) return fail("out of memory");
    if (write_all(fd, b.p, b.len)) return fail("request could not be sent");

    uint32_t magic, plen;
    int32_t code;
    if (read_u32(fd, &magic) || magic != MAGIC_RSP) return fail("bad response from Magk");
    if (read_u32(fd, (uint32_t *)&code) || read_u32(fd, &plen) || plen == 0 || plen > 4096)
        return fail("bad response from Magk");
    char path[4097];
    if (read_all(fd, path, plen)) return fail("truncated response from Magk");
    path[plen] = 0;
    close(fd);

    int out = open(path, O_RDONLY);
    if (out >= 0) {
        char chunk[16384];
        ssize_t r;
        while ((r = read(out, chunk, sizeof chunk)) > 0) {
            if (write_all(1, chunk, (size_t)r)) break;
        }
        close(out);
        unlink(path);  /* the server also sweeps stale reply files */
    }
    return code & 0xff ? (code & 0xff) : (code != 0 ? 1 : 0);
}
