#!/usr/bin/env python3
"""MCP server for the V9x Remote Agent.

Exposes a Windows 9x guest running V9XAGNT.EXE as MCP tools over stdio, so
any MCP client (Claude Code, Claude Desktop, others) can execute programs,
transfer files, take screenshots, and perform proven reboots.

Standard library only; Python 3.9+. The V9XR wire protocol is implemented
natively (see docs/protocol.md), so no PowerShell is required and the server
runs on Windows, macOS, and Linux hosts.

The guest protocol is UNAUTHENTICATED: anyone who can reach the agent port
controls the guest. This server therefore refuses non-loopback hosts unless
--allow-remote is passed explicitly.
"""

import argparse
import base64
import json
import os
import re
import socket
import struct
import sys
import time
import zlib

PROTOCOL_VERSION = 0x0100
MAGIC = b"V9XR"
HEADER = struct.Struct("<4sHHIIII")
MAX_PAYLOAD = 65536
CHUNK_SIZE = 32768
MAX_FILE = 64 * 1024 * 1024
DEFAULT_SCREENSHOT = "C:\\V9XREMOTE\\TEMP\\SCREEN.BMP"

MSG_HELLO = 0x0001
MSG_HELLO_RESPONSE = 0x8001
MSG_PING = 0x0002
MSG_PING_RESPONSE = 0x8002
MSG_EXEC = 0x0010
MSG_EXEC_ACCEPTED = 0x8010
MSG_EXEC_COMPLETE = 0x8011
MSG_EXEC_STDOUT = 0x9010
MSG_EXEC_STDERR = 0x9011
MSG_FILE_STAT = 0x0020
MSG_FILE_STAT_RESPONSE = 0x8020
MSG_FILE_LIST = 0x0021
MSG_FILE_LIST_RESPONSE = 0x8021
MSG_FILE_MKDIR = 0x0022
MSG_FILE_MKDIR_RESPONSE = 0x8022
MSG_FILE_OPEN_WRITE = 0x0023
MSG_FILE_WRITE_READY = 0x8023
MSG_FILE_WRITE_CHUNK = 0x0024
MSG_FILE_WRITE_ACK = 0x8024
MSG_FILE_COMMIT = 0x0025
MSG_FILE_WRITE_COMPLETE = 0x8025
MSG_FILE_OPEN_READ = 0x0026
MSG_FILE_READ_COMPLETE = 0x8026
MSG_FILE_READ_CHUNK = 0x9026
MSG_INFO = 0x0030
MSG_INFO_RESPONSE = 0x8030
MSG_REBOOT = 0x0040
MSG_REBOOT_ACCEPTED = 0x8040
MSG_SHUTDOWN = 0x0041
MSG_SHUTDOWN_ACCEPTED = 0x8041
MSG_SCREENSHOT = 0x0050
MSG_SCREENSHOT_RESPONSE = 0x8050
MSG_INPUT = 0x0060
MSG_INPUT_RESPONSE = 0x8060
MSG_ERROR = 0x8FFF

CAP_INFO = 0x0001
CAP_EXEC = 0x0004
CAP_SHELL = 0x0008
CAP_FILE_READ = 0x0020
CAP_FILE_WRITE = 0x0040
CAP_POWER = 0x0080
CAP_SCREENSHOT = 0x0100
CAP_INPUT = 0x0400
CAP_EXEC_DETACH = 0x0800

INPUT_OP_MOUSE_MOVE = 1
INPUT_OP_MOUSE_BUTTON = 2
INPUT_OP_MOUSE_WHEEL = 3
INPUT_OP_KEY = 4
INPUT_OP_TYPE = 5
INPUT_OP_DELAY = 6

BUTTONS = {"left": 0, "right": 1, "middle": 2}

VK_NAMES = {
    "ENTER": 0x0D, "RETURN": 0x0D, "TAB": 0x09, "ESC": 0x1B, "ESCAPE": 0x1B,
    "SPACE": 0x20, "BACKSPACE": 0x08, "BKSP": 0x08, "DELETE": 0x2E, "DEL": 0x2E,
    "INSERT": 0x2D, "INS": 0x2D, "HOME": 0x24, "END": 0x23,
    "PAGEUP": 0x21, "PGUP": 0x21, "PAGEDOWN": 0x22, "PGDN": 0x22,
    "UP": 0x26, "DOWN": 0x28, "LEFT": 0x25, "RIGHT": 0x27,
    "CTRL": 0x11, "CONTROL": 0x11, "ALT": 0x12, "MENU": 0x12, "SHIFT": 0x10,
    "WIN": 0x5B, "LWIN": 0x5B, "RWIN": 0x5C,
    "F1": 0x70, "F2": 0x71, "F3": 0x72, "F4": 0x73, "F5": 0x74, "F6": 0x75,
    "F7": 0x76, "F8": 0x77, "F9": 0x78, "F10": 0x79, "F11": 0x7A, "F12": 0x7B,
}


def virtual_key(name):
    upper = name.strip().upper()
    if upper in VK_NAMES:
        return VK_NAMES[upper]
    if len(upper) == 1 and (upper.isalpha() or upper.isdigit()):
        return ord(upper)
    raise V9xError("unknown key name '%s'" % name)

