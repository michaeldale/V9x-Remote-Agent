"""Offline tests for the V9x MCP server: codecs, BMP->PNG, JSON-RPC loop."""

import io
import json
import os
import socket
import struct
import threading
import unittest
import zlib

import v9x_mcp


class DesktopWaitTests(unittest.TestCase):
    def test_guest_wait_protocol(self):
        client = v9x_mcp.V9xClient.__new__(v9x_mcp.V9xClient)
        client.hello = {"capabilities": v9x_mcp.CAP_LOGON_DISMISS, "build_id": "fixture"}
        client.timeout = 2
        sent, timeouts = [], []
        class Socket:
            def settimeout(self, value):
                timeouts.append(value)
        client.sock = Socket()
        def request(cap, name, msg, payload, response):
            client.require(cap, name)
            sent.append((msg, payload, response))
            return struct.pack("<6I", 3, 7, 1024, 768, 0, 100)
        client.tool_request = request
        result = client.wait_desktop(5, True)
        self.assertEqual(sent, [(0x65, struct.pack("<II", 1, 5000), 0x8065)])
        self.assertTrue(result["desktop_ready"] and result["logon_dismissed"])
        self.assertEqual(result["boot_counter"], 7)
        self.assertEqual(timeouts, [35, 2])

    def test_long_wait_is_chunked_to_guest_limit(self):
        # The guest caps one DESKTOP_WAIT at 60 s; the host loops to its own
        # deadline and keeps a dismissal reported by an earlier chunk.
        client = v9x_mcp.V9xClient.__new__(v9x_mcp.V9xClient)
        client.hello = {"capabilities": v9x_mcp.CAP_LOGON_DISMISS, "build_id": "fixture"}
        client.timeout = 2
        class Socket:
            def settimeout(self, value):
                pass
        client.sock = Socket()
        clock, sent = [0.0], []
        def request(cap, name, msg, payload, response):
            ms = struct.unpack("<II", payload)[1]
            sent.append(ms)
            clock[0] += ms / 1000.0
            flags = 2 if len(sent) == 1 else (1 if len(sent) == 3 else 0)
            return struct.pack("<6I", flags, 7, 1024, 768, 0, ms)
        client.tool_request = request
        old = v9x_mcp.time.monotonic
        v9x_mcp.time.monotonic = lambda: clock[0]
        try:
            result = client.wait_desktop(150, True)
        finally:
            v9x_mcp.time.monotonic = old
        self.assertEqual(sent, [60000, 60000, 30000])
        self.assertTrue(result["desktop_ready"] and result["logon_dismissed"])
        self.assertEqual(result["waited_ms"], 150000)

    def test_kill_reports_kill_when_restore_fails(self):
        class Client:
            hello = {"capabilities": 0xFFFFFFFF}
            def __enter__(self): return self
            def __exit__(self, *exc): return False
            def require(self, cap, name): pass
            def kill(self, pid): return {"pid": pid, "name": "GAME.EXE"}
            def wait_process(self, seconds, pid):
                raise v9x_mcp.V9xError("pid %d still running after 10 s" % pid)
        handler = v9x_mcp.ToolHandler.__new__(v9x_mcp.ToolHandler)
        handler.connect = lambda *a, **k: Client()
        with self.assertRaises(v9x_mcp.V9xError) as caught:
            handler.v9x_kill({"pid": 123, "restore_display": True})
        self.assertIn("pid 123 was killed", str(caught.exception))
        self.assertIn("GAME.EXE", str(caught.exception))

    def test_reboot_cancels_only_after_matching_proof(self):
        calls = []
        class Candidate:
            def __init__(self, boot, token):
                self.hello = {"boot_counter": boot, "pending_job": token}
            def wait_desktop(self, timeout, dismiss_logon):
                calls.append((self.hello["pending_job"], timeout, dismiss_logon))
                return {"desktop_ready": True, "logon_dismissed": True}
            def close(self):
                calls.append("close")
        candidates = [Candidate(1, "job"), Candidate(2, "wrong"), Candidate(2, "job")]
        old_sleep = v9x_mcp.time.sleep
        v9x_mcp.time.sleep = lambda _s: None
        try:
            result = v9x_mcp.wait_for_reboot(None, None, 1, "job", 5,
                connect=lambda _left: candidates.pop(0), dismiss_logon=True)
        finally:
            v9x_mcp.time.sleep = old_sleep
        self.assertTrue(result["logon_dismissed"])
        self.assertEqual(calls, ["close", "close", ("job", 5, True), "close"])

class StringCodecTests(unittest.TestCase):
    def test_round_trip(self):
        encoded = v9x_mcp.encode_string("C:\\V9XREMOTE")
        value, offset = v9x_mcp.parse_string(encoded, 0)
        self.assertEqual(value, "C:\\V9XREMOTE")
        self.assertEqual(offset, len(encoded))

    def test_empty(self):
        encoded = v9x_mcp.encode_string("")
        self.assertEqual(encoded, b"\x00\x00")
        value, offset = v9x_mcp.parse_string(encoded, 0)
        self.assertEqual(value, "")
        self.assertEqual(offset, 2)

    def test_rejects_non_ascii(self):
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.encode_string("caf\u00e9")

    def test_rejects_nul(self):
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.encode_string("a\x00b")

    def test_truncated(self):
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.parse_string(b"\x05\x00abc", 0)


