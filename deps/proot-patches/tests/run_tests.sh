#!/usr/bin/env bash
# Regression test for the --fake-netlink PRoot extension, run on a normal
# x86_64 Linux box (ptrace must be allowed). It builds termux/proot with the
# Magk patch applied and runs, under it, a static Go program, busybox `ip` and
# a C program that exercises poll/epoll/dup/fork/threads.
#
# Needs: gcc make git libtalloc-dev busybox-static golang-go
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
TAG="${PROOT_TAG:-v5.1.107.102}"

git clone -q --depth 1 --branch "$TAG" https://github.com/termux/proot.git "$W/proot" 2>/dev/null
"$HERE/../apply.sh" "$W/proot" >/dev/null
make -C "$W/proot/src" -j"$(nproc)" >/dev/null 2>&1
PROOT="$W/proot/src/proot"

R="$W/rootfs"; mkdir -p "$R/bin" "$R/tmp"
cp "$(command -v busybox)" "$R/bin/busybox"; ln -s busybox "$R/bin/ip"
gcc -O2 -static -pthread "$HERE/nltest.c" -o "$R/bin/nltest"
CGO_ENABLED=0 go build -o "$R/bin/netcheck" "$HERE/netcheck.go"
export PROOT_TMP_DIR="$R/tmp"
run() { timeout 60 "$PROOT" --fake-netlink -r "$R" "$@" 2>&1 | grep -v '^proot '; }

fail=0
check() { if ! grep -q "$2" <<<"$3"; then echo "FAIL: $1 (expected: $2)"; echo "$3" | head -5; fail=1; else echo "ok:   $1"; fi; }

check "static Go sees only lo"          "300x net.Interfaces: bad = 0" "$(run /bin/netcheck)"
check "ordinary TCP/HTTP still works"   "http over loopback: hello-tcp" "$(run /bin/netcheck)"
check "ordinary UDP still works"        "udp echo: ping" "$(run /bin/netcheck)"
check "busybox ip addr shows lo"        "inet 127.0.0.1/8 scope host lo" "$(run /bin/ip addr)"
check "busybox ip addr shows ::1"       "inet6 ::1/128" "$(run /bin/ip addr)"
out="$(run /bin/nltest)"
for t in "blocking: ok" "nonblock+poll: ok" "nonblock+epoll: ok" "dup'd fd: ok" "forked child: ok" "thread(shared fd): ok" "SO_MARK on UDP socket: rc=0"; do
  check "nltest $t" "$t" "$out"
done
[ -z "$(ls -A "$R/tmp")" ] && echo "ok:   no FIFOs left behind" || { echo "FAIL: FIFOs left in $R/tmp"; fail=1; }
exit $fail
