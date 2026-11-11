#!/usr/bin/env bash
# Builds PRoot (termux/proot, GPLv2, plus deps/proot-patches) for Android arm64 and installs it as
# android/app/src/main/jniLibs/arm64-v8a/libproot.so — the file
# RootfsManager.prootBinary points at. talloc is linked statically, and the
# PRoot loader is embedded in the binary (PRootKernel.kt relies on both).
#
# Needs: ANDROID_NDK_HOME (r27c is what CI uses), git, make, curl, python3.
# Usage:  ANDROID_NDK_HOME=/path/to/ndk ./deps/build_proot.sh
#
# NOTE: written without access to an NDK, so it has not been run end-to-end.
# If a step fails, the error output of that step is the thing to fix first.
set -euo pipefail

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the Android NDK directory}"
API="${ANDROID_API:-26}"
TALLOC_VERSION="${TALLOC_VERSION:-2.4.2}"
PROOT_TAG="${PROOT_TAG:-v5.1.107.102}"
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(dirname "$HERE")"
OUT_DIR="$REPO/android/app/src/main/jniLibs/arm64-v8a"
WORK="$HERE/build/proot"
TC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
TARGET=aarch64-linux-android

export CC="$TC/${TARGET}${API}-clang"
export AR="$TC/llvm-ar"
export RANLIB="$TC/llvm-ranlib"
export STRIP="$TC/llvm-strip"
export OBJCOPY="$TC/llvm-objcopy"
export OBJDUMP="$TC/llvm-objdump"
[ -x "$CC" ] || { echo "clang not found at $CC (check ANDROID_NDK_HOME / ANDROID_API)"; exit 1; }

PREFIX="$WORK/prefix"
mkdir -p "$WORK" "$PREFIX" "$OUT_DIR"
cd "$WORK"

# ---- 1. talloc (static) ------------------------------------------------------
if [ ! -f "$PREFIX/lib/libtalloc.a" ]; then
  [ -d "talloc-$TALLOC_VERSION" ] || {
    curl -fsSL "https://download.samba.org/pub/talloc/talloc-$TALLOC_VERSION.tar.gz" | tar xz
  }
  cat > cross-answers.txt <<'ANS'
Checking uname sysname type: "Linux"
Checking uname machine type: "dontcare"
Checking uname release type: "dontcare"
Checking uname version type: "dontcare"
Checking simple C program: OK
building library support: OK
Checking for large file support: OK
Checking for -D_FILE_OFFSET_BITS=64: OK
Checking for WORDS_BIGENDIAN: OK
Checking for C99 vsnprintf: OK
Checking for HAVE_SECURE_MKSTEMP: OK
rpath library support: OK
-Wl,--version-script support: FAIL
Checking correct behavior of strtoll: OK
Checking correct behavior of strptime: OK
Checking for HAVE_IFACE_GETIFADDRS: OK
Checking for HAVE_IFACE_IFCONF: OK
Checking for HAVE_IFACE_IFREQ: OK
Checking getconf LFS_CFLAGS: OK
Checking for large file support without additional flags: OK
Checking for working strptime: OK
Checking for HAVE_SHARED_MMAP: OK
Checking for HAVE_MREMAP: OK
Checking for HAVE_INCOHERENT_MMAP: OK
Checking getconf large file support flags work: OK
ANS
  ( cd "talloc-$TALLOC_VERSION"
    CFLAGS="-fPIC -O2" ./configure \
      --prefix="$PREFIX" --disable-rpath --disable-python \
      --cross-compile --cross-answers="$WORK/cross-answers.txt" \
      --bundled-libraries=NONE --builtin-libraries=replace
    # waf may fail while linking the shared lib (we do not need it); we only
    # need the compiled objects, which we archive into a static libtalloc.a.
    make -j"$(nproc)" || true
    mkdir -p "$PREFIX/lib" "$PREFIX/include"
    OBJS=$(find bin -name '*.o' ! -name '*test*' ! -name '*pytalloc*' | sort)
    [ -n "$OBJS" ] || { echo "talloc: no object files were built"; exit 1; }
    "$AR" rcs "$PREFIX/lib/libtalloc.a" $OBJS
    cp talloc.h "$PREFIX/include/"
  )
fi

# ---- 2. PRoot ----------------------------------------------------------------
[ -d proot ] || git clone --depth 1 --branch "$PROOT_TAG" https://github.com/termux/proot.git proot
# Magk extensions (--fake-netlink: rtnetlink answered inside PRoot, loopback only).
"$HERE/proot-patches/apply.sh" "$WORK/proot"
cd proot/src
make clean >/dev/null 2>&1 || true
# clang 18 rejects the implicit strcmp/memset in this Termux extension.
grep -q '<string.h>' extension/ashmem_memfd/ashmem_memfd.c || sed -i '1i #include <string.h>' extension/ashmem_memfd/ashmem_memfd.c
CPPFLAGS="-I$PREFIX/include -DARG_MAX=131072 -DVERSION=\\\"${PROOT_TAG#v}\\\"" \
CFLAGS="-fPIC -O2 -Wno-error -Wno-implicit-function-declaration" \
LDFLAGS="-L$PREFIX/lib" \
  make V=1 proot

"$STRIP" proot
install -m 0755 proot "$OUT_DIR/libproot.so"
echo "installed $OUT_DIR/libproot.so"