class BareLfWarningTests(unittest.TestCase):
    def test_lf_only_ini_warns(self):
        warning = v9x_mcp.bare_lf_warning(
            "C:\\V9XREMOTE\\AGENT.INI", b"[agent]\nport=9869\n"
        )
        self.assertIn("LF-only", warning)

    def test_crlf_and_other_files_pass(self):
        self.assertIsNone(
            v9x_mcp.bare_lf_warning("C:\\AUTOEXEC.BAT", b"@ECHO OFF\r\nPATH C:\\\r\n")
        )
        self.assertIsNone(v9x_mcp.bare_lf_warning("C:\\NOTES.TXT", b"a\nb\n"))
        self.assertIsNone(v9x_mcp.bare_lf_warning("C:\\X.INF", b"a\n\0b"))


class VncIniTests(unittest.TestCase):
    def test_enabled_and_port(self):
        text = ("[agent]\r\nport=9869\r\nenabled=1\r\n[vnc]\r\n; comment\r\n"
                "enabled=1\r\nport=5901\r\n")
        self.assertEqual(v9x_mcp.parse_vnc_ini(text), (True, 5901))

    def test_off_by_default(self):
        self.assertEqual(v9x_mcp.parse_vnc_ini("[agent]\r\nport=9869\r\n"), (False, 5900))
        self.assertEqual(v9x_mcp.parse_vnc_ini("[vnc]\r\nenabled=0\r\n"), (False, 5900))
        self.assertEqual(v9x_mcp.parse_vnc_ini("[VNC]\nenabled = 1\n"), (True, 5900))

    def test_password_set(self):
        self.assertTrue(v9x_mcp.vnc_password_set("[vnc]\r\npassword=abc\r\n"))
        self.assertFalse(v9x_mcp.vnc_password_set("[vnc]\r\npassword=\r\n"))
        self.assertFalse(v9x_mcp.vnc_password_set("[agent]\r\npassword=abc\r\n"))


class FrameTests(unittest.TestCase):
    def test_header_layout_matches_protocol(self):
        header = v9x_mcp.HEADER.pack(
            v9x_mcp.MAGIC, v9x_mcp.PROTOCOL_VERSION, 0x0002, 7, 0, 8, 0
        )
        self.assertEqual(len(header), 24)
        self.assertEqual(header[0:4], b"V9XR")
        self.assertEqual(struct.unpack_from("<H", header, 4)[0], 0x0100)
        self.assertEqual(struct.unpack_from("<H", header, 6)[0], 0x0002)
        self.assertEqual(struct.unpack_from("<I", header, 8)[0], 7)
        self.assertEqual(struct.unpack_from("<I", header, 16)[0], 8)
        self.assertEqual(struct.unpack_from("<I", header, 20)[0], 0)


class HelloParseTests(unittest.TestCase):
    def build_hello(self):
        payload = struct.pack(
            "<HHIIIHH", 0x0100, 0, 0x3FF, 65536, 42, 9869, 0x0101
        )
        payload += v9x_mcp.encode_string("fixture-build")
        payload += v9x_mcp.encode_string("job-1")
        payload += struct.pack("<BBBB", 1, 0, 0, 0)
        payload += struct.pack("<III", 800, 600, 16)
        payload += v9x_mcp.encode_string("0.0.0.0")
        payload += v9x_mcp.encode_string("")
        return payload

    def test_parse_hello(self):
        hello = v9x_mcp.parse_hello(self.build_hello())
        self.assertEqual(hello["protocol_version"], 0x0100)
        self.assertEqual(hello["capabilities"], 0x3FF)
        self.assertEqual(hello["boot_counter"], 42)
        self.assertEqual(hello["port"], 9869)
        self.assertEqual(hello["build_id"], "fixture-build")
        self.assertEqual(hello["pending_job"], "job-1")
        self.assertTrue(hello["desktop_ready"])
        self.assertEqual(hello["screen_width"], 800)
        self.assertEqual(hello["screen_height"], 600)
        self.assertEqual(hello["bits_per_pixel"], 16)
        self.assertEqual(hello["listen_address"], "0.0.0.0")

    def test_trailing_data_rejected(self):
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.parse_hello(self.build_hello() + b"\x00")


class InventoryParseTests(unittest.TestCase):
    @staticmethod
    def record(record_type, fields):
        body = v9x_mcp.encode_string(record_type) + struct.pack("<H", len(fields))
        for key, value in fields:
            body += v9x_mcp.encode_string(key) + v9x_mcp.encode_string(value)
        return body

    def build(self):
        payload = struct.pack("<H", 3)
        payload += self.record("cpu", [("vendor", "GenuineIntel"), ("mhz", "450")])
        payload += self.record("drive", [("letter", "C"), ("type", "fixed"),
                                         ("total", "8589934592")])
        payload += self.record("net", [("ip", "10.0.0.5"), ("ip", "192.168.1.9"),
                                       ("ip", "172.16.0.1")])
        return payload

    def test_parse_inventory(self):
        records = v9x_mcp.parse_inventory(self.build())
        self.assertEqual([r["record"] for r in records], ["cpu", "drive", "net"])
        self.assertEqual(records[0]["fields"]["vendor"], "GenuineIntel")
        self.assertEqual(records[1]["fields"]["total"], "8589934592")
        self.assertEqual(records[2]["fields"]["ip"],
                         ["10.0.0.5", "192.168.1.9", "172.16.0.1"])

    def test_empty_inventory(self):
        self.assertEqual(v9x_mcp.parse_inventory(struct.pack("<H", 0)), [])

    def test_trailing_and_truncated_rejected(self):
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.parse_inventory(self.build() + b"\x00")
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.parse_inventory(self.build()[:-3])


