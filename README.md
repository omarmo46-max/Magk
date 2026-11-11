# Magk.ai

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Android-3DDC84.svg)](#building-from-source)
[![Android build](https://github.com/omarmo46-max/Magk/actions/workflows/android-build.yml/badge.svg)](https://github.com/omarmo46-max/Magk/actions/workflows/android-build.yml)
[![PRoot patches](https://github.com/omarmo46-max/Magk/actions/workflows/proot-patches.yml/badge.svg)](https://github.com/omarmo46-max/Magk/actions/workflows/proot-patches.yml)
[![Latest build](https://img.shields.io/github/v/release/omarmo46-max/Magk?include_prereleases&label=latest%20build)](https://github.com/omarmo46-max/Magk/releases)

**Your private, on-device AI agent for Android.**

Magk.ai brings leading models — Claude, GPT, Gemini and more — into a native
Android app, and gives them a real computer to work with: a full Linux shell
running on-device, browser automation, extensible skills, persistent memory,
and deep system integration.

It is free and fully open source, licensed under the GNU GPL v3.

Arabic is the app's default language; every other language the app ships
with is still one tap away in Settings → Appearance → Language.

---

## What it does

| | |
|---|---|
| **Bring your own model** | Claude, GPT, Gemini and other providers, via your own API keys or account sign-in. |
| **A real Linux shell** | A sandboxed Alpine Linux environment runs on-device — the agent can install packages, run scripts, and work with real files. |
| **Device integration** | Calendar, Contacts, Clipboard, Media, Alarms, notifications and more, exposed to the agent as tools. |
| **Browser automation** | The agent can browse and interact with the web on your behalf. |
| **Skills & memory** | Extensible skills plus persistent memory across sessions. |
| **MCP integrations** | Connect any Model Context Protocol server (Claude Desktop–compatible config). |
| **Native offloads** | Heavy or platform-specific work is handed to native code instead of the sandbox. |

---

## Building from source

This repository ships a Linux sandbox inside the app, so the native
dependencies (PRoot, and the rclone + Tailscale/tsnet gomobile library) and
the Alpine rootfs are **built from source** rather than committed as
binaries.

### Easiest: let GitHub Actions do it

Push this repo to GitHub and the included workflow
(`.github/workflows/android-build.yml`) builds `rclone.aar`, PRoot and the guest
client, then assembles a debug APK on every push. Builds from `main` are
published as a pre-release (`0-beta-build<N>`) on the
[Releases](https://github.com/omarmo46-max/Magk/releases) page. You can also trigger it by
hand from the Actions tab (`workflow_dispatch`).

### Building locally

```sh
git clone https://github.com/omarmo46-max/Magk.git
cd Magk

# 1. Build the native rclone + Tailscale library (needs Go — version is
#    pinned in deps/rclone-mobile/go.mod — the Android NDK via
#    ANDROID_NDK_HOME or ANDROID_HOME, and gomobile):
go install golang.org/x/mobile/cmd/gomobile@latest
go install golang.org/x/mobile/cmd/gobind@latest
gomobile init
./deps/build_rclone_android.sh
mkdir -p android/app/libs
cp deps/build/rclone/rclone.aar android/app/libs/

# 1b. Build PRoot (the Linux sandbox's user-space chroot) into jniLibs:
ANDROID_NDK_HOME=/path/to/ndk ./deps/build_proot.sh
#     (applies deps/proot-patches: --fake-netlink; regression test: deps/proot-patches/tests/run_tests.sh)
pip install ziglang && ./deps/build_offload_client.sh       # guest client for the magk-* tools (static musl)
#     (the Alpine rootfs is downloaded + checksum-verified by Gradle itself
#      via the fetchAlpineRootfs task, so there is nothing to do for it)

# 2. Build the app
cd android
./gradlew :app:assembleDebug
```

Requirements: Android Studio / an Android SDK with API 36, and the NDK
version pinned in `android/app/build.gradle.kts` (the CI workflow uses NDK
r27c as a known-good default since none is pinned there).

Skipping step 1 still produces a working build — Gradle will just fail to
resolve `rclone.aar` as a dependency, so remove that `implementation(...)`
line in `android/app/build.gradle.kts` first if you want to build without
the remote-backup-destinations and Tailscale/VPN features.

---

## Repository layout

```
android/                              Android app (Kotlin / Jetpack Compose) + JNI native code
  app/src/main/java/com/magk/app/     App sources (agent, sandbox, backup, providers, UI, ...)
  app/src/main/cpp/                   Native code: PTY bridge, crash handler, jieba
  app/src/test/                       Unit tests
deps/
  build_proot.sh                      Builds PRoot (+ our patches) into jniLibs/libproot.so
  proot-patches/                      PRoot extensions: --fake-netlink (rtnetlink answered in PRoot)
    tests/run_tests.sh                Regression test for the patches (runs on any x86_64 Linux box)
    tests/rootfs_smoke.sh             Boots the real Alpine aarch64 rootfs (qemu-user) and checks shell, network, tools
  magk-offload-client.c               Guest-side client that lets magk-* tools run with any PRoot
  build_offload_client.sh             Builds it into jniLibs/libmagk_offload.so
  rclone-mobile/                      Go source for the rclone + Tailscale/tsnet gomobile library
  build_rclone_android.sh             Builds rclone-mobile into android/app/libs/rclone.aar
docs/                                 Privacy policy (served by GitHub Pages)
.github/                              CI workflows, issue / PR templates, Dependabot
```

---

## How the Linux sandbox works

The agent's shell runs Alpine Linux under **PRoot** (no root needed). Three pieces
make it complete on a stock Android device:

- **Alpine rootfs** — downloaded and SHA-256-verified at build time (`fetchAlpineRootfs`).
- **PRoot + `--fake-netlink`** — Android denies `NETLINK_ROUTE` sockets to apps, so
  anything that lists network interfaces (Go programs, `ip`, Node, ...) would fail.
  Our PRoot extension answers those requests itself (loopback only).
- **`magk-offload` client** — the `magk-*` tools are served by the app through a local
  socket; the client in the sandbox talks to it, so they work with any PRoot build.

The app only passes PRoot options the bundled binary lists in `--help`
(`ProotCapabilities`), so a PRoot without the extensions still starts a shell.

---

## Acknowledgements

Magk.ai stands on a good deal of open-source work, including:

- **[PRoot](https://github.com/termux/proot)** (GPLv2) — user-space chroot
  powering the sandbox; **[talloc](https://talloc.samba.org)** (LGPLv3+)
  underpins it.
- **[Alpine Linux](https://alpinelinux.org)** — the minirootfs the sandbox boots.
- **[rclone](https://rclone.org)** (MIT) and **[Tailscale](https://tailscale.com)**
  (BSD-3-Clause) — the backends behind remote backup destinations and the
  optional VPN/network integration (see the build note above).
- **[AndroidX & Jetpack Compose](https://developer.android.com/jetpack)**,
  **[OkHttp](https://square.github.io/okhttp/)**, **[Coil](https://coil-kt.github.io/coil/)**,
  **[kotlinx](https://github.com/Kotlin)** serialization & coroutines,
  **[multiplatform-markdown-renderer](https://github.com/mikepenz/multiplatform-markdown-renderer)**,
  **[Reorderable](https://github.com/Calvin-LL/Reorderable)**,
  **[ACRA](https://github.com/ACRA/acra)** (all Apache-2.0), and
  **[Shizuku](https://github.com/RikkaApps/Shizuku-API)** (MIT).
- **[cppjieba](https://github.com/yanyiwu/cppjieba)** (MIT) and
  **[KaTeX](https://katex.org)** (MIT).

---

## Troubleshooting the sandbox

Inside the sandbox shell run **`magk-doctor`** (add `--net` to also test internet access). It checks
the Alpine rootfs, the filesystem, `apk`, network-interface listing and the `magk-*` tool service, and
prints PASS/FAIL with a hint for each failure — paste its output when reporting a problem.

CI runs the same checks on the real Alpine aarch64 rootfs
(`deps/proot-patches/tests/rootfs_smoke.sh`, via qemu-user) on every change to the sandbox code.

---

## Contributing

Issues and pull requests are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). Please report
security problems privately as described in [SECURITY.md](SECURITY.md).

---

## License

Magk.ai is licensed under the **[GNU General Public License v3.0](LICENSE)**.
The app links GPLv2-licensed PRoot, so the combined work is distributed
under GPLv3.

---

## Contact

- **Telegram**: [@O_P_E_2009](https://t.me/O_P_E_2009) — the single point of
  contact for feedback, bug reports and questions.
- **Privacy policy**: [omarmo46-max.github.io/Magk/privacy-policy.html](https://omarmo46-max.github.io/Magk/privacy-policy.html)
- **Issues**: also tracked via [GitHub Issues](https://github.com/omarmo46-max/Magk/issues).

---

Founded by **Omar Barbar**.
