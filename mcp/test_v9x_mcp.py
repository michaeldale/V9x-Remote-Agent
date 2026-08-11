"""Offline tests for the V9x MCP server: codecs, BMP->PNG, JSON-RPC loop."""

import io
import json
import struct
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
        self.assertIn("v9x_click", names)
        self.assertIn("v9x_key", names)
        self.assertEqual(len(tools), 20)

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


class LoopbackGuardTests(unittest.TestCase):
    def test_loopback_names(self):
        self.assertTrue(v9x_mcp.is_loopback("127.0.0.1"))
        self.assertTrue(v9x_mcp.is_loopback("localhost"))
        self.assertTrue(v9x_mcp.is_loopback("127.9.9.9"))
        self.assertFalse(v9x_mcp.is_loopback("192.168.1.10"))
        self.assertFalse(v9x_mcp.is_loopback("example.com"))


if __name__ == "__main__":
    unittest.main()