class BmpToPngTests(unittest.TestCase):
    @staticmethod
    def build_bmp(width, height, pixels_bgr):
        stride = (width * 3 + 3) & ~3
        pixel_bytes = bytearray()
        for row in reversed(pixels_bgr):
            line = bytearray()
            for b, g, r in row:
                line += bytes((b, g, r))
            line += b"\x00" * (stride - len(line))
            pixel_bytes += line
        file_size = 54 + len(pixel_bytes)
        header = b"BM" + struct.pack("<IHHI", file_size, 0, 0, 54)
        info = struct.pack(
            "<IiiHHIIiiII", 40, width, height, 1, 24, 0, len(pixel_bytes), 2835,
            2835, 0, 0,
        )
        return bytes(header + info + pixel_bytes)

    def test_two_by_two(self):
        # Top-left red, top-right green, bottom-left blue, bottom-right white.
        bmp = self.build_bmp(
            2,
            2,
            [
                [(0, 0, 255), (0, 255, 0)],
                [(255, 0, 0), (255, 255, 255)],
            ],
        )
        png = v9x_mcp.bmp_to_png(bmp)
        self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
        width, height = struct.unpack_from(">II", png, 16)
        self.assertEqual((width, height), (2, 2))
        self.assertEqual(png[24:29], bytes((8, 2, 0, 0, 0)))
        idat_offset = png.index(b"IDAT") + 4
        idat_length = struct.unpack_from(">I", png, idat_offset - 8)[0]
        raw = zlib.decompress(png[idat_offset : idat_offset + idat_length])
        expected = (
            b"\x00" + bytes((255, 0, 0, 0, 255, 0))
            + b"\x00" + bytes((0, 0, 255, 255, 255, 255))
        )
        self.assertEqual(raw, expected)

    def test_rejects_non_bmp(self):
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.bmp_to_png(b"PK\x03\x04" + b"\x00" * 60)

    def test_rle8(self):
        # 4x2, palette black/red: bottom row a run of 4 reds, top row the
        # literal 0,1,0 then a run of one black; end of line, end of bitmap.
        palette = b"\x00\x00\x00\x00" + b"\x00\x00\xff\x00"
        pixels = b"\x04\x01\x00\x00" + b"\x00\x03\x00\x01\x00\x00" + b"\x01\x00\x00\x01"
        offset = 54 + len(palette)
        bmp = (b"BM" + struct.pack("<IHHI", offset + len(pixels), 0, 0, offset)
               + struct.pack("<IiiHHIIiiII", 40, 4, 2, 1, 8, 1, len(pixels), 0, 0, 2, 2)
               + palette + pixels)
        width, height, rows = v9x_mcp.decode_bmp(bmp)
        red, black = b"\xff\x00\x00", b"\x00\x00\x00"
        self.assertEqual((width, height), (4, 2))
        self.assertEqual(rows, [black + red + black + black, red * 4])

    def test_agent_fixtures(self):
        """The BMPs the agent's shotxf.c writes (dumped by build-host-tests.ps1)
        decode to exactly the RGB its C reference decoder produced."""
        folder = os.path.join(os.path.dirname(__file__), "..", "build", "host-tests", "shotxf")
        if not os.path.isdir(folder):
            self.skipTest("run scripts\\build-host-tests.ps1 to dump the shotxf fixtures")
        names = sorted(n[:-4] for n in os.listdir(folder) if n.endswith(".bmp"))
        self.assertGreaterEqual(len(names), 6)
        for name in names:
            with open(os.path.join(folder, name + ".bmp"), "rb") as handle:
                width, height, rows = v9x_mcp.decode_bmp(handle.read())
            with open(os.path.join(folder, name + ".rgb"), "rb") as handle:
                self.assertEqual(b"".join(rows), handle.read(), name)

    def test_host_shape_matches_agent(self):
        """The fallback for pre-0.13.0 agents crops and box-scales like
        shotxf.c: the crop_scale fixture is region 3,2,20,16 at 50 percent of
        full24."""
        folder = os.path.join(os.path.dirname(__file__), "..", "build", "host-tests", "shotxf")
        if not os.path.isdir(folder):
            self.skipTest("run scripts\\build-host-tests.ps1 to dump the shotxf fixtures")
        with open(os.path.join(folder, "full24.bmp"), "rb") as handle:
            width, height, rows = v9x_mcp.decode_bmp(handle.read())
        out_w, out_h, shaped = v9x_mcp.shape_rgb(width, height, rows, [3, 2, 20, 16], 50)
        with open(os.path.join(folder, "crop_scale.rgb"), "rb") as handle:
            self.assertEqual(b"".join(shaped), handle.read())
        self.assertEqual((out_w, out_h), (10, 8))

    def test_gray_png_is_one_channel(self):
        png = v9x_mcp.encode_png(1, 1, [b"\x40\x40\x40"], gray=True)
        self.assertEqual(png[24:26], bytes((8, 0)))