EXEC_FLAG_STDOUT_TRUNCATED = 0x01
EXEC_FLAG_STDERR_TRUNCATED = 0x02
EXEC_FLAG_TIMED_OUT = 0x04
EXEC_FLAG_CANCELLED = 0x08
EXEC_FLAG_PIPE_CAPTURE = 0x10
EXEC_FLAG_GUI_WINDOW = 0x20
EXEC_FLAG_DETACHED = 0x40
EXEC_FLAG_ORPHANED = 0x80

EXEC_OPTION_DETACH = 0x0001

EXEC_RESULT_NAMES = {
    0: "ok",
    1: "create-failed",
    2: "timeout",
    3: "cancelled",
    4: "internal",
}


class V9xError(Exception):
    """Transport or protocol failure."""


class V9xGuestError(V9xError):
    def __init__(self, status, native_error, detail):
        super().__init__(
            "guest error %d (Win32 %d): %s" % (status, native_error, detail)
        )
        self.status = status
        self.native_error = native_error
        self.detail = detail


def encode_string(value):
    if not isinstance(value, str):
        raise V9xError("protocol strings must be str")
    for character in value:
        code = ord(character)
        if code == 0 or code > 127:
            raise V9xError("protocol strings must be non-NUL 7-bit ASCII")
    raw = value.encode("ascii")
    if len(raw) > 65535:
        raise V9xError("string exceeds protocol limit")
    return struct.pack("<H", len(raw)) + raw


def parse_string(payload, offset):
    if offset + 2 > len(payload):
        raise V9xError("truncated protocol string length")
    (length,) = struct.unpack_from("<H", payload, offset)
    offset += 2
    if offset + length > len(payload):
        raise V9xError("truncated protocol string")
    return payload[offset : offset + length].decode("ascii"), offset + length


def parse_desktop_block(payload, offset):
    values = {
        "pending_job": "",
        "desktop_ready": False,
        "screen_width": 0,
        "screen_height": 0,
        "bits_per_pixel": 0,
        "listen_address": "",
        "allowed_client": "",
    }
    if offset < len(payload):
        values["pending_job"], offset = parse_string(payload, offset)
    if offset + 16 <= len(payload):
        values["desktop_ready"] = bool(payload[offset])
        (
            values["screen_width"],
            values["screen_height"],
            values["bits_per_pixel"],
        ) = struct.unpack_from("<III", payload, offset + 4)
        offset += 16
    if offset < len(payload):
        values["listen_address"], offset = parse_string(payload, offset)
    if offset < len(payload):
        values["allowed_client"], offset = parse_string(payload, offset)
    if offset != len(payload):
        raise V9xError("response contains trailing data")
    return values


def parse_hello(payload):
    if len(payload) < 22:
        raise V9xError("truncated HELLO response")
    version, _reserved, capabilities, max_payload, boot_counter, port, winsock = (
        struct.unpack_from("<HHIIIHH", payload, 0)
    )
    build_id, offset = parse_string(payload, 20)
    values = parse_desktop_block(payload, offset)
    values.update(
        {
            "protocol_version": version,
            "capabilities": capabilities,
            "max_payload": max_payload,
            "boot_counter": boot_counter,
            "port": port,
            "winsock_version": winsock,
            "build_id": build_id,
        }
    )
    return values


def parse_info(payload):
    if len(payload) < 20:
        raise V9xError("truncated INFO response")
    boot_counter, uptime, capabilities, port, winsock, raw_version = (
        struct.unpack_from("<IIIHHI", payload, 0)
    )
    offset = 20
    strings = {}
    for name in (
        "agent_version",
        "build_id",
        "computer_name",
        "windows_version",
        "system_directory",
        "windows_directory",
        "current_directory",
    ):
        strings[name], offset = parse_string(payload, offset)
    values = parse_desktop_block(payload, offset)
    values.update(strings)
    values.update(
        {
            "boot_counter": boot_counter,
            "uptime_ms": uptime,
            "capabilities": capabilities,
            "port": port,
            "winsock_version": winsock,
            "raw_windows_version": raw_version,
        }
    )
    return values


def bmp_to_png(data):
    """Convert a bottom-up 24-bit BI_RGB BMP (the agent's screenshot format)
    to a PNG, standard library only."""
    if len(data) < 54 or data[:2] != b"BM":
        raise V9xError("not a BMP file")
    (pixel_offset,) = struct.unpack_from("<I", data, 10)
    _header_size, width, height, _planes, bpp, compression = struct.unpack_from(
        "<IiiHHI", data, 14
    )
    if bpp != 24 or compression != 0:
        raise V9xError("only 24-bit uncompressed BMP is supported")
    if width <= 0 or height == 0:
        raise V9xError("invalid BMP dimensions")
    bottom_up = height > 0
    rows = abs(height)
    stride = (width * 3 + 3) & ~3
    if pixel_offset + stride * rows > len(data):
        raise V9xError("truncated BMP pixel data")
    scanlines = []
    for y in range(rows):
        source_y = rows - 1 - y if bottom_up else y
        start = pixel_offset + source_y * stride
        row = data[start : start + width * 3]
        rgb = bytearray(width * 3)
        rgb[0::3] = row[2::3]
        rgb[1::3] = row[1::3]
        rgb[2::3] = row[0::3]
        scanlines.append(b"\x00" + bytes(rgb))
    raw = b"".join(scanlines)

    def chunk(tag, body):
        return (
            struct.pack(">I", len(body))
            + tag
            + body
            + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, rows, 8, 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )


