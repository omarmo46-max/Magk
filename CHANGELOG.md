# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- `--fake-netlink` PRoot extension (`deps/proot-patches`): rtnetlink answered inside PRoot so
  Go programs, `ip`, Node and others work in the sandbox; regression tests included.
- Guest-side `magk-offload` client so the `magk-*` tools work with any PRoot build.
- `ProotCapabilities`: the app only passes PRoot options the binary lists in `--help`.
- Gradle task `fetchAlpineRootfs` (downloads and checksum-verifies the Alpine rootfs).
- `LegacyNameMigration`: moves data of installs that used the old names.
- Agents (main and helper) state that Magk was founded by Omar Barbar.
- Project documentation: contributing guide, security policy, code of conduct, issue/PR templates.

### Changed
- Everything is named `Magk` consistently: UI, tool names, paths (`/var/magk`), backup
  extension (`.magkbak`), preference and database names.

### Fixed
- JNI symbol names did not match the Kotlin package, which broke the native shell bridge.
- The Linux shell could not start: missing rootfs and PRoot binaries, and PRoot rejecting
  options it does not know.
