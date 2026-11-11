#!/usr/bin/env bash
# Builds the guest-side offload client (deps/magk-offload-client.c) as a static
# arm64 executable and installs it as jniLibs/arm64-v8a/libmagk_offload.so.
# PRootKernel copies it to /usr/local/libexec/magk-offload inside the rootfs;
# the /usr/local/bin/<handler> stubs run it so the magk-* tools work with a
# stock PRoot. Needs ANDROID_NDK_HOME.
set -euo pipefail
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to the Android NDK directory}"
API="${ANDROID_API:-26}"
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(dirname "$HERE")/android/app/src/main/jniLibs/arm64-v8a"
TC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin"
mkdir -p "$OUT"
"$TC/aarch64-linux-android${API}-clang" -O2 -Wall -static \
  "$HERE/magk-offload-client.c" -o "$OUT/libmagk_offload.so"
"$TC/llvm-strip" "$OUT/libmagk_offload.so"
echo "installed $OUT/libmagk_offload.so"