class V9xClient:
    """One connection to the guest agent: connect, HELLO, operate, close."""

    def __init__(self, host, port, timeout=15.0, label="v9x-mcp"):
        self.timeout = timeout
        self.request_id = int(time.time() * 1000) % 0x7FFFFFF0 + 1
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        payload = struct.pack("<HH", PROTOCOL_VERSION, PROTOCOL_VERSION)
        payload += encode_string(label)
        self.send(MSG_HELLO, payload)
        frame = self.expect((MSG_HELLO_RESPONSE,))
        self.hello = parse_hello(frame[3])
        if self.hello["protocol_version"] != PROTOCOL_VERSION:
            raise V9xError("no compatible protocol version")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()

    def require(self, capability, name):
        if not self.hello["capabilities"] & capability:
            raise V9xError(
                "agent build '%s' does not advertise %s support"
                % (self.hello["build_id"], name)
            )

    def next_id(self):
        self.request_id = (self.request_id % 0x7FFFFFFE) + 1
        return self.request_id

    def send(self, msg_type, payload=b""):
        if len(payload) > MAX_PAYLOAD:
            raise V9xError("payload exceeds protocol limit")
        header = HEADER.pack(
            MAGIC, PROTOCOL_VERSION, msg_type, self.request_id, 0, len(payload), 0
        )
        self.sock.sendall(header + payload)

    def read_exact(self, length):
        chunks = []
        remaining = length
        while remaining:
            data = self.sock.recv(remaining)
            if not data:
                raise V9xError("peer closed the connection")
            chunks.append(data)
            remaining -= len(data)
        return b"".join(chunks)

    def recv(self):
        header = self.read_exact(HEADER.size)
        magic, version, msg_type, request_id, flags, length, reserved = HEADER.unpack(
            header
        )
        if magic != MAGIC:
            raise V9xError("invalid V9XR frame magic")
        if length > MAX_PAYLOAD:
            raise V9xError("peer payload exceeds protocol limit")
        if reserved != 0:
            raise V9xError("reserved header field is nonzero")
        payload = self.read_exact(length) if length else b""
        return version, msg_type, request_id, payload, flags

    def expect(self, types):
        version, msg_type, request_id, payload, flags = self.recv()
        if request_id != self.request_id:
            raise V9xError("response request ID does not match")
        if msg_type == MSG_ERROR:
            if len(payload) < 10:
                raise V9xError("truncated ERROR_RESPONSE")
            status, native = struct.unpack_from("<II", payload, 0)
            detail, _ = parse_string(payload, 8)
            raise V9xGuestError(status, native, detail)
        if msg_type not in types:
            raise V9xError("unexpected response type 0x%04x" % msg_type)
        return version, msg_type, request_id, payload

    # Operations -----------------------------------------------------------

    def ping(self):
        self.next_id()
        self.send(MSG_PING)
        _, _, _, payload = self.expect((MSG_PING_RESPONSE,))
        uptime, boot = struct.unpack_from("<II", payload, 0)
        return {
            "uptime_ms": uptime,
            "boot_counter": boot,
            "build_id": self.hello["build_id"],
        }

    def info(self):
        self.require(CAP_INFO, "information")
        self.next_id()
        self.send(MSG_INFO)
        _, _, _, payload = self.expect((MSG_INFO_RESPONSE,))
        return parse_info(payload)

    def execute(
        self,
        mode,
        application="",
        arguments="",
        working_dir="",
        timeout_ms=60000,
        show_window=False,
        output_limit=262144,
        detach=False,
    ):
        self.require(CAP_EXEC if mode == 0 else CAP_SHELL, "execution")
        if detach:
            self.require(CAP_EXEC_DETACH, "detached execution")
        payload = struct.pack(
            "<BBHIII", mode, 1 if show_window else 0,
            EXEC_OPTION_DETACH if detach else 0, timeout_ms, output_limit,
            output_limit,
        )
        payload += encode_string(application)
        payload += encode_string(arguments)
        payload += encode_string(working_dir)
        self.sock.settimeout(timeout_ms / 1000.0 + 20.0)
        try:
            self.next_id()
            self.send(MSG_EXEC, payload)
            _, _, _, accepted = self.expect((MSG_EXEC_ACCEPTED,))
            if len(accepted) != 4:
                raise V9xError("invalid EXEC_ACCEPTED payload")
            stdout = bytearray()
            stderr = bytearray()
            while True:
                _, msg_type, _, data = self.expect(
                    (MSG_EXEC_STDOUT, MSG_EXEC_STDERR, MSG_EXEC_COMPLETE)
                )
                if msg_type == MSG_EXEC_STDOUT:
                    stdout.extend(data)
                elif msg_type == MSG_EXEC_STDERR:
                    stderr.extend(data)
                else:
                    break
            if len(data) != 28:
                raise V9xError("invalid EXEC_COMPLETE payload")
            result, exit_code, native, elapsed, out_total, err_total, flags = (
                struct.unpack("<7I", data)
            )
            return {
                "result": EXEC_RESULT_NAMES.get(result, str(result)),
                "exit_code": exit_code if exit_code < 0x80000000 else exit_code - 0x100000000,
                "native_error": native,
                "elapsed_ms": elapsed,
                "stdout": stdout.decode("ascii", "replace"),
                "stderr": stderr.decode("ascii", "replace"),
                "stdout_bytes": out_total,
                "stderr_bytes": err_total,
                "stdout_truncated": bool(flags & EXEC_FLAG_STDOUT_TRUNCATED),
                "stderr_truncated": bool(flags & EXEC_FLAG_STDERR_TRUNCATED),
                "timed_out": bool(flags & EXEC_FLAG_TIMED_OUT),
                "cancelled": bool(flags & EXEC_FLAG_CANCELLED),
                "gui_window": bool(flags & EXEC_FLAG_GUI_WINDOW),
                "detached": bool(flags & EXEC_FLAG_DETACHED),
                "orphaned": bool(flags & EXEC_FLAG_ORPHANED),
            }
        finally:
            self.sock.settimeout(self.timeout)

    def stat(self, guest_path):
        self.require(CAP_FILE_READ, "file")
        self.next_id()
        self.send(MSG_FILE_STAT, encode_string(guest_path))
        _, _, _, payload = self.expect((MSG_FILE_STAT_RESPONSE,))
        if len(payload) != 16:
            raise V9xError("invalid FILE_STAT_RESPONSE payload")
        size, attributes, native = struct.unpack_from("<III", payload, 4)
        return {
            "path": guest_path,
            "exists": bool(payload[0]),
            "is_directory": bool(payload[1]),
            "size": size,
            "attributes": attributes,
            "native_error": native,
        }

    def list_dir(self, guest_path):
        self.require(CAP_FILE_READ, "file")
        self.next_id()
        self.send(MSG_FILE_LIST, encode_string(guest_path))
        _, _, _, payload = self.expect((MSG_FILE_LIST_RESPONSE,))
        if len(payload) < 4:
            raise V9xError("truncated FILE_LIST_RESPONSE")
        (count,) = struct.unpack_from("<I", payload, 0)
        offset = 4
        entries = []
        for _ in range(count):
            if offset + 8 > len(payload):
                raise V9xError("truncated directory entry")
            attributes, size = struct.unpack_from("<II", payload, offset)
            offset += 8
            name, offset = parse_string(payload, offset)
            entries.append(
                {
                    "name": name,
                    "size": size,
                    "attributes": attributes,
                    "is_directory": bool(attributes & 0x10),
                }
            )
        if offset != len(payload):
            raise V9xError("directory listing contains trailing data")
        return entries

    def mkdir(self, guest_path):
        self.require(CAP_FILE_WRITE, "file-write")
        self.next_id()
        self.send(MSG_FILE_MKDIR, encode_string(guest_path))
        _, _, _, payload = self.expect((MSG_FILE_MKDIR_RESPONSE,))
        if len(payload) != 4:
            raise V9xError("invalid FILE_MKDIR_RESPONSE payload")
        return {"path": guest_path, "created": bool(struct.unpack("<I", payload)[0])}

    def put_bytes(self, data, guest_path):
        self.require(CAP_FILE_WRITE, "file-write")
        if len(data) > MAX_FILE:
            raise V9xError("file exceeds the 64 MiB protocol limit")
        crc = zlib.crc32(data) & 0xFFFFFFFF
        self.next_id()
        self.send(
            MSG_FILE_OPEN_WRITE,
            struct.pack("<II", len(data), crc) + encode_string(guest_path),
        )
        _, _, _, ready = self.expect((MSG_FILE_WRITE_READY,))
        if ready != struct.pack("<II", len(data), crc):
            raise V9xError("FILE_WRITE_READY did not echo size and CRC32")
        offset = 0
        while offset < len(data):
            piece = data[offset : offset + CHUNK_SIZE]
            self.send(MSG_FILE_WRITE_CHUNK, struct.pack("<I", offset) + piece)
            _, _, _, ack = self.expect((MSG_FILE_WRITE_ACK,))
            offset += len(piece)
            if ack != struct.pack("<I", offset):
                raise V9xError("upload acknowledgement offset mismatch")
        self.send(MSG_FILE_COMMIT)
        _, _, _, done = self.expect((MSG_FILE_WRITE_COMPLETE,))
        if done != struct.pack("<II", len(data), crc):
            raise V9xError("commit did not confirm size and CRC32")
        return {"guest_path": guest_path, "size": len(data), "crc32": "%08X" % crc}

    def get_bytes(self, guest_path):
        self.require(CAP_FILE_READ, "file")
        self.next_id()
        self.send(MSG_FILE_OPEN_READ, encode_string(guest_path))
        received = bytearray()
        while True:
            _, msg_type, _, payload = self.expect(
                (MSG_FILE_READ_CHUNK, MSG_FILE_READ_COMPLETE)
            )
            if msg_type == MSG_FILE_READ_CHUNK:
                if len(payload) < 4 or struct.unpack_from("<I", payload, 0)[0] != len(
                    received
                ):
                    raise V9xError("download chunk offset mismatch")
                received.extend(payload[4:])
            else:
                break
        if len(payload) != 8:
            raise V9xError("invalid FILE_READ_COMPLETE payload")
        size, crc = struct.unpack("<II", payload)
        data = bytes(received)
        if size != len(data) or crc != (zlib.crc32(data) & 0xFFFFFFFF):
            raise V9xError("downloaded size or CRC32 does not match the guest")
        return data

    def screenshot(self, guest_path=DEFAULT_SCREENSHOT):
        self.require(CAP_SCREENSHOT, "screenshot")
        self.next_id()
        self.send(MSG_SCREENSHOT, encode_string(guest_path))
        old_timeout = self.timeout
        self.sock.settimeout(max(old_timeout, 60.0))
        try:
            _, _, _, payload = self.expect((MSG_SCREENSHOT_RESPONSE,))
            if len(payload) < 22:
                raise V9xError("truncated SCREENSHOT_RESPONSE")
            width, height, bpp, file_bytes, crc = struct.unpack_from("<IIIII", payload, 0)
            path, offset = parse_string(payload, 20)
            if offset != len(payload):
                raise V9xError("SCREENSHOT_RESPONSE contains trailing data")
            data = self.get_bytes(path)
        finally:
            self.sock.settimeout(old_timeout)
        if len(data) != file_bytes or (zlib.crc32(data) & 0xFFFFFFFF) != crc:
            raise V9xError("screenshot download does not match capture metadata")
        return {
            "width": width,
            "height": height,
            "source_bits_per_pixel": bpp,
            "guest_path": path,
        }, data

    def send_input(self, actions):
        self.require(CAP_INPUT, "input")
        if len(actions) > 256:
            raise V9xError("at most 256 input actions per request")
        payload = struct.pack("<H", len(actions))
        for action in actions:
            op = action["op"]
            if op == "move":
                payload += struct.pack("<Bii", INPUT_OP_MOUSE_MOVE, int(action["x"]), int(action["y"]))
            elif op == "button":
                payload += struct.pack(
                    "<BBB", INPUT_OP_MOUSE_BUTTON, int(action["button"]),
                    1 if action["down"] else 0,
                )
            elif op == "wheel":
                payload += struct.pack("<Bh", INPUT_OP_MOUSE_WHEEL, int(action["notches"]))
            elif op == "key":
                payload += struct.pack(
                    "<BBB", INPUT_OP_KEY, int(action["vk"]), 1 if action["down"] else 0
                )
            elif op == "type":
                text = action["text"]
                for character in text:
                    if ord(character) > 127:
                        raise V9xError("type text must be 7-bit ASCII")
                raw = text.encode("ascii")
                if len(raw) > 65535:
                    raise V9xError("type text exceeds protocol limit")
                payload += struct.pack("<BH", INPUT_OP_TYPE, len(raw)) + raw
            elif op == "delay":
                payload += struct.pack("<BH", INPUT_OP_DELAY, int(action["ms"]))
            else:
                raise V9xError("unknown input action '%s'" % op)
        self.next_id()
        self.send(MSG_INPUT, payload)
        _, _, _, response = self.expect((MSG_INPUT_RESPONSE,))
        if len(response) != 12:
            raise V9xError("invalid INPUT_RESPONSE payload")
        performed, cursor_x, cursor_y = struct.unpack("<Iii", response)
        return {
            "actions_performed": performed,
            "cursor_x": cursor_x,
            "cursor_y": cursor_y,
        }

    def power(self, msg_type, response_type, job_id):
        self.require(CAP_POWER, "power-control")
        token = job_id.encode("ascii")
        if not token or len(token) > 63:
            raise V9xError("job_id must be 1 to 63 ASCII bytes")
        self.next_id()
        self.send(msg_type, encode_string(job_id))
        _, _, _, payload = self.expect((response_type,))
        if len(payload) < 6:
            raise V9xError("truncated power-control acceptance")
        (boot,) = struct.unpack_from("<I", payload, 0)
        echoed, offset = parse_string(payload, 4)
        if offset != len(payload) or echoed != job_id:
            raise V9xError("power-control acceptance token does not match")
        return boot


