#!/usr/bin/env python3
"""Stand-in for the app's NativeOffloadServer (see NativeOffload.kt for the wire format).

Listens on an abstract unix socket, answers every request with a one-line report of what it
received (handler, args, cwd, session id) written to <tmpdir>/.native-offload-<pid>-<n>, and
returns exit code 3 for the handler named "fail". Serves --count connections, then exits.
"""
import argparse, os, socket, struct

ap = argparse.ArgumentParser()
ap.add_argument("--name", required=True)
ap.add_argument("--tmpdir", required=True, help="host directory the guest sees as /tmp")
ap.add_argument("--count", type=int, default=2)
a = ap.parse_args()

def rd(c, n):
    b = b""
    while len(b) < n:
        x = c.recv(n - len(b))
        if not x:
            raise EOFError
        b += x
    return b

def u32(c): return struct.unpack("<i", rd(c, 4))[0]
def st(c):
    n = u32(c)
    return rd(c, n).decode() if n else ""

s = socket.socket(socket.AF_UNIX)
s.bind("\0" + a.name)
s.listen(5)
print("ready", flush=True)
seq = 0
for _ in range(a.count):
    c, _ = s.accept()
    assert u32(c) == 0x46464F4E and u32(c) == 1, "bad request header"
    pid = u32(c)
    argv = [st(c) for _ in range(u32(c))]
    env = dict((lambda x: (x.split("=", 1) + [""])[:2])(st(c)) for _ in range(u32(c)))
    cwd = st(c)
    seq += 1
    fname = ".native-offload-%d-%d" % (pid, seq)
    with open(os.path.join(a.tmpdir, fname), "w") as f:
        f.write("handler=%s args=%s cwd=%s session=%s\n" % (argv[0], argv[1:], cwd, env.get("MAGK_CHAT_SESSION_ID")))
    guest = ("/tmp/" + fname).encode()
    code = 3 if argv[0] == "fail" else 0
    c.sendall(struct.pack("<ii", 0x52464F4E, code) + struct.pack("<i", len(guest)) + guest)
    c.close()
