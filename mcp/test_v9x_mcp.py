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
        self.assertEqual(len(tools), 28)
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