def wait_for_reboot(host, port, old_boot, job_id, wait_seconds):
    deadline = time.monotonic() + wait_seconds
    last_error = "no connection attempt succeeded"
    while time.monotonic() < deadline:
        time.sleep(0.5)
        try:
            candidate = V9xClient(host, port, timeout=3.0)
        except (OSError, V9xError) as exc:
            last_error = str(exc)
            continue
        try:
            hello = candidate.hello
            if hello["boot_counter"] != old_boot and hello["pending_job"] == job_id:
                return hello
            last_error = "reconnected without proof (boot %d, pending '%s')" % (
                hello["boot_counter"],
                hello["pending_job"],
            )
        finally:
            candidate.close()
    raise V9xError(
        "guest did not reconnect with a new boot counter and resume token '%s' "
        "within %d seconds; last error: %s" % (job_id, wait_seconds, last_error)
    )


# MCP plumbing ---------------------------------------------------------------


def server_version():
    header = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..",
        "include",
        "v9xremote",
        "version.h",
    )
    try:
        with open(header, "r", encoding="ascii") as handle:
            match = re.search(r'V9X_AGENT_VERSION "([^"]+)"', handle.read())
            if match:
                return match.group(1)
    except OSError:
        pass
    return "unknown"


def guest_path_schema(description):
    return {"type": "string", "description": description}