class ToolCodecTests(unittest.TestCase):
    """FILE_READ_TEXT, PROCESS_LIST, REG_QUERY (agent 0.13.0+)."""

    @staticmethod
    def string(value, encoding="ascii"):
        raw = value.encode(encoding)
        return struct.pack("<H", len(raw)) + raw

    def test_read_text(self):
        text = b"one\nzwei \xfc\n"
        payload = struct.pack("<6I", 9000, 50, 2, 2, 1, len(text)) + text
        result = v9x_mcp.parse_read_text(payload)
        self.assertEqual(result["text"], "one\nzwei \u00fc\n")
        self.assertTrue(result["truncated"])
        self.assertEqual((result["lines"], result["matched"], result["returned"]), (50, 2, 2))
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.parse_read_text(payload + b"x")

    def test_process_list(self):
        payload = (struct.pack("<I", 2)
                   + struct.pack("<5I", 0xFFFE8075, 0, 5, 2, 0) + self.string("C:\\V9XREMOTE\\V9XAGNT.EXE")
                   + struct.pack("<5I", 0x10, 0xFFFE8075, 1, 1, 99) + self.string("NOTEPAD.EXE"))
        agent, job = v9x_mcp.parse_process_list(payload)
        self.assertEqual(agent["name"], "V9XAGNT.EXE")
        self.assertTrue(agent["agent"])
        self.assertFalse(agent["agent_job"])
        self.assertEqual((job["name"], job["agent_job"], job["job_request_id"]), ("NOTEPAD.EXE", True, 99))

    def test_reg_query(self):
        values = (self.string("") + struct.pack("<II", 1, 3) + b"hi\x00"
                  + self.string("N") + struct.pack("<II", 4, 4) + struct.pack("<I", 7)
                  + self.string("M") + struct.pack("<II", 7, 5) + b"a\x00b\x00\x00"
                  + self.string("B") + struct.pack("<II", 3, 2) + b"\x01\xff")
        payload = struct.pack("<III", 1, 4, 1) + self.string("Sub") + values
        result = v9x_mcp.parse_reg_query(payload)
        self.assertEqual(result["subkeys"], ["Sub"])
        self.assertTrue(result["truncated"])
        data = {v["name"]: (v["type"], v["data"]) for v in result["values"]}
        self.assertEqual(data["(Default)"], ("REG_SZ", "hi"))
        self.assertEqual(data["N"], ("REG_DWORD", 7))
        self.assertEqual(data["M"], ("REG_MULTI_SZ", ["a", "b"]))
        self.assertEqual(data["B"], ("REG_BINARY", "01 ff"))

    def test_reg_key_encoding(self):
        self.assertEqual(v9x_mcp.encode_reg_key("HKLM\\Software\\X"),
                         struct.pack("<I", 0x80000002) + self.string("Software\\X"))
        self.assertEqual(v9x_mcp.encode_reg_key("hkey_current_user"),
                         struct.pack("<I", 0x80000001) + self.string(""))
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.encode_reg_key("HKXX\\Software")

    def test_window_list(self):
        entry = (struct.pack("<IIIiiii", 0x20, 7, 1, -4, 10, 500, 350)
                 + self.string("#32770") + self.string("Setup"))
        (window,) = v9x_mcp.parse_window_list(struct.pack("<II", 1, 0) + entry)
        self.assertEqual((window["title"], window["class"], window["x"], window["y"]),
                         ("Setup", "#32770", -4, 10))
        self.assertEqual((window["width"], window["height"], window["foreground"]), (504, 340, True))

    def window_text_payload(self, handle, pid, title, controls):
        payload = struct.pack("<4I", handle, pid, len(controls), 0)
        payload += self.string("#32770") + self.string(title)
        for child, control_id, visible, cls, text in controls:
            payload += struct.pack("<III", child, control_id, 1 if visible else 0)
            payload += self.string(cls) + self.string(text)
        return payload

    WIN98_FAULT = [
        (0x10, 1, True, "Button", "&Close"),
        (0x11, 5, False, "Button", "De&bug"),
        (0x12, 8, True, "Edit", "CRASHPRB caused an invalid page fault in\r\nmodule CRASHPRB.EXE at 016f:004010e8."),
        (0x13, 3, True, "Static", "This program has performed an illegal operation and will be shut down."),
    ]

    def test_window_text_and_fault_classification(self):
        parsed = v9x_mcp.parse_window_text(self.window_text_payload(0x1B8, 9, "Crashprb", self.WIN98_FAULT))
        self.assertEqual((parsed["handle"], parsed["pid"], parsed["title"], len(parsed["controls"])),
                         (0x1B8, 9, "Crashprb", 4))
        fault = v9x_mcp.classify_fault_dialog(parsed)
        self.assertTrue(fault["is_fault"])
        self.assertEqual(fault["buttons"], ["Close"])
        self.assertTrue(fault["details"].startswith("CRASHPRB caused an invalid page fault in\nmodule"))
        self.assertTrue(v9x_mcp.fault_names_target(fault["all_text"], {"CRASHPRB"}))
        self.assertFalse(v9x_mcp.fault_names_target(fault["all_text"], {"CRASH"}))
        box = v9x_mcp.classify_fault_dialog(v9x_mcp.parse_window_text(self.window_text_payload(
            1, 9, "CRASHPRB", [(2, 2, True, "Static", "An ordinary message, not a fault.")])))
        self.assertFalse(box["is_fault"])
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.parse_window_text(self.window_text_payload(1, 2, "x", [])[:-1])

    def test_wait_fault_reports_a_dialog_owned_by_another_process(self):
        # 9x: the dialog belongs to KERNEL32 (pid 1), not the crashed pid 42.
        closed = []
        windows = [[{"handle": 0x1B8, "pid": 1, "class": "#32770", "title": "Crashprb"}], []]
        client = self.fake_client(
            processes=lambda: [{"pid": 42, "name": "CRASHPRB.EXE"}],
            windows=lambda: windows.pop(0) if len(windows) > 1 else windows[0],
            window_text=lambda h: v9x_mcp.parse_window_text(
                self.window_text_payload(h, 1, "Crashprb", self.WIN98_FAULT)),
            close_window=lambda h: closed.append(h))
        result = client.wait_fault(5, pid=42, dismiss=True)
        self.assertEqual((result["outcome"], result["dismissed"], closed), ("fault", True, [0x1B8]))
        self.assertIn("illegal operation", result["fault"]["message"])
        self.assertNotIn("all_text", result["fault"])

    def test_wait_fault_exits_and_any_mode_times_out_quietly(self):
        client = self.fake_client(processes=lambda: [], windows=lambda: [])
        self.assertEqual(client.wait_fault(5, name="GONE.EXE")["outcome"], "exited")
        original = v9x_mcp.poll_pause
        v9x_mcp.poll_pause = lambda _started: None
        try:
            self.assertEqual(client.wait_fault(0)["outcome"], "no_fault")
        finally:
            v9x_mcp.poll_pause = original

    def test_connect_retries_a_connection_dropped_before_hello(self):
        attempts = []

        def fake_client(*_args, **_kwargs):
            attempts.append(1)
            if len(attempts) == 1:
                raise ConnectionResetError("forward answered, agent not listening")
            return "client"

        original, original_sleep = v9x_mcp.V9xClient, v9x_mcp.time.sleep
        v9x_mcp.V9xClient, v9x_mcp.time.sleep = fake_client, lambda _s: None
        try:
            handler = v9x_mcp.ToolHandler("127.0.0.1", 1, 0.1)
            self.assertEqual(handler.connect(), "client")
            self.assertEqual(len(attempts), 2)
        finally:
            v9x_mcp.V9xClient, v9x_mcp.time.sleep = original, original_sleep

    def test_connect_refused_fails_fast_without_wait(self):
        attempts = []

        def refused(*_args, **_kwargs):
            attempts.append(1)
            raise ConnectionRefusedError("refused")

        original = v9x_mcp.V9xClient
        v9x_mcp.V9xClient = refused
        try:
            handler = v9x_mcp.ToolHandler("127.0.0.1", 1, 0.1)
            with self.assertRaises(v9x_mcp.V9xError):
                handler.connect()
            self.assertEqual(len(attempts), 1)
        finally:
            v9x_mcp.V9xClient = original

    def test_tools_are_listed(self):
        names = {tool["name"] for tool in v9x_mcp.TOOLS}
        for name in ("v9x_read_text", "v9x_hash", "v9x_delete", "v9x_ps", "v9x_kill", "v9x_reg_query",
                     "v9x_windows", "v9x_wait_file", "v9x_wait_process", "v9x_note",
                     "v9x_wait_fault", "v9x_close_window"):
            self.assertIn(name, names)
            self.assertTrue(callable(getattr(v9x_mcp.ToolHandler, name)))

    def fake_client(self, **methods):
        client = v9x_mcp.V9xClient.__new__(v9x_mcp.V9xClient)
        client.hello = {"capabilities": 0x01FFFFFF, "build_id": "test"}  # fixture models 0.16 operations
        for name, method in methods.items():
            setattr(client, name, method)
        return client

    def test_note_sends_cleaned_text(self):
        sent = []
        client = self.fake_client(
            tool_request=lambda cap, name, msg, payload, resp: sent.append((cap, msg, payload, resp)) or b"")
        result = client.note("Driver test 12 –\tVBE ")
        self.assertEqual(result, {"text": "Driver test 12 ? VBE"})
        self.assertEqual(sent, [(v9x_mcp.CAP_EVENT_LOG, 0x00A0,
                                 v9x_mcp.encode_string("Driver test 12 ? VBE"), 0x80A0)])
        with self.assertRaises(v9x_mcp.V9xError):
            client.note("x" * 201)
        with self.assertRaises(v9x_mcp.V9xError):
            client.note(" \t ")

    def test_wait_process_returns_once_the_pid_is_gone(self):
        lists = [[{"pid": 7, "name": "BENCH.EXE"}, {"pid": 9, "name": "OTHER.EXE"}],
                 [{"pid": 7, "name": "BENCH.EXE"}], []]
        client = self.fake_client(processes=lambda: lists.pop(0))
        original = v9x_mcp.time.sleep
        v9x_mcp.time.sleep = lambda _s: None
        try:
            result = client.wait_process(60, pid=7)
        finally:
            v9x_mcp.time.sleep = original
        self.assertTrue(result["was_running"])
        self.assertEqual(result["exited"], ["BENCH.EXE"])
        self.assertEqual(lists, [])

    def test_wait_process_by_name_reports_never_running(self):
        client = self.fake_client(processes=lambda: [{"pid": 9, "name": "OTHER.EXE"}])
        result = client.wait_process(60, name="bench*.exe")
        self.assertFalse(result["was_running"])

    def test_wait_file_match_waits_for_the_line(self):
        reads = [{"matched": 0, "text": ""}, {"matched": 1, "text": "Complete=yes\n"}]
        client = self.fake_client(
            stat=lambda _p: {"exists": True, "size": 10},
            read_text=lambda _p, _t, _m, _b: reads.pop(0))
        original = v9x_mcp.time.sleep
        v9x_mcp.time.sleep = lambda _s: None
        try:
            result = client.wait_file("C:\\R.INI", 60, match="complete=yes")
        finally:
            v9x_mcp.time.sleep = original
        self.assertEqual(result["line"], "Complete=yes")


