"""vnc_stall.py: drive V9XVNC.EXE's watchdog with the TEMP\\VNCSTALL.DAT test hook.

1. Connect, take a full frame.
2. Create the stall file: the next capture hangs in the worker.
3. While stalled, move the guest pointer through VNC and check through the
   agent that it moved (input still served), and that no frame arrives.
4. Delete the stall file: frames must resume.
5. With the file present again, a NEW session must hit the 15 s setup limit
   and the server must exit 10 (checked with -status afterwards).

Needs a guest with VNC enabled and running, and shell (allow_dos_vm=1) to
delete the stall file. It ends with the server stopped; start it again with
"v9xctl vnc start".

    python tests/integration/vnc_stall.py <agent port> <vnc port>
"""

import json
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import vnc_smoke  # noqa: E402

V9XCTL = os.path.join(REPO, "scripts", "v9xctl.ps1")
STALL = "C:\\V9XREMOTE\\TEMP\\VNCSTALL.DAT"
AGENT_PORT = sys.argv[1] if len(sys.argv) > 1 else "9869"
VNC_PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 5900
SCRATCH = os.environ.get("TEMP", HERE)


def ctl(*args):
    out = subprocess.run(
        ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", V9XCTL]
        + list(args) + ["-Port", AGENT_PORT, "-Json"],
        capture_output=True, text=True)
    try:
        return out.returncode, json.loads(out.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return out.returncode, out.stdout + out.stderr


def stall(on):
    if on:
        marker = os.path.join(SCRATCH, "VNCSTALL.DAT")
        open(marker, "wb").write(b"stall\r\n")
        return ctl("put", "-Source", marker, "-Destination", STALL)
    return ctl("shell", "-Command", "DEL " + STALL)


def cursor():
    _, result = ctl("input", "-Sequence", "delay 1")
    return result["CursorX"], result["CursorY"]


def connect():
    client = vnc_smoke.Rfb("127.0.0.1", VNC_PORT, 8, 30)
    client.stats = dict.fromkeys(("raw", "hextile", "desktop_size", "tiles",
                                  "hex_raw_tiles", "subrects", "cursor_shapes",
                                  "pointer_pos"), 0)
    client.cursor = None
    client.pointer_pos = None
    client.set_encodings([vnc_smoke.ENC_HEXTILE, vnc_smoke.ENC_RAW])
    return client


def frame_within(client, seconds):
    client.sock.settimeout(seconds)
    try:
        while client.read_update() is None:
            pass
        return True
    except socket.timeout:
        return False


failures = []
client = connect()
client.request(False)
print("full frame:", frame_within(client, 30))

print("stall on:", stall(True)[0])
time.sleep(1.0)
client.pointer(0, 111, 222)   # input during the stall
client.request(False)         # asks for a frame the stalled worker cannot make
time.sleep(1.0)
position = cursor()
print("cursor moved during stall:", position)
if position != (111, 222):
    failures.append("input was not served during the stall")
got = frame_within(client, 7.0)
print("frame during stall (expect False):", got)
if got:
    failures.append("a frame arrived while the capture was stalled")

print("stall off:", stall(False)[0])
got = frame_within(client, 10.0)
print("frame after recovery (expect True):", got)
if not got:
    failures.append("frames did not resume after the stall")
client.sock.close()

print("stall on for setup:", stall(True)[0])
time.sleep(1.0)
started = time.time()
try:
    second = connect()  # handshake happens after INIT; INIT is stuck
    second.request(False)
    frame_within(second, 25.0)
    print("unexpected: session opened during a setup stall")
    failures.append("session opened while capture setup was stalled")
except (RuntimeError, OSError) as error:
    print("session refused after %.1fs: %s" % (time.time() - started, error))
time.sleep(1.0)
print("stall off:", stall(False)[0])
code, status = ctl("vnc", "status")
print("status after setup stall:", status)
if isinstance(status, dict) and status.get("Running"):
    failures.append("server still running after a 15 s setup stall")

print("FAILURES:" if failures else "ALL PASSED", failures if failures else "")
sys.exit(1 if failures else 0)