TOOLS = [
    {
        "name": "v9x_ping",
        "description": "Check that the Windows 9x guest agent is alive; returns uptime and the agent-start counter (not reboot proof by itself).",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "v9x_info",
        "description": "Full guest agent status: version, build, computer name, Windows version, screen mode, desktop readiness, pending job.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "v9x_exec",
        "description": "Run a Win32 executable directly in the guest with bounded captured stdout/stderr. Use v9x_shell for COMMAND.COM built-ins and batch files.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "application": guest_path_schema("Full guest path of the EXE, e.g. C:\\V9XREMOTE\\JOBS\\X\\APP.EXE"),
                "arguments": {"type": "string", "description": "Command-line arguments (optional)"},
                "working_dir": {"type": "string", "description": "Guest working directory (optional)"},
                "timeout_ms": {"type": "integer", "description": "Guest-side timeout in ms (default 60000, max 3600000)"},
                "show_window": {"type": "boolean", "description": "Show the child's window (console children; GUI-subsystem EXEs manage their own window)"},
                "detach": {"type": "boolean", "description": "Launch and return immediately without waiting or capturing output. Use for installers and programs that outlive the request."},
            },
            "required": ["application"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_shell",
        "description": "Run a command through COMMAND.COM /C in the guest (DIR, COPY, batch files, VER...).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "The shell command line"},
                "timeout_ms": {"type": "integer", "description": "Guest-side timeout in ms (default 60000)"},
                "detach": {"type": "boolean", "description": "Launch and return immediately without waiting or capturing output. Use instead of START for commands that spawn long-lived programs."},
            },
            "required": ["command"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_put_file",
        "description": "Upload a local file into the guest, CRC32-verified and transactional (64 MiB max).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "local_path": {"type": "string", "description": "Path of the file on this machine"},
                "guest_path": guest_path_schema("Destination path in the guest"),
            },
            "required": ["local_path", "guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_get_file",
        "description": "Download a file from the guest to this machine, CRC32-verified.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "guest_path": guest_path_schema("Path of the file in the guest"),
                "local_path": {"type": "string", "description": "Destination path on this machine"},
            },
            "required": ["guest_path", "local_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_push_tree",
        "description": "Recursively upload a local directory into the guest (mkdir + put for every file).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "local_dir": {"type": "string", "description": "Local directory to upload"},
                "guest_dir": guest_path_schema("Destination directory in the guest"),
            },
            "required": ["local_dir", "guest_dir"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_stat",
        "description": "Existence, size, and attributes of a guest path.",
        "inputSchema": {
            "type": "object",
            "properties": {"guest_path": guest_path_schema("Guest path to inspect")},
            "required": ["guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_list_dir",
        "description": "List a guest directory (bounded to 16 KiB of entries).",
        "inputSchema": {
            "type": "object",
            "properties": {"guest_path": guest_path_schema("Guest directory to list")},
            "required": ["guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_mkdir",
        "description": "Create a guest directory (idempotent).",
        "inputSchema": {
            "type": "object",
            "properties": {"guest_path": guest_path_schema("Guest directory to create")},
            "required": ["guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_click",
        "description": "Click a mouse button in the guest, optionally moving to (x,y) first. Take a v9x_screenshot to find coordinates.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "integer", "description": "Absolute screen X to move to before clicking (optional)"},
                "y": {"type": "integer", "description": "Absolute screen Y to move to before clicking (optional)"},
                "button": {"type": "string", "enum": ["left", "right", "middle"], "description": "Default left"},
                "double": {"type": "boolean", "description": "Double-click if true"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_move",
        "description": "Move the mouse cursor to absolute screen coordinates in the guest.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "integer"},
                "y": {"type": "integer"},
            },
            "required": ["x", "y"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_type",
        "description": "Type ASCII text into the guest's focused window (handles shifted characters; \\n presses Enter, \\t presses Tab).",
        "inputSchema": {
            "type": "object",
            "properties": {"text": {"type": "string", "description": "ASCII text to type"}},
            "required": ["text"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_key",
        "description": "Press a key or a hotkey combination in the guest, e.g. 'ENTER', 'CTRL+ESC' (Start menu), 'ALT+F4', 'CTRL+ALT+DELETE'. Modifiers are held while the final key is pressed.",
        "inputSchema": {
            "type": "object",
            "properties": {"keys": {"type": "string", "description": "Key name or '+'-joined combo"}},
            "required": ["keys"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_scroll",
        "description": "Scroll the mouse wheel in the guest. Positive notches scroll up (away from the user), negative scroll down.",
        "inputSchema": {
            "type": "object",
            "properties": {"notches": {"type": "integer", "description": "Wheel notches, e.g. -3"}},
            "required": ["notches"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_input",
        "description": "Send a raw ordered batch of input actions atomically; use for drags and precise sequences. Each action is an object with an 'op': move {x,y}, button {button,down}, wheel {notches}, key {vk|name,down}, type {text}, delay {ms}.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "actions": {
                    "type": "array",
                    "items": {"type": "object"},
                    "description": "Ordered list of raw action objects",
                }
            },
            "required": ["actions"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_screenshot",
        "description": "Capture a confirmed-stable guest desktop as PNG. Never call during a fullscreen transition or suspected display wedge; use info, trace execution, and file retrieval first.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "save_path": {"type": "string", "description": "Optional local path to also save the PNG to"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_reboot_with_proof",
        "description": "Reboot and verify disconnect/reconnect with a changed agent-start counter and echoed resume token. The counter alone is not reboot proof. Call v9x_wait_desktop afterwards to complete confirmation before GUI work.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "job_id": {"type": "string", "description": "Resume token, 1 to 63 ASCII chars (default: generated)"},
                "wait_seconds": {"type": "integer", "description": "How long to wait for the proof (default 180)"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_shutdown",
        "description": "Cleanly shut down the guest (it will not come back until the VM is started again).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "job_id": {"type": "string", "description": "Resume token, 1 to 63 ASCII chars (default: generated)"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_wait_desktop",
        "description": "Wait until the Windows desktop (Explorer) is ready; required after boot before GUI exec or screenshots.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "wait_seconds": {"type": "integer", "description": "Maximum wait (default 120)"},
            },
            "additionalProperties": False,
        },
    },
]


class ToolHandler:
    def __init__(self, host, port, timeout):
        self.host = host
        self.port = port
        self.timeout = timeout

    def connect(self):
        try:
            return V9xClient(self.host, self.port, timeout=self.timeout)
        except OSError as exc:
            raise V9xError(
                "cannot reach the guest agent at %s:%d (%s); is the VM running "
                "and the port forwarded?" % (self.host, self.port, exc)
            )

    def call(self, name, args):
        method = getattr(self, name, None)
        if method is None:
            raise V9xError("unknown tool: %s" % name)
        return method(args)

    @staticmethod
    def text(value):
        return [{"type": "text", "text": json.dumps(value, indent=1)}]

    def v9x_ping(self, _args):
        with self.connect() as client:
            return self.text(client.ping())

    def v9x_info(self, _args):
        with self.connect() as client:
            return self.text(client.info())

    def v9x_exec(self, args):
        with self.connect() as client:
            return self.text(
                client.execute(
                    0,
                    application=args["application"],
                    arguments=args.get("arguments", ""),
                    working_dir=args.get("working_dir", ""),
                    timeout_ms=int(args.get("timeout_ms", 60000)),
                    show_window=bool(args.get("show_window", False)),
                    detach=bool(args.get("detach", False)),
                )
            )

    def v9x_shell(self, args):
        with self.connect() as client:
            return self.text(
                client.execute(
                    1,
                    arguments=args["command"],
                    timeout_ms=int(args.get("timeout_ms", 60000)),
                    detach=bool(args.get("detach", False)),
                )
            )

    def v9x_put_file(self, args):
        with open(args["local_path"], "rb") as handle:
            data = handle.read()
        with self.connect() as client:
            return self.text(client.put_bytes(data, args["guest_path"]))

    def v9x_get_file(self, args):
        with self.connect() as client:
            data = client.get_bytes(args["guest_path"])
        local_path = os.path.abspath(args["local_path"])
        parent = os.path.dirname(local_path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        with open(local_path, "wb") as handle:
            handle.write(data)
        return self.text(
            {
                "guest_path": args["guest_path"],
                "local_path": local_path,
                "size": len(data),
                "crc32": "%08X" % (zlib.crc32(data) & 0xFFFFFFFF),
            }
        )

    def v9x_push_tree(self, args):
        local_dir = os.path.abspath(args["local_dir"])
        if not os.path.isdir(local_dir):
            raise V9xError("local_dir is not a directory: %s" % local_dir)
        guest_dir = args["guest_dir"].rstrip("\\")
        uploaded = []
        total = 0
        with self.connect() as client:
            client.mkdir(guest_dir)
            for root, dirs, files in os.walk(local_dir):
                dirs.sort()
                relative = os.path.relpath(root, local_dir)
                base = guest_dir
                if relative != ".":
                    base = guest_dir + "\\" + relative.replace("/", "\\")
                    client.mkdir(base)
                for name in sorted(files):
                    with open(os.path.join(root, name), "rb") as handle:
                        data = handle.read()
                    destination = base + "\\" + name
                    client.put_bytes(data, destination)
                    uploaded.append(destination)
                    total += len(data)
        return self.text(
            {"guest_dir": guest_dir, "files": len(uploaded), "bytes": total}
        )

    def v9x_stat(self, args):
        with self.connect() as client:
            return self.text(client.stat(args["guest_path"]))

    def v9x_list_dir(self, args):
        with self.connect() as client:
            return self.text(client.list_dir(args["guest_path"]))

    def v9x_mkdir(self, args):
        with self.connect() as client:
            return self.text(client.mkdir(args["guest_path"]))

    def v9x_click(self, args):
        button = BUTTONS.get(args.get("button", "left"))
        if button is None:
            raise V9xError("unknown mouse button '%s'" % args.get("button"))
        actions = []
        if "x" in args and "y" in args:
            actions.append({"op": "move", "x": args["x"], "y": args["y"]})
        presses = 2 if args.get("double") else 1
        for _ in range(presses):
            actions.append({"op": "button", "button": button, "down": True})
            actions.append({"op": "button", "button": button, "down": False})
        with self.connect() as client:
            return self.text(client.send_input(actions))

    def v9x_move(self, args):
        with self.connect() as client:
            return self.text(
                client.send_input([{"op": "move", "x": args["x"], "y": args["y"]}])
            )

    def v9x_type(self, args):
        with self.connect() as client:
            return self.text(client.send_input([{"op": "type", "text": args["text"]}]))

    def v9x_key(self, args):
        vks = [virtual_key(name) for name in args["keys"].split("+")]
        actions = [{"op": "key", "vk": vk, "down": True} for vk in vks]
        actions += [{"op": "key", "vk": vk, "down": False} for vk in reversed(vks)]
        with self.connect() as client:
            return self.text(client.send_input(actions))

    def v9x_scroll(self, args):
        with self.connect() as client:
            return self.text(
                client.send_input([{"op": "wheel", "notches": args["notches"]}])
            )

    def v9x_input(self, args):
        actions = []
        for raw in args["actions"]:
            action = dict(raw)
            if action.get("op") == "key" and "vk" not in action and "name" in action:
                action["vk"] = virtual_key(action.pop("name"))
            actions.append(action)
        with self.connect() as client:
            return self.text(client.send_input(actions))

    def v9x_screenshot(self, args):
        with self.connect() as client:
            metadata, bmp = client.screenshot()
        png = bmp_to_png(bmp)
        save_path = args.get("save_path")
        if save_path:
            save_path = os.path.abspath(save_path)
            parent = os.path.dirname(save_path)
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(save_path, "wb") as handle:
                handle.write(png)
            metadata["saved_to"] = save_path
        return [
            {"type": "text", "text": json.dumps(metadata, indent=1)},
            {
                "type": "image",
                "data": base64.b64encode(png).decode("ascii"),
                "mimeType": "image/png",
            },
        ]

    def v9x_reboot_with_proof(self, args):
        job_id = args.get("job_id") or ("mcp-%d" % (int(time.time()) % 100000000))
        wait_seconds = int(args.get("wait_seconds", 180))
        with self.connect() as client:
            old_boot = client.hello["boot_counter"]
            client.power(MSG_REBOOT, MSG_REBOOT_ACCEPTED, job_id)
        hello = wait_for_reboot(self.host, self.port, old_boot, job_id, wait_seconds)
        return self.text(
            {
                "rebooted": True,
                "job_id": job_id,
                "previous_boot_counter": old_boot,
                "boot_counter": hello["boot_counter"],
                "pending_job": hello["pending_job"],
                "desktop_ready": hello["desktop_ready"],
            }
        )

    def v9x_shutdown(self, args):
        job_id = args.get("job_id") or ("mcp-%d" % (int(time.time()) % 100000000))
        with self.connect() as client:
            boot = client.power(MSG_SHUTDOWN, MSG_SHUTDOWN_ACCEPTED, job_id)
        return self.text({"accepted": True, "job_id": job_id, "boot_counter": boot})

    def v9x_wait_desktop(self, args):
        wait_seconds = int(args.get("wait_seconds", 120))
        deadline = time.monotonic() + wait_seconds
        with self.connect() as client:
            while True:
                info = client.info()
                if info["desktop_ready"]:
                    return self.text(
                        {
                            "desktop_ready": True,
                            "boot_counter": info["boot_counter"],
                            "screen_width": info["screen_width"],
                            "screen_height": info["screen_height"],
                            "bits_per_pixel": info["bits_per_pixel"],
                        }
                    )
                if time.monotonic() >= deadline:
                    raise V9xError(
                        "desktop did not become ready within %d seconds" % wait_seconds
                    )
                time.sleep(1.0)


class McpServer:
    def __init__(self, handler, stdin=None, stdout=None):
        self.handler = handler
        self.stdin = stdin if stdin is not None else sys.stdin
        self.stdout = stdout if stdout is not None else sys.stdout

    def reply(self, message_id, result=None, error=None):
        response = {"jsonrpc": "2.0", "id": message_id}
        if error is not None:
            response["error"] = error
        else:
            response["result"] = result
        self.stdout.write(json.dumps(response) + "\n")
        self.stdout.flush()

    def handle(self, message):
        method = message.get("method")
        message_id = message.get("id")
        if method == "initialize":
            requested = ""
            params = message.get("params") or {}
            if isinstance(params, dict):
                requested = params.get("protocolVersion") or ""
            self.reply(
                message_id,
                {
                    "protocolVersion": requested or "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {
                        "name": "v9x-remote-agent",
                        "version": server_version(),
                    },
                },
            )
        elif method == "ping":
            self.reply(message_id, {})
        elif method == "tools/list":
            self.reply(message_id, {"tools": TOOLS})
        elif method == "tools/call":
            params = message.get("params") or {}
            name = params.get("name", "")
            arguments = params.get("arguments") or {}
            try:
                content = self.handler.call(name, arguments)
                self.reply(message_id, {"content": content, "isError": False})
            except (V9xError, OSError, KeyError, ValueError) as exc:
                self.reply(
                    message_id,
                    {
                        "content": [{"type": "text", "text": "Error: %s" % exc}],
                        "isError": True,
                    },
                )
        elif message_id is not None:
            self.reply(
                message_id, error={"code": -32601, "message": "method not found"}
            )
        # Notifications (no id) that we do not understand are ignored.

    def run(self):
        for line in self.stdin:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except ValueError:
                continue
            if isinstance(message, dict) and "method" in message:
                self.handle(message)


def is_loopback(host):
    if host in ("localhost", "127.0.0.1", "::1"):
        return True
    try:
        return socket.inet_aton(host)[0] == 127
    except OSError:
        return False


def main(argv=None):
    parser = argparse.ArgumentParser(description="MCP server for the V9x Remote Agent")
    parser.add_argument("--host", default="127.0.0.1", help="guest agent host (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=9869, help="guest agent port (default 9869)")
    parser.add_argument("--timeout", type=float, default=15.0, help="per-operation socket timeout seconds")
    parser.add_argument(
        "--allow-remote",
        action="store_true",
        help="permit a non-loopback --host (the protocol is unauthenticated; know what you are doing)",
    )
    args = parser.parse_args(argv)
    if not is_loopback(args.host) and not args.allow_remote:
        parser.error(
            "refusing non-loopback host '%s' without --allow-remote; the guest "
            "protocol is unauthenticated" % args.host
        )
    sys.stderr.write(
        "v9x-mcp: serving guest %s:%d over MCP stdio; the guest protocol is "
        "unauthenticated, keep the port loopback-only\n" % (args.host, args.port)
    )
    sys.stderr.flush()
    McpServer(ToolHandler(args.host, args.port, args.timeout)).run()


if __name__ == "__main__":
    main()