class JsonRpcTests(unittest.TestCase):
    def run_server(self, messages):
        stdin = io.StringIO("".join(json.dumps(m) + "\n" for m in messages))
        stdout = io.StringIO()
        server = v9x_mcp.McpServer(
            v9x_mcp.ToolHandler("127.0.0.1", 1, 0.1), stdin=stdin, stdout=stdout
        )
        server.run()
        return [json.loads(line) for line in stdout.getvalue().splitlines()]

    def test_initialize_and_tools_list(self):
        responses = self.run_server(
            [
                {
                    "jsonrpc": "2.0",
                    "id": 1,
                    "method": "initialize",
                    "params": {"protocolVersion": "2024-11-05"},
                },
                {"jsonrpc": "2.0", "method": "notifications/initialized"},
                {"jsonrpc": "2.0", "id": 2, "method": "tools/list"},
            ]
        )
        self.assertEqual(len(responses), 2)
        init = responses[0]
        self.assertEqual(init["id"], 1)
        self.assertEqual(init["result"]["protocolVersion"], "2024-11-05")
        self.assertEqual(init["result"]["serverInfo"]["name"], "v9x-remote-agent")
        tools = responses[1]["result"]["tools"]
        names = {tool["name"] for tool in tools}
        self.assertIn("v9x_exec", names)
        self.assertIn("v9x_screenshot", names)
        self.assertIn("v9x_reboot_with_proof", names)
        self.assertIn("v9x_reset_with_proof", names)
        self.assertIn("v9x_click", names)
        self.assertIn("v9x_key", names)
        self.assertIn("v9x_download", names)
        self.assertIn("v9x_update", names)
        self.assertIn("v9x_inventory", names)
        self.assertIn("v9x_discover", names)
        self.assertIn("v9x_vnc", names)
        self.assertIn("v9x_relay_machines", names)
        self.assertIn("v9x_chat", names)
        self.assertEqual(len(tools), 42)
        # Every tool that talks to an agent can go through the relay.
        for tool in tools:
            if tool["name"] in ("v9x_discover", "v9x_relay_machines", "v9x_chat"):
                self.assertNotIn("relay", tool["inputSchema"]["properties"])
            else:
                self.assertIn("relay", tool["inputSchema"]["properties"], tool["name"])

    def test_relay_argument_must_be_machine_id(self):
        responses = self.run_server(
            [
                {
                    "jsonrpc": "2.0",
                    "id": 11,
                    "method": "tools/call",
                    "params": {"name": "v9x_ping", "arguments": {"relay": "not-a-machine"}},
                }
            ]
        )
        result = responses[0]["result"]
        self.assertTrue(result["isError"])
        self.assertIn("machine id", result["content"][0]["text"])

    def test_unknown_method_errors(self):
        responses = self.run_server(
            [{"jsonrpc": "2.0", "id": 5, "method": "resources/list"}]
        )
        self.assertEqual(responses[0]["error"]["code"], -32601)

    def test_tool_call_unreachable_guest_is_tool_error(self):
        responses = self.run_server(
            [
                {
                    "jsonrpc": "2.0",
                    "id": 9,
                    "method": "tools/call",
                    "params": {"name": "v9x_ping", "arguments": {}},
                }
            ]
        )
        result = responses[0]["result"]
        self.assertTrue(result["isError"])
        self.assertIn("cannot reach the guest agent", result["content"][0]["text"])


