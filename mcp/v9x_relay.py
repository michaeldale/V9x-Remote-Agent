"""Remote-help relay client: drive a V9x agent through the update server.

An opted-in agent (0.12.0+, AGENT.INI [remote] mode=on) checks in with the
update server over plain HTTP. A relay session is a byte tunnel from here to
that agent's own protocol port: what we send is signed chunk by chunk with
the developer's Ed25519 key (by build\\tools\\v9xsign.exe, so the key stays in
its file) and checked by the agent; what comes back is the agent's ordinary
V9XR frames. RelaySession behaves enough like a socket for V9xClient.

Contract: docs/remote-help.md. Stdlib only.
"""

import json
import os
import socket
import struct
import subprocess
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

DEFAULT_SERVER = "https://michaeldale.com.au"
CHUNK_MAX = 1048576
WAIT_MAX = 20


class RelayError(Exception):
    pass


def _home_file(name):
    return os.path.join(os.path.expanduser("~"), ".v9x", name)


def _repo_signer():
    here = os.path.dirname(os.path.abspath(__file__))
    return os.path.join(here, "..", "build", "tools", "v9xsign.exe")


class RelayConfig:
    """Where the server is and where the two secrets live.

    server          V9X_RELAY_SERVER, default https://michaeldale.com.au
                    (a local test site: http://localhost/bluetraitblog)
    operator key    V9X_OPERATOR_KEY, or ~/.v9x/operator.key - opens
                    sessions on the server; it cannot sign commands
    signing key     ~/.v9x/relay-signing.key (v9xsign keygen) - signs
    signer          V9XSIGN, or build/tools/v9xsign.exe in this checkout
    """

    def __init__(self, server=None, operator_key=None, signing_key=None, signer=None):
        self.server = (server or os.environ.get("V9X_RELAY_SERVER") or DEFAULT_SERVER).rstrip("/")
        self.operator_key = operator_key or os.environ.get("V9X_OPERATOR_KEY")
        if not self.operator_key:
            path = _home_file("operator.key")
            try:
                with open(path, "r", encoding="ascii") as handle:
                    self.operator_key = handle.read().strip()
            except OSError:
                self.operator_key = ""
        self.signing_key = signing_key or _home_file("relay-signing.key")
        self.signer = signer or os.environ.get("V9XSIGN") or _repo_signer()

    def check(self, need_signing=True):
        if not self.operator_key:
            raise RelayError(
                "no operator key: put the key from the plugin's Machines page in "
                "%s or V9X_OPERATOR_KEY" % _home_file("operator.key")
            )
        if need_signing:
            if not os.path.isfile(self.signing_key):
                raise RelayError("no relay signing key at %s (v9xsign keygen)" % self.signing_key)
            if not os.path.isfile(self.signer):
                raise RelayError(
                    "v9xsign.exe not found at %s; run scripts\\build-host-tests.ps1 "
                    "or set V9XSIGN" % self.signer
                )

    def request(self, op, params=None, body=None, method=None, timeout=40.0):
        """One operator call. Returns (status, headers, body bytes)."""
        query = {"bt_type": "v9xr_" + op}
        query.update(params or {})
        url = "%s/index.php?%s" % (self.server, urllib.parse.urlencode(query))
        request = urllib.request.Request(
            url,
            data=body,
            method=method or ("POST" if body is not None else "GET"),
            headers={
                "X-V9X-Operator": self.operator_key,
                "User-Agent": "v9x-relay",
                "Content-Type": "application/octet-stream",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=timeout) as response:
                return response.status, response.headers, response.read()
        except urllib.error.HTTPError as exc:
            return exc.code, exc.headers, exc.read()
        except (urllib.error.URLError, OSError) as exc:
            raise RelayError("relay server %s unreachable: %s" % (self.server, exc))

    def json(self, op, params=None, body=None, method=None, timeout=40.0):
        status, _headers, data = self.request(op, params, body, method, timeout)
        try:
            value = json.loads(data.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            raise RelayError("relay server answered %d with something other than JSON" % status)
        if status != 200 or not value.get("success", False):
            raise RelayError(value.get("message") or "relay server error %d" % status)
        return value

    # Machines and chat ----------------------------------------------------

    def machines(self, include_retired=False):
        params = {"all": "1"} if include_retired else {}
        return self.json("machines", params)["machines"]

    def chat_send(self, machine, text):
        return self.json("chat", {"machine": machine}, body=text.encode("utf-8"), method="POST")

    def chat_read(self, machine, after=0):
        return self.json("chat", {"machine": machine, "after": str(int(after))})["messages"]


class RelaySession:
    """A socket-like tunnel to one machine's agent.

    sendall() buffers; the buffer is signed and sent as soon as the caller
    waits for an answer (recv), so one request costs one signing run and one
    POST however many writes made it up.
    """

    # V9xClient.put_stream keeps this many 32 KiB chunks in flight (1 MiB).
    pipeline_window = 32

    def __init__(self, config, machine, open_wait=150.0, timeout=60.0):
        config.check()
        self.config = config
        self.machine = machine.upper()
        self.timeout = max(float(timeout), 30.0)
        self.pending = bytearray()
        self.inbox = bytearray()
        self.down_seq = 1
        self.up_after = 0
        self.closed = False
        opened = config.json("open", {"machine": self.machine}, method="POST")
        self.session = opened["session"]
        self.install_id = opened["install_id"]
        deadline = time.monotonic() + open_wait
        state = opened
        while state["state"] == "pending" and time.monotonic() < deadline:
            wait = int(min(WAIT_MAX, max(1, deadline - time.monotonic())))
            state = config.json("status", {"session": self.session, "wait": str(wait)},
                                timeout=wait + 20.0)
        if state["state"] != "open":
            self._close_quietly()
            if state["state"] == "pending":
                raise RelayError(
                    "machine %s did not take the session within %d s; it checks in every "
                    "60 s when idle, so is it online and set to On?" % (self.machine, open_wait)
                )
            raise RelayError("machine %s refused the session (%s)" % (
                self.machine, state.get("reason") or state["state"]))
        self.nonce = state["nonce"]

    # socket interface -----------------------------------------------------

    def settimeout(self, value):
        if value is not None:
            self.timeout = max(float(value), 30.0)

    def sendall(self, data):
        if self.closed:
            raise ConnectionError("relay session is closed")
        self.pending += data

    def recv(self, count):
        self._flush()
        deadline = time.monotonic() + self.timeout
        while not self.inbox:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout("timed out waiting for the agent through the relay")
            self._pull(int(min(WAIT_MAX, max(1, remaining))))
        piece = bytes(self.inbox[:count])
        del self.inbox[:count]
        return piece

    def close(self):
        if not self.closed:
            self._close_quietly()

    # internals ------------------------------------------------------------

    def _close_quietly(self):
        self.closed = True
        try:
            self.config.request("close", {"session": self.session}, body=b"", method="POST",
                                timeout=15.0)
        except RelayError:
            pass

    def _flush(self):
        if not self.pending:
            return
        if self.closed:
            raise ConnectionError("relay session is closed")
        data = bytes(self.pending)
        self.pending = bytearray()
        handle, raw_path = tempfile.mkstemp(prefix="v9xrelay-", suffix=".in")
        with os.fdopen(handle, "wb") as out:
            out.write(data)
        signed_path = raw_path[:-3] + ".bin"
        try:
            result = subprocess.run(
                [
                    self.config.signer, "relay", "@" + self.config.signing_key,
                    self.machine, self.install_id, self.nonce, self.session,
                    str(self.down_seq), raw_path, signed_path,
                ],
                capture_output=True, text=True, timeout=60,
            )
            if result.returncode != 0:
                raise RelayError("v9xsign failed: %s" % (result.stderr.strip() or result.returncode))
            next_seq = int(result.stdout.strip())
            with open(signed_path, "rb") as handle_in:
                batch = handle_in.read()
        finally:
            for path in (raw_path, signed_path):
                try:
                    os.remove(path)
                except OSError:
                    pass
        # A big transfer is posted a few records at a time.
        offset = 0
        while offset < len(batch):
            end = offset
            count = 0
            while end < len(batch) and count < 4:
                length = struct.unpack_from("<I", batch, end + 8)[0]
                end += 76 + length
                count += 1
            answer = self.config.json("down", {"session": self.session},
                                      body=batch[offset:end], method="POST", timeout=120.0)
            offset = end
        self.down_seq = next_seq
        if answer.get("next_seq") != next_seq:
            raise RelayError("relay server lost track of the sequence (%s)" % answer)

    def _pull(self, wait):
        status, headers, data = self.config.request(
            "up", {"session": self.session, "after": str(self.up_after), "wait": str(wait)},
            timeout=wait + 30.0,
        )
        if status != 200:
            raise ConnectionError("relay server error %d while reading" % status)
        offset = 0
        while offset + 12 <= len(data):
            if data[offset:offset + 4] != b"V9XU":
                raise ConnectionError("malformed relay reply")
            seq, length = struct.unpack_from("<II", data, offset + 4)
            chunk = data[offset + 12:offset + 12 + length]
            if len(chunk) != length:
                raise ConnectionError("truncated relay reply")
            if seq == self.up_after + 1:
                self.inbox += chunk
                self.up_after = seq
            offset += 12 + length
        state = headers.get("X-V9X-Session-State", "open")
        if state in ("closed", "refused") and not self.inbox:
            self.closed = True
            raise ConnectionError(
                "relay session closed (%s)" % (headers.get("X-V9X-Session-Reason") or state)
            )
