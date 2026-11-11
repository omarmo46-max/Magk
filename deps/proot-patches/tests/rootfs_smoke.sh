#!/usr/bin/env bash
# End-to-end smoke test on the REAL Alpine aarch64 rootfs (the one the app ships), run on an
# x86_64 Linux box through qemu-user: boots the rootfs under the patched PRoot and checks
#   * the shell and apk work
#   * `ip addr` lists only `lo` (--fake-netlink)
#   * a handler stub -> magk-offload client (static musl aarch64) -> app service round trip
#   * magk-doctor passes
#
# Needs: gcc make git libtalloc-dev qemu-user-static curl python3 + `pip install ziglang`
# Set ALPINE_TAR=/path/to/alpine-minirootfs.tar(.gz) to skip the download.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
W="$(mktemp -d)"; trap 'kill $SRV 2>/dev/null || true; rm -rf "$W"' EXIT
TAG="${PROOT_TAG:-v5.1.107.102}"
VER="${ALPINE_VERSION:-3.21.3}"

git clone -q --depth 1 --branch "$TAG" https://github.com/termux/proot.git "$W/proot" 2>/dev/null
"$HERE/../apply.sh" "$W/proot" >/dev/null
make -C "$W/proot/src" -j"$(nproc)" >/dev/null 2>&1
PROOT="$W/proot/src/proot"

R="$W/rootfs"; mkdir -p "$R"
if [ -n "${ALPINE_TAR:-}" ]; then tar -xf "$ALPINE_TAR" -C "$R"
else curl -fsSL "https://dl-cdn.alpinelinux.org/alpine/v${VER%.*}/releases/aarch64/alpine-minirootfs-$VER-aarch64.tar.gz" | tar -xz -C "$R"; fi

mkdir -p "$R/usr/local/libexec" "$R/usr/local/bin" "$R/tmp"
python3 -m ziglang cc -O2 -static -target aarch64-linux-musl "$REPO/deps/magk-offload-client.c" -o "$R/usr/local/libexec/magk-offload"
cp "$REPO/android/app/src/main/assets/default_mount/usr/local/bin/magk-doctor" "$R/usr/local/bin/magk-doctor"
# Same text as HandlerStub.content("magk-config") in the app.
cat > "$R/usr/local/bin/magk-config" <<'STUB'
#!/bin/sh
if [ -x /usr/local/libexec/magk-offload ]; then
    exec /usr/local/libexec/magk-offload "magk-config" "$@"
fi
echo "magk-config: unavailable - the Magk offload client is missing from this build. Update or reinstall Magk." >&2
exit 69
STUB
chmod 755 "$R"/usr/local/bin/* "$R/usr/local/libexec/magk-offload"

NAME="native-offload-smoke-$$"
python3 "$HERE/mock_offload_server.py" --name "$NAME" --tmpdir "$R/tmp" --count 3 >"$W/server.log" 2>&1 & SRV=$!
for _ in $(seq 1 50); do grep -q ready "$W/server.log" && break; sleep 0.1; done

export PROOT_TMP_DIR="$R/tmp"
run() { timeout 120 "$PROOT" --fake-netlink -0 -q qemu-aarch64-static -r "$R" -b /proc -b /dev -w /root \
        /usr/bin/env MAGK_OFFLOAD_SOCKET="$NAME" MAGK_CHAT_SESSION_ID=smoke "$@" 2>&1 | grep -v '^proot '; }

fail=0
check() { if grep -q -- "$2" <<<"$3"; then echo "ok:   $1"; else echo "FAIL: $1 (expected: $2)"; echo "$3" | head -6; fail=1; fi; }

check "rootfs boots (Alpine $VER, aarch64)" "$VER" "$(run /bin/sh -c 'cat /etc/alpine-release; uname -m')"
check "apk works"                           "apk-tools" "$(run /sbin/apk --version)"
ipout="$(run /sbin/ip addr)"
check "ip addr shows lo"                    "inet 127.0.0.1/8" "$ipout"
if grep -q "eth0\|ifb0" <<<"$ipout"; then echo "FAIL: ip addr still shows real interfaces"; fail=1; else echo "ok:   ip addr shows only the fake lo"; fi
check "handler stub -> client -> app"       "handler=magk-config args=\['status', '--json'\] cwd=/root session=smoke" "$(run /bin/sh -c 'magk-config status --json')"
check "magk-doctor passes"                  "All checks passed." "$(run /bin/sh -c 'magk-doctor')"
[ -z "$(ls -A "$R/tmp" | grep '^\.magk-nl-' || true)" ] && echo "ok:   no FIFOs left behind" || { echo "FAIL: FIFOs left behind"; fail=1; }
exit $fail