class RelayOpenTests(unittest.TestCase):
    """RelaySession.open against a stubbed server: owner, label, take-over, busy."""

    def make_config(self, replies):
        import v9x_relay
        config = v9x_relay.RelayConfig(server="http://test", operator_key="k",
                                       signing_key=__file__, signer=__file__)
        calls = []

        def request(op, params=None, body=None, method=None, timeout=40.0):
            calls.append((op, dict(params or {})))
            status, value = replies.pop(0)
            return status, {}, json.dumps(value).encode("utf-8")

        config.request = request
        return config, calls

    def test_busy_refusal_raises_busy_error(self):
        import v9x_relay
        config, calls = self.make_config([(409, {
            "success": False, "busy": True,
            "message": "Machine M-ZJQ2DX is busy: another caller ('wait-window') holds its relay session"})])
        with self.assertRaises(v9x_relay.RelayBusyError) as caught:
            v9x_relay.RelaySession(config, "m-zjq2dx", owner="a" * 32, label="v9x_screenshot")
        self.assertIn("busy", str(caught.exception))
        op, params = calls[0]
        self.assertEqual(op, "open")
        self.assertEqual(params["machine"], "M-ZJQ2DX")
        self.assertEqual(params["owner"], "a" * 32)
        self.assertEqual(params["label"], "v9x_screenshot")
        self.assertNotIn("replace", params)

    def test_take_over_sends_replace_and_opens(self):
        import v9x_relay
        config, calls = self.make_config([
            (200, {"success": True, "session": "b" * 32, "install_id": "x", "state": "open",
                   "nonce": "c" * 32}),
            (200, {"success": True}),
        ])
        session = v9x_relay.RelaySession(config, "M-ZJQ2DX", take_over=True)
        self.assertEqual(calls[0][1]["replace"], "1")
        self.assertRegex(calls[0][1]["owner"], "^[0-9a-f]{32}$")
        self.assertEqual(session.nonce, "c" * 32)
        session.close()

    def test_tool_call_strips_take_over_argument(self):
        handler = v9x_mcp.ToolHandler("127.0.0.1", 1, 1.0)
        seen = {}
        handler.v9x_probe_args = lambda args: seen.update(args) or {}
        handler.call("v9x_probe_args", {"relay_take_over": True, "x": 1})
        self.assertEqual(seen, {"x": 1})
        self.assertTrue(handler.relay_take_over)
        self.assertEqual(handler.relay_label, "v9x_probe_args")


