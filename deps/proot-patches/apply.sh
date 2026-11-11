#!/usr/bin/env bash
# Adds the Magk extensions to a termux/proot source tree:
#   --fake-netlink   (fake_netlink.c)  rtnetlink answered inside PRoot
# Usage: apply.sh /path/to/proot   (idempotent)
set -euo pipefail
SRC="${1:?usage: apply.sh <proot-source-dir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
[ -d "$SRC/src/extension" ] || { echo "not a proot tree: $SRC"; exit 1; }

mkdir -p "$SRC/src/extension/fake_netlink"
cp "$HERE/fake_netlink.c" "$SRC/src/extension/fake_netlink/fake_netlink.c"

python3 - "$SRC/src" <<'PY'
import re, sys
src = sys.argv[1]
def edit(path, fn):
    s = open(path).read()
    n = fn(s)
    if n != s:
        open(path, "w").write(n)

# 1. callback declaration
edit(src + "/extension/extension.h", lambda s: s if "fake_netlink_callback" in s else s.replace(
    "extern int port_switch_callback(",
    "extern int fake_netlink_callback(Extension *extension, ExtensionEvent event, intptr_t d1, intptr_t d2);\nextern int port_switch_callback(", 1))

# 2. object file
edit(src + "/GNUmakefile", lambda s: s if "fake_netlink.o" in s else s.replace(
    "\textension/port_switch/port_switch.o \\\n",
    "\textension/port_switch/port_switch.o \\\n\textension/fake_netlink/fake_netlink.o \\\n", 1))

# 3. option handler + table entry
def proot_c(s):
    if "handle_option_fake_netlink" in s:
        return s
    h = '''static int handle_option_fake_netlink(Tracee *tracee, const Cli *cli UNUSED, const char *value UNUSED)
{
\tif (initialize_extension(tracee, fake_netlink_callback, NULL) < 0)
\t\tnote(tracee, WARNING, INTERNAL, "fake-netlink not initialized");
\treturn 0;
}

'''
    return s.replace("static int handle_option_H(", h + "static int handle_option_H(", 1)
edit(src + "/cli/proot.c", proot_c)

def proot_h(s):
    if "handle_option_fake_netlink" in s:
        return s
    anchor = "static int handle_option_sysvipc("
    assert anchor in s, "proot.h declaration anchor not found"
    s = s.replace(anchor,
        "static int handle_option_fake_netlink(Tracee *tracee, const Cli *cli, const char *value);\n" + anchor, 1)
    entry = '''        { .class = "Extension options",
          .arguments = {
                { .name = "--fake-netlink", .separator = '\\0', .value = NULL },
                { .name = NULL, .separator = '\\0', .value = NULL } },
          .handler = handle_option_fake_netlink,
          .description = "Answer rtnetlink requests inside PRoot (loopback only) and make SO_MARK a no-op.",
          .detail = "\\tAndroid denies NETLINK_ROUTE sockets to apps; programs that list network\\n\\
\\tinterfaces (Go, ip, ...) fail without this.",
        },
'''
    marker = '        { .class = "Extension options",\n          .arguments = {\n                { .name = "-H"'
    assert marker in s, "proot.h anchor not found"
    return s.replace(marker, entry + marker, 1)
edit(src + "/cli/proot.h", proot_h)
PY
echo "fake_netlink applied to $SRC"
