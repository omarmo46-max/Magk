#!/usr/bin/env bash
# Builds the guest-side offload client (deps/magk-offload-client.c) as a static
# aarch64 *musl* executable and installs it as jniLibs/arm64-v8a/libmagk_offload.so.
# The sandbox is Alpine (musl), so a static musl binary is the natural fit: it
# needs nothing from the guest and nothing from Android. PRootKernel copies it
# to /usr/local/libexec/magk-offload inside the rootfs; the /usr/local/bin/<handler>
# stubs run it so the magk-* tools work with any PRoot.
# Needs python3 + pip:  pip install ziglang   (zig ships clang and musl)
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(dirname "$HERE")/android/app/src/main/jniLibs/arm64-v8a"
mkdir -p "$OUT"
python3 -m ziglang cc -O2 -Wall -static -target aarch64-linux-musl \
  -Wl,-z,max-page-size=16384 \
  "$HERE/magk-offload-client.c" -o "$OUT/libmagk_offload.so"
echo "installed $OUT/libmagk_offload.so"