class InputEncodingTests(unittest.TestCase):
    class FakeSocket:
        def __init__(self):
            self.sent = bytearray()

        def sendall(self, data):
            self.sent += data

        def settimeout(self, _value):
            pass

        def recv(self, _length):
            return b""  # forces a clean "peer closed" after we captured the send

    def make_client(self):
        client = v9x_mcp.V9xClient.__new__(v9x_mcp.V9xClient)
        client.request_id = 10
        client.hello = {"capabilities": v9x_mcp.CAP_INPUT, "build_id": "fixture"}
        client.sock = self.FakeSocket()
        return client

    def payload_of(self, client):
        # Skip the 24-byte header; the rest is the INPUT payload.
        return bytes(client.sock.sent[v9x_mcp.HEADER.size :])

    def test_move_and_click_batch(self):
        client = self.make_client()
        actions = [
            {"op": "move", "x": 100, "y": 200},
            {"op": "button", "button": 0, "down": True},
            {"op": "button", "button": 0, "down": False},
        ]
        # Bypass the network round trip: only inspect what we send.
        try:
            client.send_input(actions)
        except v9x_mcp.V9xError:
            pass  # recv side is not wired; we assert on the sent bytes
        payload = self.payload_of(client)
        self.assertEqual(struct.unpack_from("<H", payload, 0)[0], 3)
        self.assertEqual(payload[2], v9x_mcp.INPUT_OP_MOUSE_MOVE)
        self.assertEqual(struct.unpack_from("<ii", payload, 3), (100, 200))
        self.assertEqual(payload[11], v9x_mcp.INPUT_OP_MOUSE_BUTTON)

    def test_type_rejects_non_ascii(self):
        client = self.make_client()
        with self.assertRaises(v9x_mcp.V9xError):
            client.send_input([{"op": "type", "text": "café"}])

    def test_virtual_key_names(self):
        self.assertEqual(v9x_mcp.virtual_key("ENTER"), 0x0D)
        self.assertEqual(v9x_mcp.virtual_key("ctrl"), 0x11)
        self.assertEqual(v9x_mcp.virtual_key("A"), 0x41)
        self.assertEqual(v9x_mcp.virtual_key("7"), 0x37)
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.virtual_key("NOPE")


class InputScreenshotTests(unittest.TestCase):
    """screenshot=true on an input tool: input, settle, capture, one connection."""

    BMP = (
        b"BM" + struct.pack("<IHHI", 62, 0, 0, 54)
        + struct.pack("<IiiHHIIiiII", 40, 1, 1, 1, 24, 0, 8, 0, 0, 0, 0)
        + b"\x00\x00\xff\x00"
    )

    class FakeClient:
        def __init__(self, calls):
            self.calls = calls

        def __enter__(self):
            return self

        def __exit__(self, *_exc):
            self.calls.append("close")

        def send_input(self, actions):
            self.calls.append(("input", actions))
            return {"actions_performed": len(actions), "cursor_x": 5, "cursor_y": 6}

        def screenshot(self, **shape):
            self.calls.append(("screenshot", shape))
            return {"width": 1, "height": 1}, InputScreenshotTests.BMP

    def make_handler(self):
        calls = []
        handler = v9x_mcp.ToolHandler("127.0.0.1", 1, 0.1)
        handler.connect = lambda: self.FakeClient(calls)
        return handler, calls

    def test_without_screenshot_is_text_only(self):
        handler, calls = self.make_handler()
        content = handler.call("v9x_key", {"keys": "ENTER"})
        self.assertEqual([part["type"] for part in content], ["text"])
        self.assertNotIn("screenshot", calls)

    def test_screenshot_follows_input_on_same_connection(self):
        handler, calls = self.make_handler()
        content = handler.call("v9x_click", {"x": 10, "y": 20, "screenshot": True, "settle_ms": 0})
        self.assertEqual([part["type"] for part in content], ["text", "text", "image"])
        self.assertEqual([c if isinstance(c, str) else c[0] for c in calls],
                         ["input", "screenshot", "close"])
        self.assertEqual(json.loads(content[0]["text"])["settle_ms"], 0)
        self.assertEqual(calls[1][1], {"region": None, "scale_percent": 100, "gray": False, "window": None})

    def test_shape_arguments_reach_the_capture(self):
        handler, calls = self.make_handler()
        handler.call("v9x_key", {"keys": "ENTER", "screenshot": True, "settle_ms": 0,
                                 "region": [10, 20, 300, 200], "scale": 0.5, "gray": True, "window": None})
        self.assertEqual(calls[1][1], {"region": [10, 20, 300, 200], "scale_percent": 50,
                                       "gray": True, "window": None})

    def test_bad_region_is_refused_before_input(self):
        handler, calls = self.make_handler()
        with self.assertRaises(v9x_mcp.V9xError):
            handler.call("v9x_key", {"keys": "ENTER", "screenshot": True, "region": [1, 2]})
        self.assertEqual(calls, [])

    def test_settle_ms_range(self):
        handler, _ = self.make_handler()
        with self.assertRaises(v9x_mcp.V9xError):
            handler.call("v9x_type", {"text": "x", "screenshot": True, "settle_ms": 999999})

    def test_input_tools_advertise_screenshot(self):
        for tool in v9x_mcp.TOOLS:
            props = tool["inputSchema"]["properties"]
            self.assertEqual("screenshot" in props, tool["name"] in v9x_mcp.INPUT_TOOLS,
                             tool["name"])


class InfoInstallIdTests(unittest.TestCase):
    def build_info(self, install_id=None):
        payload = struct.pack("<IIIHHI", 3, 1000, 0x4003, 9869, 0x0101, 0x0A04)
        for value in ("0.8.0", "b", "WIN98", "Windows 4.10", "S", "W", "C", ""):
            payload += v9x_mcp.encode_string(value)
        payload += bytes(16)
        payload += v9x_mcp.encode_string("0.0.0.0") + v9x_mcp.encode_string("")
        if install_id is not None:
            payload += v9x_mcp.encode_string(install_id)
        return payload

    def test_install_id_is_optional(self):
        self.assertEqual(v9x_mcp.parse_info(self.build_info())["install_id"], "")
        info = v9x_mcp.parse_info(self.build_info(MDNS_FIXTURE_ID))
        self.assertEqual(info["install_id"], MDNS_FIXTURE_ID)
        self.assertEqual(info["computer_name"], "WIN98")

    def test_hello_never_takes_an_install_id(self):
        hello = HelloParseTests().build_hello() + v9x_mcp.encode_string(MDNS_FIXTURE_ID)
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.parse_hello(hello)


MDNS_FIXTURE_ID = "1a2b3c4d-0000-4000-8000-000000000001"


def mdns_fixture():
    """The announcement src/common/mdns.c builds for test_protocol.c's fixture
    service (build-host-tests.ps1 keeps the file in step with the encoder)."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests", "host",
                        "mdns-announce.hex")
    with open(path, "r", encoding="ascii") as handle:
        return bytes.fromhex(handle.read().strip())


def localised_fixture(tcp_port):
    """The fixture with its A record at 127.0.0.1 and SRV port set."""
    packet = bytearray(mdns_fixture())
    packet[-4:] = bytes([127, 0, 0, 1])
    at = packet.find(b"\x00\x21\x80\x01")
    packet[at + 14 : at + 16] = struct.pack(">H", tcp_port)
    return bytes(packet)


class MdnsCodecTests(unittest.TestCase):
    def test_announcement_folds_into_one_agent(self):
        packet = v9x_mcp.parse_mdns_packet(mdns_fixture())
        self.assertTrue(packet["is_response"])
        self.assertEqual(len(packet["records"]), 5)
        agents = v9x_mcp.fold_mdns_replies([("10.1.1.1", packet)])
        self.assertEqual(len(agents), 1)
        agent = agents[0]
        self.assertEqual(agent["instance"], "WIN98-1a2b3c4d")
        self.assertEqual(agent["install_id"], MDNS_FIXTURE_ID)
        self.assertEqual(agent["address"], "192.168.1.50")
        self.assertEqual(agent["port"], 9869)
        self.assertEqual(agent["agent_version"], "0.8.0")
        self.assertEqual(agent["responded_from"], "10.1.1.1")

    def test_goodbye_removes_agent(self):
        packet = v9x_mcp.parse_mdns_packet(mdns_fixture())
        goodbye = v9x_mcp.parse_mdns_packet(mdns_fixture())
        for record in goodbye["records"]:
            record["ttl"] = 0
        self.assertEqual(v9x_mcp.fold_mdns_replies([("a", packet), ("a", goodbye)]), [])

    def test_query_and_compression(self):
        query = v9x_mcp.mdns_query(0x1234)
        self.assertEqual(len(query), 39)
        self.assertEqual(query[:2], b"\x12\x34")
        self.assertEqual(query[-4:], b"\x00\x0c\x00\x01")
        pointer = (bytes([0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0]) + query[12:35]
                   + bytes([0, 12, 0, 1, 0, 0, 0, 10, 0, 2, 0xC0, 12]))
        record = v9x_mcp.parse_mdns_packet(pointer)["records"][0]
        self.assertEqual(record["data"], "_v9xremote._tcp.local")
        loop = bytes([0, 0, 0x84, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0xC0, 12, 0, 12, 0, 1,
                      0, 0, 0, 10, 0, 0])
        with self.assertRaises(v9x_mcp.V9xError):
            v9x_mcp.parse_mdns_packet(loop)


class DiscoveryTests(unittest.TestCase):
    def responder(self, answer, rounds=1):
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.bind(("127.0.0.1", 0))
        sock.settimeout(10)

        def serve():
            try:
                for _ in range(rounds):
                    request, source = sock.recvfrom(2048)
                    if b"_v9xremote" in request and not request[2] & 0x80:
                        sock.sendto(answer, source)
            except OSError:
                pass
            finally:
                sock.close()

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        return "127.0.0.1:%d" % sock.getsockname()[1], thread

    def test_discover_agents_unicast(self):
        target, thread = self.responder(localised_fixture(4242))
        agents = v9x_mcp.discover_agents(2.0, target, MDNS_FIXTURE_ID)
        thread.join(5)
        self.assertEqual(len(agents), 1)
        self.assertEqual(agents[0]["address"], "127.0.0.1")
        self.assertEqual(agents[0]["port"], 4242)

    def test_resolve_by_install_id(self):
        target, thread = self.responder(localised_fixture(4243))
        handler = v9x_mcp.ToolHandler("127.0.0.1", 9869, 1.0, install_id=MDNS_FIXTURE_ID,
                                      query_address=target, discover_seconds=2.0)
        self.assertEqual(handler.resolve(), ("127.0.0.1", 4243))
        thread.join(5)

    def test_resolve_keeps_the_loopback_guard(self):
        target, thread = self.responder(mdns_fixture())
        handler = v9x_mcp.ToolHandler("127.0.0.1", 9869, 1.0, install_id=MDNS_FIXTURE_ID,
                                      query_address=target, discover_seconds=2.0)
        with self.assertRaises(v9x_mcp.V9xError) as caught:
            handler.resolve()
        self.assertIn("--allow-remote", str(caught.exception))
        thread.join(5)

    def test_unknown_install_id(self):
        target, thread = self.responder(localised_fixture(4244))
        handler = v9x_mcp.ToolHandler("127.0.0.1", 9869, 1.0,
                                      install_id="ffffffff-0000-4000-8000-000000000000",
                                      query_address=target, discover_seconds=0.5)
        with self.assertRaises(v9x_mcp.V9xError):
            handler.resolve()
        thread.join(5)


class LoopbackGuardTests(unittest.TestCase):
    def test_loopback_names(self):
        self.assertTrue(v9x_mcp.is_loopback("127.0.0.1"))
        self.assertTrue(v9x_mcp.is_loopback("localhost"))
        self.assertTrue(v9x_mcp.is_loopback("127.9.9.9"))
        self.assertFalse(v9x_mcp.is_loopback("192.168.1.10"))
        self.assertFalse(v9x_mcp.is_loopback("example.com"))


if __name__ == "__main__":
    unittest.main()
