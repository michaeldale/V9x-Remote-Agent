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
import fnmatch
import hashlib
import io
import json
import os
import random
import re
import select
import socket
import struct
import sys
import time
import zlib

try:
    from v9x_relay import RelayConfig, RelayError, RelaySession
except ImportError:  # imported as a package (tests)
    from .v9x_relay import RelayConfig, RelayError, RelaySession

PROTOCOL_VERSION = 0x0100
MAGIC = b"V9XR"
HEADER = struct.Struct("<4sHHIIII")
MAX_PAYLOAD = 65536
CHUNK_SIZE = 32768
MAX_FILE = 2147483647  # 2 GiB - 1 (agent 0.11.0+; 256 MiB before)
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
MSG_DOWNLOAD = 0x0027
MSG_DOWNLOAD_PROGRESS = 0x9027
MSG_DOWNLOAD_COMPLETE = 0x8027
MSG_INFO = 0x0030
MSG_INFO_RESPONSE = 0x8030
MSG_REBOOT = 0x0040
MSG_REBOOT_ACCEPTED = 0x8040
MSG_SHUTDOWN = 0x0041
MSG_SHUTDOWN_ACCEPTED = 0x8041
MSG_UPDATE = 0x0042
MSG_UPDATE_ACCEPTED = 0x8042
MSG_RESET = 0x0043
MSG_RESET_ACCEPTED = 0x8043
MSG_SCREENSHOT = 0x0050
MSG_SCREENSHOT_RESPONSE = 0x8050
MSG_INPUT = 0x0060
MSG_INPUT_RESPONSE = 0x8060
MSG_INVENTORY = 0x0070
MSG_INVENTORY_RESPONSE = 0x8070
# File tools, processes, registry (agent 0.13.0+).
MSG_FILE_DELETE = 0x0028
MSG_FILE_DELETE_RESPONSE = 0x8028
MSG_FILE_HASH = 0x0029
MSG_FILE_HASH_RESPONSE = 0x8029
MSG_FILE_READ_TEXT = 0x002A
MSG_FILE_READ_TEXT_RESPONSE = 0x802A
MSG_PROCESS_LIST = 0x0080
MSG_PROCESS_LIST_RESPONSE = 0x8080
MSG_PROCESS_KILL = 0x0081
MSG_PROCESS_KILL_RESPONSE = 0x8081
MSG_REG_QUERY = 0x0090
MSG_REG_QUERY_RESPONSE = 0x8090
MSG_ERROR = 0x8FFF

CAP_INFO = 0x0001
CAP_EXEC = 0x0004
CAP_SHELL = 0x0008
CAP_FILE_READ = 0x0020
CAP_FILE_WRITE = 0x0040
CAP_POWER = 0x0080
CAP_SCREENSHOT = 0x0100
CAP_DRIVER_UPDATE = 0x0200
CAP_INPUT = 0x0400
CAP_EXEC_DETACH = 0x0800
CAP_HTTP_DOWNLOAD = 0x1000
CAP_INVENTORY = 0x2000
CAP_INSTALL_ID = 0x4000
CAP_INPUT_RELATIVE = 0x8000
CAP_HARD_RESET = 0x10000
# SCREENSHOT_REQUEST takes an options block after the path (agent 0.13.0+):
# flags, region x, y, w, h (w or h 0 = whole screen), scale percent.
CAP_SCREENSHOT_OPTIONS = 0x20000
SHOT_COMPACT = 0x1  # lossless 8-bit palette + RLE8 when <= 256 colours
SHOT_GRAY = 0x2
CAP_FILE_TOOLS = 0x40000  # delete, hash, read-text; reads flush the 9x INI cache
CAP_PROCESS = 0x80000
CAP_REGISTRY = 0x100000
CAP_WINDOWS = 0x200000
MSG_WINDOW_LIST = 0x0062
MSG_WINDOW_LIST_RESPONSE = 0x8062
DELETE_RECURSIVE = 0x1
DELETE_FORCE = 0x2
REG_HIVES = {
    "HKCR": 0x80000000, "HKEY_CLASSES_ROOT": 0x80000000,
    "HKCU": 0x80000001, "HKEY_CURRENT_USER": 0x80000001,
    "HKLM": 0x80000002, "HKEY_LOCAL_MACHINE": 0x80000002,
    "HKU": 0x80000003, "HKEY_USERS": 0x80000003,
    "HKCC": 0x80000005, "HKEY_CURRENT_CONFIG": 0x80000005,
    "HKDD": 0x80000006, "HKEY_DYN_DATA": 0x80000006,
}
REG_TYPES = {0: "REG_NONE", 1: "REG_SZ", 2: "REG_EXPAND_SZ", 3: "REG_BINARY",
             4: "REG_DWORD", 5: "REG_DWORD_BIG_ENDIAN", 7: "REG_MULTI_SZ"}
# Guest names and text are in the Windows ANSI code page.
GUEST_TEXT = "cp1252"

INFO_FLAG_EXTENDED = 0x0001

MDNS_GROUP = "224.0.0.251"
MDNS_PORT = 5353
MDNS_SERVICE = "_v9xremote._tcp.local"

INPUT_OP_MOUSE_MOVE = 1
INPUT_OP_MOUSE_BUTTON = 2
INPUT_OP_MOUSE_WHEEL = 3
INPUT_OP_KEY = 4
INPUT_OP_TYPE = 5
INPUT_OP_DELAY = 6
INPUT_OP_MOUSE_MOVE_RELATIVE = 7

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


WINDOWS_TEXT_SUFFIXES = (".ini", ".bat", ".cmd", ".reg", ".cfg", ".inf")


VNC_EXE = "C:\\V9XREMOTE\\V9XVNC.EXE"


def parse_vnc_ini(text):
    """(enabled, port) from the [vnc] section of an AGENT.INI. VNC is off
    unless enabled=1, which SETUP.EXE's VNC option writes."""
    enabled = False
    port = 5900
    section = ""
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if section != "vnc" or "=" not in line or line.startswith(";"):
            continue
        key, value = (part.strip() for part in line.split("=", 1))
        if key.lower() == "enabled":
            enabled = value == "1"
        elif key.lower() == "port" and value.isdigit():
            port = int(value)
    return enabled, port


def vnc_password_set(text):
    """Whether the [vnc] section sets a VNC password (its value is never
    reported)."""
    section = ""
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
        elif section == "vnc" and "=" in line and not line.startswith(";"):
            key, value = (part.strip() for part in line.split("=", 1))
            if key.lower() == "password":
                return value != ""
    return False


def bare_lf_warning(guest_path, data):
    """put is byte-exact; flag a Windows text file sent with LF-only lines,
    which COMMAND.COM misparses and the Win9x profile writer corrupts (an
    LF-only AGENT.INI collected a new install_id on every start)."""
    if not guest_path.lower().endswith(WINDOWS_TEXT_SUFFIXES):
        return None
    if len(data) > 1048576 or b"\0" in data:
        return None
    if data.count(b"\n") == data.count(b"\r\n"):
        return None
    return (
        "%s was sent with LF-only line ends; Windows 9x expects CRLF. "
        "Convert the source and put it again." % guest_path
    )


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


def parse_ansi_string(payload, offset):
    """A protocol string the guest filled from Windows itself (process and
    registry names): ANSI, not necessarily ASCII."""
    if offset + 2 > len(payload):
        raise V9xError("truncated protocol string length")
    (length,) = struct.unpack_from("<H", payload, offset)
    offset += 2
    if offset + length > len(payload):
        raise V9xError("truncated protocol string")
    return payload[offset : offset + length].decode(GUEST_TEXT, "replace"), offset + length


def parse_read_text(payload):
    if len(payload) < 24:
        raise V9xError("truncated FILE_READ_TEXT_RESPONSE")
    size, lines, matched, returned, flags, length = struct.unpack_from("<6I", payload, 0)
    if 24 + length != len(payload):
        raise V9xError("FILE_READ_TEXT_RESPONSE length mismatch")
    return {
        "file_size": size, "lines": lines, "matched": matched, "returned": returned,
        "truncated": bool(flags & 1), "long_lines_cut": bool(flags & 2),
        "text": payload[24:].decode(GUEST_TEXT, "replace"),
    }


def parse_process_list(payload):
    if len(payload) < 4:
        raise V9xError("truncated PROCESS_LIST_RESPONSE")
    (count,) = struct.unpack_from("<I", payload, 0)
    offset = 4
    processes = []
    for _ in range(count):
        if offset + 20 > len(payload):
            raise V9xError("truncated process entry")
        pid, parent, threads, flags, job = struct.unpack_from("<5I", payload, offset)
        path, offset = parse_ansi_string(payload, offset + 20)
        processes.append({
            "pid": pid, "parent_pid": parent, "threads": threads,
            "name": re.split(r"[\\/:]", path)[-1], "path": path,
            "agent": bool(flags & 2), "agent_job": bool(flags & 1),
            "job_request_id": job,
        })
    if offset != len(payload):
        raise V9xError("PROCESS_LIST_RESPONSE contains trailing data")
    return processes


def parse_window_list(payload):
    if len(payload) < 8:
        raise V9xError("truncated WINDOW_LIST_RESPONSE")
    count, _truncated = struct.unpack_from("<II", payload, 0)
    offset = 8
    windows = []
    for _ in range(count):
        if offset + 28 > len(payload):
            raise V9xError("truncated window entry")
        handle, pid, flags, left, top, right, bottom = struct.unpack_from("<IIIiiii", payload, offset)
        class_name, offset = parse_ansi_string(payload, offset + 28)
        title, offset = parse_ansi_string(payload, offset)
        windows.append({
            "title": title, "class": class_name, "x": left, "y": top,
            "width": right - left, "height": bottom - top,
            "foreground": bool(flags & 1), "minimized": bool(flags & 2),
            "pid": pid, "handle": handle,
        })
    if offset != len(payload):
        raise V9xError("WINDOW_LIST_RESPONSE contains trailing data")
    return windows


def encode_reg_key(key):
    parts = key.replace("/", "\\").rstrip("\\").split("\\", 1)
    hive = REG_HIVES.get(parts[0].rstrip(":").upper())
    if hive is None:
        raise V9xError("unknown registry hive %r (HKLM, HKCU, HKCR, HKU, HKCC, HKDD)" % parts[0])
    return struct.pack("<I", hive) + encode_string(parts[1] if len(parts) > 1 else "")


def parse_reg_query(payload):
    if len(payload) < 12:
        raise V9xError("truncated REG_QUERY_RESPONSE")
    subkey_count, value_count, flags = struct.unpack_from("<III", payload, 0)
    offset = 12
    subkeys = []
    for _ in range(subkey_count):
        name, offset = parse_ansi_string(payload, offset)
        subkeys.append(name)
    values = []
    for _ in range(value_count):
        name, offset = parse_ansi_string(payload, offset)
        if offset + 8 > len(payload):
            raise V9xError("truncated registry value")
        value_type, length = struct.unpack_from("<II", payload, offset)
        offset += 8
        raw = payload[offset : offset + length]
        if len(raw) != length:
            raise V9xError("truncated registry data")
        offset += length
        if value_type in (1, 2):
            data = raw.decode(GUEST_TEXT, "replace").rstrip("\x00")
        elif value_type == 4 and length >= 4:
            data = struct.unpack_from("<I", raw, 0)[0]
        elif value_type == 7:
            data = [s for s in raw.decode(GUEST_TEXT, "replace").split("\x00") if s]
        else:
            data = raw.hex(" ")
        values.append({"name": name or "(Default)",
                       "type": REG_TYPES.get(value_type, "type %d" % value_type),
                       "data": data})
    if offset != len(payload):
        raise V9xError("REG_QUERY_RESPONSE contains trailing data")
    return {"subkeys": subkeys, "values": values, "truncated": bool(flags & 1)}


def parse_desktop_block(payload, offset, install_id=False):
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
    if install_id:
        # Only an INFO reply to a request with INFO_FLAG_EXTENDED carries it.
        values["install_id"] = ""
        if offset < len(payload):
            values["install_id"], offset = parse_string(payload, offset)
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
    values = parse_desktop_block(payload, offset, install_id=True)
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


def parse_inventory(payload):
    """INVENTORY_RESPONSE: u16 record count, then per record a type string,
    a u16 field count and that many (key, value) string pairs. Returned as
    [{"record": type, "fields": {key: value}}]; a key that repeats within a
    record (e.g. several net "ip" fields) becomes a list of values."""
    if len(payload) < 2:
        raise V9xError("truncated INVENTORY response")
    (count,) = struct.unpack_from("<H", payload, 0)
    offset = 2
    records = []
    for _ in range(count):
        record_type, offset = parse_string(payload, offset)
        if offset + 2 > len(payload):
            raise V9xError("truncated INVENTORY record")
        (field_count,) = struct.unpack_from("<H", payload, offset)
        offset += 2
        fields = {}
        for _ in range(field_count):
            key, offset = parse_string(payload, offset)
            value, offset = parse_string(payload, offset)
            if key in fields:
                if not isinstance(fields[key], list):
                    fields[key] = [fields[key]]
                fields[key].append(value)
            else:
                fields[key] = value
        records.append({"record": record_type, "fields": fields})
    if offset != len(payload):
        raise V9xError("response contains trailing data")
    return records


# mDNS / DNS-SD discovery (RFC 6762, RFC 6763) --------------------------------
# DNS is big-endian, unlike the V9XR frames. Mirrors src/common/mdns.c and
# the PowerShell codec in scripts/V9xProtocol.ps1.


def mdns_query(query_id, name=MDNS_SERVICE, qtype=12):
    out = struct.pack(">HHHHHH", query_id & 0xFFFF, 0, 1, 0, 0, 0)
    for label in name.rstrip(".").split("."):
        raw = label.encode("ascii")
        if not 1 <= len(raw) <= 63:
            raise V9xError("invalid DNS label %r" % label)
        out += bytes([len(raw)]) + raw
    return out + b"\x00" + struct.pack(">HH", qtype, 1)


def mdns_read_name(packet, offset):
    """Decode a (possibly compressed) name; returns (name, offset after it)."""
    labels = []
    end = None
    jumps = 0
    while True:
        if offset >= len(packet):
            raise V9xError("truncated DNS name")
        length = packet[offset]
        if length == 0:
            if end is None:
                end = offset + 1
            break
        if length & 0xC0 == 0xC0:
            jumps += 1
            if offset + 1 >= len(packet) or jumps > 16:
                raise V9xError("invalid DNS compression pointer")
            if end is None:
                end = offset + 2
            offset = ((length & 0x3F) << 8) | packet[offset + 1]
            continue
        if length & 0xC0 or offset + 1 + length > len(packet):
            raise V9xError("invalid DNS label")
        labels.append(packet[offset + 1 : offset + 1 + length].decode("ascii", "replace"))
        offset += 1 + length
    return ".".join(labels), end


def parse_mdns_packet(packet):
    if len(packet) < 12:
        raise V9xError("truncated DNS header")
    query_id, flags, questions, answers, authority, additional = struct.unpack_from(
        ">HHHHHH", packet, 0
    )
    offset = 12
    for _ in range(questions):
        _, offset = mdns_read_name(packet, offset)
        offset += 4
    records = []
    for _ in range(answers + authority + additional):
        name, offset = mdns_read_name(packet, offset)
        if offset + 10 > len(packet):
            raise V9xError("truncated DNS record")
        rtype, rclass, ttl, length = struct.unpack_from(">HHIH", packet, offset)
        offset += 10
        if offset + length > len(packet):
            raise V9xError("truncated DNS record data")
        rdata = packet[offset : offset + length]
        if rtype == 1:
            if length != 4:
                raise V9xError("invalid DNS A record")
            data = socket.inet_ntoa(rdata)
        elif rtype == 12:
            data, _ = mdns_read_name(packet, offset)
        elif rtype == 33:
            if length < 7:
                raise V9xError("invalid DNS SRV record")
            priority, weight, port = struct.unpack_from(">HHH", packet, offset)
            target, _ = mdns_read_name(packet, offset + 6)
            data = {"priority": priority, "weight": weight, "port": port, "target": target}
        elif rtype == 16:
            data = []
            at = 0
            while at < length:
                size = rdata[at]
                if at + 1 + size > length:
                    raise V9xError("invalid DNS TXT record")
                if size:
                    data.append(rdata[at + 1 : at + 1 + size].decode("ascii", "replace"))
                at += 1 + size
        else:
            data = rdata.hex()
        offset += length
        records.append(
            {
                "name": name,
                "type": rtype,
                "class": rclass & 0x7FFF,
                "cache_flush": bool(rclass & 0x8000),
                "ttl": ttl,
                "data": data,
            }
        )
    return {"id": query_id, "is_response": bool(flags & 0x8000), "records": records}


def fold_mdns_replies(replies):
    """(source_ip, parsed packet) pairs in arrival order -> one dict per agent.
    A service PTR with TTL 0 is a goodbye and removes the agent."""
    instances = {}
    sources = {}
    srv = {}
    txt = {}
    addresses = {}
    for source, packet in replies:
        for record in packet["records"]:
            key = record["name"].lower()
            if record["type"] == 12 and key == MDNS_SERVICE:
                instance_key = record["data"].lower()
                if record["ttl"] == 0:
                    instances.pop(instance_key, None)
                else:
                    instances[instance_key] = record["data"]
                    sources[instance_key] = source
            elif record["type"] == 33:
                srv[key] = record["data"]
            elif record["type"] == 16:
                txt[key] = record["data"]
            elif record["type"] == 1 and record["ttl"] != 0:
                addresses[key] = record["data"]
    agents = []
    suffix = "." + MDNS_SERVICE
    for instance_key, instance in instances.items():
        fields = {}
        for entry in txt.get(instance_key, []):
            name, sep, value = entry.partition("=")
            if sep and name:
                fields[name] = value
        service = srv.get(instance_key)
        host_name = service["target"] if service else ""
        address = addresses.get(host_name.lower(), sources[instance_key])
        label = instance[: -len(suffix)] if instance.lower().endswith(suffix) else instance
        agents.append(
            {
                "instance": label,
                "install_id": fields.get("id", ""),
                "address": address,
                "port": service["port"] if service else 9869,
                "host_name": host_name,
                "computer_name": fields.get("name", ""),
                "agent_version": fields.get("ver", ""),
                "build_id": fields.get("build", ""),
                "windows_version": fields.get("os", ""),
                "allow_dos_vm": (fields["dos"] == "1") if "dos" in fields else None,
                "responded_from": sources[instance_key],
            }
        )
    return agents


def local_ipv4_addresses():
    """Best-effort list of this host's non-loopback IPv4 addresses (stdlib only)."""
    found = []
    try:
        for entry in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            address = entry[4][0]
            if not address.startswith("127.") and address not in found:
                found.append(address)
    except OSError:
        pass
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect((MDNS_GROUP, MDNS_PORT))
        address = probe.getsockname()[0]
        if address not in found and not address.startswith(("127.", "0.")):
            found.append(address)
    except OSError:
        pass
    finally:
        probe.close()
    return found


def discover_agents(seconds=3.0, target=None, install_id=None):
    """One-shot mDNS query for _v9xremote._tcp.local from an ephemeral port, so
    agents answer by unicast. Multicast on every IPv4 interface, or unicast to
    target ("ip" or "ip:port"). Stops early once install_id has answered."""
    query = mdns_query(random.randint(1, 0xFFFF))
    sockets = []
    replies = []
    try:
        if target:
            host, _, port = target.partition(":")
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(sock)
            sock.sendto(query, (host, int(port) if port else MDNS_PORT))
        else:
            for address in local_ipv4_addresses() or ["0.0.0.0"]:
                sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                try:
                    sock.bind((address, 0))
                    if address != "0.0.0.0":
                        sock.setsockopt(
                            socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(address)
                        )
                    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 255)
                    sock.sendto(query, (MDNS_GROUP, MDNS_PORT))
                    sockets.append(sock)
                except OSError:
                    sock.close()
            if not sockets:
                raise V9xError("no IPv4 interface could send the discovery query")
        deadline = time.monotonic() + seconds
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            readable, _, _ = select.select(sockets, [], [], min(remaining, 0.2))
            for sock in readable:
                try:
                    data, (source, _port) = sock.recvfrom(9000)
                    packet = parse_mdns_packet(data)
                except (OSError, V9xError):
                    continue
                if not packet["is_response"]:
                    continue
                replies.append((source, packet))
                if install_id and any(
                    agent["install_id"].lower() == install_id.lower()
                    for agent in fold_mdns_replies(replies)
                ):
                    deadline = 0
    finally:
        for sock in sockets:
            sock.close()
    return fold_mdns_replies(replies)


def decode_bmp(data):
    """Decode the agent's screenshot BMP to (width, height, rows): rows are
    top-down bytes of R, G, B. Handles what the agent writes: 24-bit BI_RGB,
    and from 0.13.0 8-bit BI_RGB and BI_RLE8 with a palette (shotxf.c)."""
    if len(data) < 54 or data[:2] != b"BM":
        raise V9xError("not a BMP file")
    (pixel_offset,) = struct.unpack_from("<I", data, 10)
    _header_size, width, height, _planes, bpp, compression = struct.unpack_from(
        "<IiiHHI", data, 14
    )
    (colours,) = struct.unpack_from("<I", data, 46)
    if width <= 0 or height == 0:
        raise V9xError("invalid BMP dimensions")
    bottom_up = height > 0
    rows = abs(height)

    def ordered(bottom_up_rows):
        return bottom_up_rows[::-1] if bottom_up else bottom_up_rows

    if bpp == 24 and compression == 0:
        stride = (width * 3 + 3) & ~3
        if pixel_offset + stride * rows > len(data):
            raise V9xError("truncated BMP pixel data")
        stored = []
        for y in range(rows):
            start = pixel_offset + y * stride
            row = data[start : start + width * 3]
            rgb = bytearray(width * 3)
            rgb[0::3] = row[2::3]
            rgb[1::3] = row[1::3]
            rgb[2::3] = row[0::3]
            stored.append(bytes(rgb))
        return width, rows, ordered(stored)
    if bpp != 8 or compression not in (0, 1):
        raise V9xError("unsupported BMP format (%d-bit, compression %d)" % (bpp, compression))
    colours = colours or 256
    if colours > 256 or 54 + colours * 4 > pixel_offset or pixel_offset > len(data):
        raise V9xError("invalid BMP palette")
    palette = [
        bytes((data[54 + i * 4 + 2], data[54 + i * 4 + 1], data[54 + i * 4]))
        for i in range(colours)
    ]
    if compression == 0:
        stride = (width + 3) & ~3
        if pixel_offset + stride * rows > len(data):
            raise V9xError("truncated BMP pixel data")
        index_rows = [
            data[pixel_offset + y * stride : pixel_offset + y * stride + width]
            for y in range(rows)
        ]
    else:
        index_rows = [bytearray(width) for _ in range(rows)]
        at, x, y = pixel_offset, 0, 0
        while True:
            if at + 2 > len(data):
                raise V9xError("truncated RLE8 data")
            count, value = data[at], data[at + 1]
            at += 2
            if count:
                if y >= rows or x + count > width:
                    raise V9xError("RLE8 run outside the image")
                index_rows[y][x : x + count] = bytes((value,)) * count
                x += count
            elif value == 0:
                x, y = 0, y + 1
            elif value == 1:
                break
            elif value == 2:
                if at + 2 > len(data):
                    raise V9xError("truncated RLE8 delta")
                x, y = x + data[at], y + data[at + 1]
                at += 2
            else:
                if y >= rows or x + value > width or at + value > len(data):
                    raise V9xError("RLE8 literal outside the image")
                index_rows[y][x : x + value] = data[at : at + value]
                x += value
                at += value + (value & 1)
    stored = []
    for indices in index_rows:
        try:
            stored.append(b"".join(palette[i] for i in indices))
        except IndexError:
            raise V9xError("BMP pixel outside the palette")
    return width, rows, ordered(stored)


def plan_shot(screen_width, screen_height, region=None, scale_percent=100):
    """The agent's region/size resolution (shotxf.c v9x_shotxf_plan):
    returns (x, y, w, h, out_w, out_h)."""
    if not 1 <= scale_percent <= 100:
        raise V9xError("scale must be 0.01..1")
    if region:
        x, y, w, h = (int(v) for v in region)
        if w <= 0 or h <= 0 or x < 0 or y < 0:
            raise V9xError("region is x, y, width, height with width and height >= 1")
        if screen_width and (x >= screen_width or y >= screen_height):
            raise V9xError("region starts outside the %dx%d screen" % (screen_width, screen_height))
        if screen_width:
            w = min(w, screen_width - x)
            h = min(h, screen_height - y)
    else:
        x, y, w, h = 0, 0, screen_width, screen_height
    out_w = max(1, (w * scale_percent + 99) // 100)
    out_h = max(1, (h * scale_percent + 99) // 100)
    return x, y, w, h, out_w, out_h


def shape_rgb(width, height, rows, region=None, scale_percent=100, gray=False):
    """Crop, box-scale and grey an RGB image the way the agent does, for
    agents older than 0.13.0 that only send the full frame."""
    x0, y0, w, h, out_w, out_h = plan_shot(width, height, region, scale_percent)
    shaped = []
    for oy in range(out_h):
        sy0 = y0 + oy * h // out_h
        sy1 = max(sy0 + 1, y0 + (oy + 1) * h // out_h)
        line = bytearray(out_w * 3)
        for ox in range(out_w):
            sx0 = x0 + ox * w // out_w
            sx1 = max(sx0 + 1, x0 + (ox + 1) * w // out_w)
            count = (sx1 - sx0) * (sy1 - sy0)
            sums = [0, 0, 0]
            for sy in range(sy0, sy1):
                row = rows[sy]
                for channel in range(3):
                    sums[channel] += sum(row[sx0 * 3 + channel : sx1 * 3 : 3])
            r, g, b = ((value + count // 2) // count for value in sums)
            if gray:
                r = g = b = (r * 77 + g * 150 + b * 29 + 128) >> 8
            line[ox * 3 : ox * 3 + 3] = bytes((r, g, b))
        shaped.append(bytes(line))
    return out_w, out_h, shaped


def encode_png(width, height, rows, gray=False):
    """PNG from top-down RGB rows, standard library only. gray stores one
    channel (the rows must then have R = G = B)."""
    if gray:
        rows = [row[0::3] for row in rows]
    raw = b"".join(b"\x00" + bytes(row) for row in rows)

    def chunk(tag, body):
        return (
            struct.pack(">I", len(body))
            + tag
            + body
            + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
        )

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0 if gray else 2, 0, 0, 0)
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )


def bmp_to_png(data, gray=False):
    """Convert an agent screenshot BMP to a PNG, standard library only."""
    width, height, rows = decode_bmp(data)
    return encode_png(width, height, rows, gray)


class V9xClient:
    """One connection to the guest agent: connect, HELLO, operate, close."""

    def __init__(self, host, port, timeout=15.0, label="v9x-mcp", transport=None):
        self.timeout = timeout
        self.request_id = int(time.time() * 1000) % 0x7FFFFFF0 + 1
        # transport: a socket-like RelaySession for a remote-help machine.
        if transport is not None:
            self.sock = transport
        else:
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
        # INSTALL_ID agents take a flags word; older ones require an empty body.
        if self.hello["capabilities"] & CAP_INSTALL_ID:
            self.send(MSG_INFO, struct.pack("<I", INFO_FLAG_EXTENDED))
        else:
            self.send(MSG_INFO)
        _, _, _, payload = self.expect((MSG_INFO_RESPONSE,))
        return parse_info(payload)

    def inventory(self):
        self.require(CAP_INVENTORY, "inventory")
        self.next_id()
        self.send(MSG_INVENTORY)
        _, _, _, payload = self.expect((MSG_INVENTORY_RESPONSE,))
        return parse_inventory(payload)

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

    def tool_request(self, capability, name, msg_type, payload, response_type):
        self.require(capability, name)
        self.next_id()
        self.send(msg_type, payload)
        _, _, _, answer = self.expect((response_type,))
        return answer

    def delete(self, guest_path, recursive=False, force=False):
        flags = (DELETE_RECURSIVE if recursive else 0) | (DELETE_FORCE if force else 0)
        answer = self.tool_request(CAP_FILE_TOOLS, "delete (agent 0.13.0+)", MSG_FILE_DELETE,
                                   struct.pack("<I", flags) + encode_string(guest_path),
                                   MSG_FILE_DELETE_RESPONSE)
        if len(answer) != 8:
            raise V9xError("invalid FILE_DELETE_RESPONSE")
        files, directories = struct.unpack("<II", answer)
        return {"path": guest_path, "files_deleted": files, "directories_deleted": directories}

    def hash_file(self, guest_path):
        answer = self.tool_request(CAP_FILE_TOOLS, "hash (agent 0.13.0+)", MSG_FILE_HASH,
                                   encode_string(guest_path), MSG_FILE_HASH_RESPONSE)
        if len(answer) != 40:
            raise V9xError("invalid FILE_HASH_RESPONSE")
        size, crc = struct.unpack_from("<II", answer, 0)
        return {"path": guest_path, "size": size, "crc32": "%08X" % crc,
                "sha256": answer[8:40].hex().upper()}

    def read_text(self, guest_path, tail=0, match="", max_bytes=16384):
        answer = self.tool_request(CAP_FILE_TOOLS, "read_text (agent 0.13.0+)", MSG_FILE_READ_TEXT,
                                   struct.pack("<II", tail, max_bytes) + encode_string(match)
                                   + encode_string(guest_path), MSG_FILE_READ_TEXT_RESPONSE)
        result = parse_read_text(answer)
        result["path"] = guest_path
        return result

    def processes(self):
        return parse_process_list(self.tool_request(
            CAP_PROCESS, "process list (agent 0.13.0+, Windows 9x/2000)", MSG_PROCESS_LIST,
            b"", MSG_PROCESS_LIST_RESPONSE))

    def kill(self, pid, exit_code=1):
        answer = self.tool_request(CAP_PROCESS, "kill (agent 0.13.0+, Windows 9x/2000)",
                                   MSG_PROCESS_KILL, struct.pack("<II", pid, exit_code),
                                   MSG_PROCESS_KILL_RESPONSE)
        if len(answer) != 8:
            raise V9xError("invalid PROCESS_KILL_RESPONSE")
        killed, job = struct.unpack("<II", answer)
        return {"pid": killed, "was_agent_job": job != 0, "job_request_id": job}

    def windows(self):
        """Visible top-level windows (agent 0.13.0+)."""
        answer = self.tool_request(CAP_WINDOWS, "window list (agent 0.13.0+)",
                                   MSG_WINDOW_LIST, b"", MSG_WINDOW_LIST_RESPONSE)
        return parse_window_list(answer)

    def wait_window(self, title, wait_seconds, gone=False):
        """Poll until a window whose title contains `title` is up (not
        minimized), or with gone=True until none is."""
        started = time.monotonic()
        while True:
            found = [w for w in self.windows()
                     if title.lower() in w["title"].lower() and not w["minimized"]]
            if bool(found) != gone:
                return {"window": found[0] if found else None,
                        "waited_ms": int((time.monotonic() - started) * 1000)}
            if time.monotonic() - started >= wait_seconds:
                raise V9xError("no window titled *%s* %s within %d s"
                               % (title, "closed" if gone else "appeared", wait_seconds))
            time.sleep(0.25)

    def wait_file(self, guest_path, wait_seconds, gone=False, stable_ms=0):
        """Poll FILE_STAT until the file exists (gone: does not), and with
        stable_ms until its size has not changed for that long."""
        started = time.monotonic()
        last_size, stable_since = None, None
        while True:
            stat = self.stat(guest_path)
            now = time.monotonic()
            if gone:
                done = not stat["exists"]
            elif not stat["exists"]:
                done = False
            elif stable_ms <= 0:
                done = True
            else:
                if stat["size"] != last_size:
                    last_size, stable_since = stat["size"], now
                done = (now - stable_since) * 1000 >= stable_ms
            if done:
                stat["waited_ms"] = int((now - started) * 1000)
                return stat
            if now - started >= wait_seconds:
                raise V9xError("%s %s within %d s" % (
                    guest_path, "still exists" if gone else
                    ("kept changing size" if stat["exists"] else "did not appear"), wait_seconds))
            time.sleep(0.25)

    def window_region(self, title):
        """The screen rectangle of the first visible window titled *title*,
        clipped to the screen, as [x, y, w, h]."""
        found = [w for w in self.windows()
                 if title.lower() in w["title"].lower() and not w["minimized"]]
        if not found:
            raise V9xError("no visible window titled *%s* (see v9x_windows)" % title)
        w = found[0]
        left, top = max(0, w["x"]), max(0, w["y"])
        right = min(self.hello.get("screen_width", 0) or w["x"] + w["width"], w["x"] + w["width"])
        bottom = min(self.hello.get("screen_height", 0) or w["y"] + w["height"], w["y"] + w["height"])
        if right <= left or bottom <= top:
            raise V9xError("window %r is off screen" % w["title"])
        return [left, top, right - left, bottom - top]

    def reg_query(self, key):
        result = parse_reg_query(self.tool_request(
            CAP_REGISTRY, "registry (agent 0.13.0+)", MSG_REG_QUERY, encode_reg_key(key),
            MSG_REG_QUERY_RESPONSE))
        result["key"] = key
        return result

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

    def put_stream(self, handle, size, guest_path):
        """Upload size bytes from a seekable binary file object. It is read
        twice (CRC32, then the chunks) and never held in memory whole."""
        self.require(CAP_FILE_WRITE, "file-write")
        if size > MAX_FILE:
            raise V9xError("file exceeds the 2 GiB protocol limit")
        start = handle.tell()
        crc = 0
        remaining = size
        while remaining:
            piece = handle.read(min(remaining, 1048576))
            if not piece:
                raise V9xError("local file is shorter than its size")
            crc = zlib.crc32(piece, crc)
            remaining -= len(piece)
        crc &= 0xFFFFFFFF
        handle.seek(start)
        self.next_id()
        self.send(
            MSG_FILE_OPEN_WRITE,
            struct.pack("<II", size, crc) + encode_string(guest_path),
        )
        _, _, _, ready = self.expect((MSG_FILE_WRITE_READY,))
        if ready != struct.pack("<II", size, crc):
            raise V9xError("FILE_WRITE_READY did not echo size and CRC32")
        # Over the LAN every chunk waits for its ACK. A relay session costs
        # a round trip through the update server per wait, so there up to
        # pipeline_window chunks are in flight; the agent handles them in
        # order and acknowledges each.
        window = max(1, int(getattr(self.sock, "pipeline_window", 1)))
        offset = 0
        acked = 0
        in_flight = []
        while acked < size:
            # Refill in bursts (half a window at a time), so each round trip
            # carries many chunks rather than one per ACK.
            refill = len(in_flight) <= window // 2
            while refill and offset < size and len(in_flight) < window:
                piece = handle.read(min(CHUNK_SIZE, size - offset))
                if not piece:
                    raise V9xError("local file changed during the upload")
                self.send(MSG_FILE_WRITE_CHUNK, struct.pack("<I", offset) + piece)
                offset += len(piece)
                in_flight.append(offset)
            _, _, _, ack = self.expect((MSG_FILE_WRITE_ACK,))
            acked = in_flight.pop(0)
            if ack != struct.pack("<I", acked):
                raise V9xError("upload acknowledgement offset mismatch")
        self.send(MSG_FILE_COMMIT)
        _, _, _, done = self.expect((MSG_FILE_WRITE_COMPLETE,))
        if done != struct.pack("<II", size, crc):
            raise V9xError("commit did not confirm size and CRC32")
        return {"guest_path": guest_path, "size": size, "crc32": "%08X" % crc}

    def put_bytes(self, data, guest_path):
        return self.put_stream(io.BytesIO(data), len(data), guest_path)

    def put_file(self, local_path, guest_path):
        with open(local_path, "rb") as handle:
            size = os.fstat(handle.fileno()).st_size
            return self.put_stream(handle, size, guest_path)

    def get_stream(self, guest_path, handle):
        """Download into a binary file object chunk by chunk; returns
        (size, crc32) once both match what the guest reports."""
        self.require(CAP_FILE_READ, "file")
        self.next_id()
        self.send(MSG_FILE_OPEN_READ, encode_string(guest_path))
        received = 0
        running_crc = 0
        while True:
            _, msg_type, _, payload = self.expect(
                (MSG_FILE_READ_CHUNK, MSG_FILE_READ_COMPLETE)
            )
            if msg_type == MSG_FILE_READ_CHUNK:
                if len(payload) < 4 or struct.unpack_from("<I", payload, 0)[0] != received:
                    raise V9xError("download chunk offset mismatch")
                piece = payload[4:]
                handle.write(piece)
                running_crc = zlib.crc32(piece, running_crc)
                received += len(piece)
            else:
                break
        if len(payload) != 8:
            raise V9xError("invalid FILE_READ_COMPLETE payload")
        size, crc = struct.unpack("<II", payload)
        if size != received or crc != (running_crc & 0xFFFFFFFF):
            raise V9xError("downloaded size or CRC32 does not match the guest")
        return size, crc

    def get_bytes(self, guest_path):
        buffer = io.BytesIO()
        self.get_stream(guest_path, buffer)
        return buffer.getvalue()

    def download(self, url, guest_path):
        self.require(CAP_HTTP_DOWNLOAD, "http-download")
        self.next_id()
        self.send(MSG_DOWNLOAD, encode_string(url) + encode_string(guest_path))
        old_timeout = self.timeout
        self.sock.settimeout(600.0)
        try:
            while True:
                _, msg_type, _, payload = self.expect(
                    (MSG_DOWNLOAD_PROGRESS, MSG_DOWNLOAD_COMPLETE)
                )
                if msg_type == MSG_DOWNLOAD_COMPLETE:
                    break
            if len(payload) != 12:
                raise V9xError("invalid DOWNLOAD_COMPLETE payload")
            status, size, crc = struct.unpack("<III", payload)
        finally:
            self.sock.settimeout(old_timeout)
        return {
            "url": url,
            "guest_path": guest_path,
            "http_status": status,
            "size": size,
            "crc32": "%08X" % crc,
        }

    def update_apply(self, agent_size, agent_crc, helper_size, helper_crc):
        self.require(CAP_DRIVER_UPDATE, "hot-update")
        self.next_id()
        self.send(
            MSG_UPDATE,
            struct.pack("<IIII", agent_size, agent_crc, helper_size, helper_crc),
        )
        _, _, _, payload = self.expect((MSG_UPDATE_ACCEPTED,))
        if len(payload) != 8:
            raise V9xError("invalid UPDATE_ACCEPTED payload")
        boot, agent_bytes = struct.unpack("<II", payload)
        return {"boot_counter": boot, "agent_size": agent_bytes}

    def screenshot(self, guest_path=DEFAULT_SCREENSHOT, region=None, scale_percent=100,
                   gray=False, window=None):
        """Capture; returns (metadata, bmp). An 0.13.0+ agent crops, scales
        and packs the image itself (always losslessly 8-bit/RLE8 when it can);
        for an older one metadata["host_shape"] tells screenshot_content to do
        it after the download."""
        self.require(CAP_SCREENSHOT, "screenshot")
        if window:
            region = self.window_region(window)
        x, y, w, h, out_w, out_h = plan_shot(
            self.hello.get("screen_width", 0), self.hello.get("screen_height", 0),
            region, scale_percent)
        shaping = bool(region) or scale_percent < 100 or gray
        guest_shapes = bool(self.hello["capabilities"] & CAP_SCREENSHOT_OPTIONS)
        payload = encode_string(guest_path)
        if guest_shapes:
            flags = SHOT_COMPACT | (SHOT_GRAY if gray else 0)
            payload += struct.pack("<6I", flags, x, y, w if region else 0,
                                   h if region else 0, scale_percent)
        self.next_id()
        self.send(MSG_SCREENSHOT, payload)
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
        metadata = {
            "width": width,
            "height": height,
            "source_bits_per_pixel": bpp,
            "guest_path": path,
            "wire_bytes": file_bytes,
        }
        if shaping:
            # screen_x = region_x + image_x * region_width / width (same for y)
            metadata.update({
                "region_x": x, "region_y": y, "region_width": w, "region_height": h,
                "scale_percent": scale_percent, "gray": bool(gray),
                "shaped_by": "guest" if guest_shapes else "host",
            })
            if not guest_shapes:
                metadata["host_shape"] = (region, scale_percent, bool(gray))
        return metadata, data

    def send_input(self, actions):
        self.require(CAP_INPUT, "input")
        if len(actions) > 256:
            raise V9xError("at most 256 input actions per request")
        payload = struct.pack("<H", len(actions))
        for action in actions:
            op = action["op"]
            if op == "move":
                payload += struct.pack("<Bii", INPUT_OP_MOUSE_MOVE, int(action["x"]), int(action["y"]))
            elif op == "moverel":
                payload += struct.pack(
                    "<Bii", INPUT_OP_MOUSE_MOVE_RELATIVE, int(action["dx"]), int(action["dy"])
                )
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


def wait_for_update(host, port, old_boot, wait_seconds, resolve=None, connect=None):
    deadline = time.monotonic() + wait_seconds
    last_error = "no connection attempt succeeded"
    while time.monotonic() < deadline:
        time.sleep(0.5)
        try:
            if connect is not None:
                candidate = connect(deadline - time.monotonic())
            else:
                candidate = V9xClient(host, port, timeout=3.0)
        except (OSError, V9xError) as exc:
            last_error = str(exc)
            if resolve is not None:
                host, port = resolve(host, port)
            continue
        try:
            hello = candidate.hello
            if hello["boot_counter"] != old_boot:
                return hello
            last_error = "reconnected with unchanged boot counter %d" % hello[
                "boot_counter"
            ]
        finally:
            candidate.close()
    raise V9xError(
        "agent did not relaunch with a new boot counter within %d seconds; "
        "last error: %s" % (wait_seconds, last_error)
    )


def wait_for_reboot(host, port, old_boot, job_id, wait_seconds, resolve=None, connect=None):
    deadline = time.monotonic() + wait_seconds
    last_error = "no connection attempt succeeded"
    while time.monotonic() < deadline:
        time.sleep(0.5)
        try:
            # connect: a relay machine comes back by checking in again.
            if connect is not None:
                candidate = connect(deadline - time.monotonic())
            else:
                candidate = V9xClient(host, port, timeout=3.0)
        except (OSError, V9xError) as exc:
            last_error = str(exc)
            # With --id the guest may come back on a new address (DHCP).
            if resolve is not None:
                host, port = resolve(host, port)
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
        "name": "v9x_discover",
        "description": "Find V9x agents on the LAN by mDNS/DNS-SD (agent 0.8.0+ with discovery=1): instance name, address, port, version and install ID of each. Opens no agent connection. Needs a network the host shares with the guests (bridged or physical, not SLiRP NAT).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "seconds": {"type": "number", "description": "How long to listen for answers (default 3, max 30)."},
                "query_address": {"type": "string", "description": "Send the query unicast to this ip[:port] instead of multicasting it."},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_ping",
        "description": "Check that the Windows 9x guest agent is alive; returns uptime and the agent-start counter (not reboot proof by itself). wait_seconds keeps retrying while the guest boots: never loop ping with sleeps.",
        "inputSchema": {
            "type": "object",
            "properties": {"wait_seconds": {"type": "integer", "minimum": 0, "maximum": 3600,
                                            "description": "Retry connecting for up to this long (default 0)"}},
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_info",
        "description": "Full guest agent status: version, build, computer name, Windows version, screen mode, desktop readiness, pending job.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "v9x_inventory",
        "description": "Hardware/software inventory (agent 0.7.0+): OS product/build, CPU, memory, drives, display adapter, IPs, BIOS and the PnP device list, as typed records of key/value fields.",
        "inputSchema": {"type": "object", "properties": {}, "additionalProperties": False},
    },
    {
        "name": "v9x_exec",
        "description": "Run a Win32 executable directly in the guest with bounded captured stdout/stderr. Use v9x_shell for shell built-ins and batch files.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "application": guest_path_schema("Full guest path of the EXE, e.g. C:\\V9XREMOTE\\JOBS\\X\\APP.EXE"),
                "arguments": {"type": "string", "description": "Command-line arguments (optional)"},
                "working_dir": {"type": "string", "description": "Guest working directory (optional)"},
                "timeout_ms": {"type": "integer", "description": "Guest-side timeout in ms (default 60000, max 3600000)"},
                "show_window": {"type": "boolean", "description": "Show the child's window (console children; GUI-subsystem EXEs manage their own window)"},
                "detach": {"type": "boolean", "description": "Launch and return immediately without waiting or capturing output. Use for installers and programs that outlive the request."},
                "wait_for_file": guest_path_schema("After the run (usually with detach): wait until this guest file exists, then return"),
                "wait_for_window": {"type": "string", "description": "After the run (usually with detach): wait until a window whose title contains this is shown, then return it"},
                "wait_seconds": {"type": "integer", "minimum": 1, "maximum": 3600, "description": "Limit for wait_for_file / wait_for_window (default 120)"},
            },
            "required": ["application"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_shell",
        "description": "Run a command through COMMAND.COM /C in the guest (CMD.EXE /C on Windows 2000): DIR, COPY, batch files, VER...",
        "inputSchema": {
            "type": "object",
            "properties": {
                "command": {"type": "string", "description": "The shell command line"},
                "timeout_ms": {"type": "integer", "description": "Guest-side timeout in ms (default 60000)"},
                "detach": {"type": "boolean", "description": "Launch and return immediately without waiting or capturing output. Use instead of START for commands that spawn long-lived programs."},
                "wait_for_file": guest_path_schema("After the run (usually with detach): wait until this guest file exists, then return"),
                "wait_for_window": {"type": "string", "description": "After the run (usually with detach): wait until a window whose title contains this is shown, then return it"},
                "wait_seconds": {"type": "integer", "minimum": 1, "maximum": 3600, "description": "Limit for wait_for_file / wait_for_window (default 120)"},
            },
            "required": ["command"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_put_file",
        "description": "Upload a local file into the guest, CRC32-verified and transactional (2 GiB max on agent 0.11.0+, 256 MiB before).",
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
        "name": "v9x_read_text",
        "description": "Read a guest text file inline (logs, INI, batch output): optionally only the last `tail` lines and/or lines containing `match` (case-insensitive), bounded to max_bytes. One call instead of get + read; ask for little. Agent 0.13.0+.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "guest_path": guest_path_schema("Guest file to read"),
                "tail": {"type": "integer", "minimum": 0, "description": "Only the last N (matching) lines; 0 = all"},
                "match": {"type": "string", "description": "Only lines containing this text (ASCII, case-insensitive)"},
                "max_bytes": {"type": "integer", "minimum": 1, "maximum": 65512, "description": "Most text to return (default 16384)"},
            },
            "required": ["guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_hash",
        "description": "Size, CRC32 and SHA-256 of a guest file, computed in the guest; pass local_path to compare with a host file (verify a push without downloading). Agent 0.13.0+.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "guest_path": guest_path_schema("Guest file to hash"),
                "local_path": {"type": "string", "description": "Optional host file to compare against"},
            },
            "required": ["guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_delete",
        "description": "Delete a guest file, or a directory (empty, or with recursive=true everything in it). force=true clears read-only/hidden/system first. Refuses drive roots, the Windows and system directories, C:\\V9XREMOTE and any folder containing them. Agent 0.13.0+.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "guest_path": guest_path_schema("Guest file or directory to delete"),
                "recursive": {"type": "boolean"},
                "force": {"type": "boolean"},
            },
            "required": ["guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_ps",
        "description": "List guest processes (pid, parent, threads, path); marks the agent and processes started by its exec requests (with their request id). Agent 0.13.0+ on Windows 9x or 2000.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "name": {"type": "string", "description": "Only image names matching this (wildcards * ?)"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_kill",
        "description": "Terminate a guest process by pid (from v9x_ps) or by unique image name; ends a stuck exec job (its exec then completes). Refuses the agent and Windows core processes. Agent 0.13.0+.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "pid": {"type": "integer", "minimum": 1},
                "name": {"type": "string", "description": "Image name, e.g. NOTEPAD.EXE; must match exactly one process"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_windows",
        "description": "Visible top-level windows in the guest: title, class, x, y, width, height, foreground, minimized, pid. With wait_for (part of a title): wait until such a window is shown (gone=true: closed) instead of sleeping; then screenshot it with window=<title>. Agent 0.13.0+.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "title": {"type": "string", "description": "Only windows whose title contains this"},
                "wait_for": {"type": "string", "description": "Wait until a window whose title contains this is shown"},
                "gone": {"type": "boolean", "description": "With wait_for: wait until it is closed"},
                "wait_seconds": {"type": "integer", "minimum": 1, "maximum": 3600, "description": "Limit for wait_for (default 60)"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_wait_file",
        "description": "Wait until a guest file exists (gone=true: is removed; stable_ms: and its size stopped changing), polling on one connection: use instead of sleeping between exec and get. Any agent.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "guest_path": guest_path_schema("Guest file to wait for"),
                "wait_seconds": {"type": "integer", "minimum": 1, "maximum": 3600, "description": "Limit (default 120)"},
                "gone": {"type": "boolean"},
                "stable_ms": {"type": "integer", "minimum": 0, "description": "Also wait until the size has not changed for this long"},
            },
            "required": ["guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_reg_query",
        "description": "Read one registry key in the guest: its subkey names and values (strings, DWORDs, multi-strings, binary as hex). Read-only. Key like HKLM\\System\\CurrentControlSet\\Services\\Class\\Display. Agent 0.13.0+.",
        "inputSchema": {
            "type": "object",
            "properties": {"key": {"type": "string", "description": "HKLM\\..., HKCU\\..., HKCR, HKU, HKCC or HKDD"}},
            "required": ["key"],
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
        "description": "Click a mouse button in the guest, optionally moving to (x,y) first. Take a v9x_screenshot to find coordinates; pass screenshot=true (on any input tool) to see the result in the same call.",
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
        "description": "Move the mouse cursor to absolute screen coordinates in the guest, or with relative=true by x,y pixels as real mouse motion (for games that read deltas and recentre the cursor; agent 0.8.1+).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "x": {"type": "integer"},
                "y": {"type": "integer"},
                "relative": {"type": "boolean"},
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
        "description": "Capture a confirmed-stable guest desktop as PNG. Never call during a fullscreen transition or suspected display wedge; use info, trace execution, and file retrieval first. Keep images small: pass region to look at one dialog and scale (e.g. 0.5) when you only need layout; image cost grows with pixels. With region/scale, screen_x = region_x + image_x * region_width / width.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "save_path": {"type": "string", "description": "Optional local path to also save the PNG to"},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_download",
        "description": "Have the guest fetch an http:// URL straight to a guest path over its own network connection (no host round-trip). HTTP only; https is rejected. Verified and written transactionally (2 GiB max on agent 0.8.1+, 256 MiB before).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "url": {"type": "string", "description": "http:// URL to fetch (https is not supported)"},
                "guest_path": guest_path_schema("Destination path in the guest"),
            },
            "required": ["url", "guest_path"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_update",
        "description": "Upgrade the guest agent WITHOUT a reboot from a local install package folder (must contain V9XAGNT.EXE and V9XSHOT.EXE). Uploads and CRC-verifies the new binaries, hot-swaps them, and waits for the agent to relaunch with a new boot counter. Agents before 0.6.0 cannot hot-update and are refused before anything is uploaded; upgrade those once with v9xctl update, which stages it with a reboot. A V9XVNC.EXE in the package replaces the guest's VNC server too (stopped first, restarted only if it was running).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "package_dir": {"type": "string", "description": "Local folder with the new V9XAGNT.EXE and V9XSHOT.EXE (e.g. build\\install)"},
                "wait_seconds": {"type": "integer", "description": "How long to wait for relaunch (default 120)"},
            },
            "required": ["package_dir"],
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_vnc",
        "description": "Status, start or stop V9XVNC.EXE, the guest's VNC (RFB) server, so a PERSON can watch and drive the guest with any VNC viewer (port 5900 in the guest). It only runs where the machine's owner enabled it with SETUP.EXE's VNC option ([vnc] enabled=1); start on a disabled guest is refused. For your own GUI work keep using v9x_screenshot and the input tools.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "action": {"type": "string", "enum": ["status", "start", "stop"], "description": "What to do (default status)"},
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
        "name": "v9x_reset_with_proof",
        "description": "Windows 9x only (agent 0.10.0+). For a guest whose Windows restart hangs (e.g. a real-mode NDIS2 driver resident under Windows): the agent notifies programs, flushes the registry and every fixed drive, then hard-resets through port CF9h without Windows' return to real mode. Same proof as v9x_reboot_with_proof; prefer that tool wherever a normal reboot works.",
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
    {
        "name": "v9x_relay_machines",
        "description": "List the machines whose owners lent them for remote help (agent 0.12.0+ with Remote help On): machine id (M-XXXXXX), owner name, GitHub user, computer, OS, agent version, online, paused, busy (a command in the last minute) and last command age. Drive one by passing relay=\"M-XXXXXX\" to any other v9x tool. Needs the operator key in ~/.v9x/operator.key.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "include_retired": {"type": "boolean", "description": "Also list retired machines."},
            },
            "additionalProperties": False,
        },
    },
    {
        "name": "v9x_chat",
        "description": "Chat with a remote-help machine's owner. With 'message' it is sent (it pops up in the agent's chat window at the next check-in); without, the conversation so far is returned (the owner's replies included).",
        "inputSchema": {
            "type": "object",
            "properties": {
                "machine": {"type": "string", "description": "Machine id, e.g. M-4F7K2Q."},
                "message": {"type": "string", "description": "Text to send. Omit to read."},
                "after": {"type": "integer", "description": "When reading, only messages with a larger id."},
            },
            "required": ["machine"],
            "additionalProperties": False,
        },
    },
]

# Every agent tool can run against a remote-help machine through the relay.
RELAY_PROPERTY = {
    "type": "string",
    "description": "Remote-help machine id (M-XXXXXX, see v9x_relay_machines) to run this "
    "against through the update server instead of the local agent. Opening a session "
    "takes up to a minute when the machine is idle, a few seconds after recent use.",
}
for _tool in TOOLS:
    if _tool["name"] not in ("v9x_discover", "v9x_relay_machines", "v9x_chat"):
        _tool["inputSchema"].setdefault("properties", {})["relay"] = RELAY_PROPERTY

# The input tools can end with a screenshot on the same connection, so one GUI
# step (act, let the guest repaint, look) is one tool call.
INPUT_TOOLS = ("v9x_click", "v9x_move", "v9x_type", "v9x_key", "v9x_scroll", "v9x_input")
DEFAULT_SETTLE_MS = 500
MAX_SETTLE_MS = 30000
SCREENSHOT_PROPERTIES = {
    "screenshot": {
        "type": "boolean",
        "description": "After the input, wait settle_ms and return a PNG of the guest "
        "screen in the same call. Same caveats as v9x_screenshot: not during a "
        "fullscreen transition or suspected display wedge.",
    },
    "settle_ms": {
        "type": "integer",
        "minimum": 0,
        "maximum": MAX_SETTLE_MS,
        "description": "Milliseconds to let the guest repaint before the screenshot "
        "(default %d; raise it for slow dialogs)" % DEFAULT_SETTLE_MS,
    },
    "save_path": {
        "type": "string",
        "description": "With screenshot: optional local path to also save the PNG to",
    },
}
# Shaping, for v9x_screenshot and the input tools' screenshot. 0.13.0+ agents
# do it before sending (fast over the relay); older ones are shaped here.
SHAPE_PROPERTIES = {
    "region": {
        "type": "array",
        "items": {"type": "integer", "minimum": 0},
        "minItems": 4,
        "maxItems": 4,
        "description": "[x, y, width, height] in screen pixels: capture only this "
        "rectangle (clipped to the screen)",
    },
    "scale": {
        "type": "number",
        "minimum": 0.01,
        "maximum": 1,
        "description": "Downscale factor, e.g. 0.5 for half width and height "
        "(a quarter of the image cost); default 1",
    },
    "gray": {
        "type": "boolean",
        "description": "8-bit greyscale (lossy, smaller); fine for reading text and layout",
    },
    "window": {
        "type": "string",
        "description": "Capture only the first visible window whose title contains this "
        "(agent 0.13.0+); instead of region",
    },
}
for _tool in TOOLS:
    if _tool["name"] in INPUT_TOOLS:
        _tool["inputSchema"]["properties"].update(SCREENSHOT_PROPERTIES)
    if _tool["name"] in INPUT_TOOLS or _tool["name"] == "v9x_screenshot":
        _tool["inputSchema"]["properties"].update(SHAPE_PROPERTIES)


class ToolHandler:
    def __init__(self, host, port, timeout, install_id=None, query_address=None,
                 discover_seconds=3.0, allow_remote=False, relay=None, relay_server=None):
        self.default_relay = relay
        self.relay = relay
        self.relay_server = relay_server
        self.relay_config = None
        self.host = host
        self.port = port
        self.timeout = timeout
        self.install_id = install_id
        self.query_address = query_address
        self.discover_seconds = discover_seconds
        self.allow_remote = allow_remote
        self.resolved = False

    def resolve(self):
        """--id: find the agent by install ID and adopt its address. The
        loopback guard applies to whatever address discovery returns."""
        matches = [
            agent
            for agent in discover_agents(
                self.discover_seconds, self.query_address, self.install_id
            )
            if agent["install_id"].lower() == self.install_id.lower()
        ]
        if not matches:
            raise V9xError(
                "no agent with install ID %s answered discovery within %g s; it needs "
                "discovery=1 in AGENT.INI and a LAN shared with this host"
                % (self.install_id, self.discover_seconds)
            )
        address = matches[-1]["address"]
        if not is_loopback(address) and not self.allow_remote:
            raise V9xError(
                "install ID %s resolved to non-loopback %s; pass --allow-remote "
                "(the guest protocol is unauthenticated)" % (self.install_id, address)
            )
        self.host, self.port = address, matches[-1]["port"]
        self.resolved = True
        return self.host, self.port

    def reresolve(self, host, port):
        try:
            return self.resolve()
        except (OSError, V9xError):
            return host, port

    def relay_settings(self):
        if self.relay_config is None:
            self.relay_config = RelayConfig(server=self.relay_server)
        return self.relay_config

    def connect_relay(self, open_wait=150.0):
        """A V9xClient whose socket is a relay session to self.relay."""
        if not re.match(r"^M-[0-9A-HJKMNP-TV-Z]{6}$", (self.relay or "").upper()):
            raise V9xError("relay must be a machine id such as M-4F7K2Q")
        timeout = max(self.timeout, 60.0)
        try:
            session = RelaySession(self.relay_settings(), self.relay,
                                   open_wait=max(5.0, open_wait), timeout=timeout)
        except RelayError as exc:
            raise V9xError(str(exc))
        try:
            return V9xClient(None, None, timeout=timeout, label="v9x-mcp-relay",
                             transport=session)
        except Exception:
            session.close()
            raise

    def reconnector(self):
        """For the reboot/update waits: how to reach the agent again."""
        if self.relay:
            return lambda remaining: self.connect_relay(open_wait=remaining)
        return None

    def connect(self, wait_seconds=0):
        """Connect and HELLO. A connection dropped before HELLO (a port
        forward answering before the agent listens) is retried twice; with
        wait_seconds every connection failure is retried for that long,
        backing off 1, 2, 4, 5 s, so callers never sleep-and-poll."""
        if self.relay:
            return self.connect_relay()
        if self.install_id and not self.resolved:
            self.resolve()
        deadline = time.monotonic() + wait_seconds
        attempt = 0
        while True:
            try:
                return V9xClient(self.host, self.port, timeout=self.timeout)
            except (OSError, V9xError) as exc:
                error = exc
            attempt += 1
            dropped = isinstance(error, (ConnectionResetError, ConnectionAbortedError)) or (
                isinstance(error, V9xError) and "closed" in str(error))
            delay = min(5.0, 2.0 ** (attempt - 1))
            if wait_seconds > 0 and time.monotonic() + delay < deadline:
                time.sleep(delay)
            elif wait_seconds <= 0 and dropped and attempt <= 2:
                time.sleep(0.5 * attempt)
            else:
                break
            if self.install_id:
                self.resolve()
        if isinstance(error, V9xError) and not isinstance(error, OSError):
            raise error
        if self.install_id:
            self.resolve()
            try:
                return V9xClient(self.host, self.port, timeout=self.timeout)
            except OSError as exc:
                error = exc
        raise V9xError(
            "cannot reach the guest agent at %s:%d (%s); is the VM running "
            "and the port forwarded?" % (self.host, self.port, error)
        )

    def call(self, name, args):
        method = getattr(self, name, None)
        if method is None:
            raise V9xError("unknown tool: %s" % name)
        args = dict(args or {})
        self.relay = args.pop("relay", None) or self.default_relay
        return method(args)

    def v9x_relay_machines(self, args):
        try:
            machines = self.relay_settings().machines(bool(args.get("include_retired")))
        except RelayError as exc:
            raise V9xError(str(exc))
        return self.text({"machines": machines})

    def v9x_chat(self, args):
        machine = str(args.get("machine", "")).upper()
        try:
            if args.get("message"):
                self.relay_settings().chat_send(machine, str(args["message"]))
                return self.text({"sent": True, "machine": machine})
            return self.text({"machine": machine,
                              "messages": self.relay_settings().chat_read(machine, args.get("after", 0))})
        except RelayError as exc:
            raise V9xError(str(exc))

    @staticmethod
    def text(value):
        return [{"type": "text", "text": json.dumps(value, indent=1)}]

    def v9x_discover(self, args):
        seconds = float(args.get("seconds", self.discover_seconds))
        if not 0 < seconds <= 30:
            raise V9xError("seconds must be between 0 and 30")
        target = args.get("query_address") or self.query_address
        return self.text({"agents": discover_agents(seconds, target)})

    def v9x_ping(self, args):
        # wait_seconds: keep trying while the guest boots, instead of a
        # caller's ping-and-sleep loop.
        with self.connect(int(args.get("wait_seconds", 0))) as client:
            return self.text(client.ping())

    def v9x_info(self, _args):
        with self.connect() as client:
            return self.text(client.info())

    def v9x_inventory(self, _args):
        with self.connect() as client:
            return self.text(client.inventory())

    def v9x_exec(self, args):
        with self.connect() as client:
            result = client.execute(
                0,
                application=args["application"],
                arguments=args.get("arguments", ""),
                working_dir=args.get("working_dir", ""),
                timeout_ms=int(args.get("timeout_ms", 60000)),
                show_window=bool(args.get("show_window", False)),
                detach=bool(args.get("detach", False)),
            )
            return self.text(self.after_run(client, result, args))

    def v9x_shell(self, args):
        with self.connect() as client:
            result = client.execute(
                1,
                arguments=args["command"],
                timeout_ms=int(args.get("timeout_ms", 60000)),
                detach=bool(args.get("detach", False)),
            )
            return self.text(self.after_run(client, result, args))

    @staticmethod
    def after_run(client, result, args):
        """wait_for_file / wait_for_window after a successful run, on the
        same connection, so the caller does not sleep and poll."""
        wait_seconds = int(args.get("wait_seconds", 120))
        if result.get("result") != "ok":
            return result
        if args.get("wait_for_file"):
            result["waited_for_file"] = client.wait_file(args["wait_for_file"], wait_seconds)
        if args.get("wait_for_window"):
            result["waited_for_window"] = client.wait_window(args["wait_for_window"], wait_seconds)
        return result

    def v9x_put_file(self, args):
        with self.connect() as client:
            result = client.put_file(args["local_path"], args["guest_path"])
        warning = None
        if result["size"] <= 1048576:
            with open(args["local_path"], "rb") as handle:
                warning = bare_lf_warning(args["guest_path"], handle.read())
        if warning:
            result["warning"] = warning
        return self.text(result)

    def v9x_get_file(self, args):
        local_path = os.path.abspath(args["local_path"])
        parent = os.path.dirname(local_path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        temporary = "%s.v9x.part.%08x" % (local_path, random.getrandbits(32))
        try:
            with self.connect() as client, open(temporary, "xb") as handle:
                size, crc = client.get_stream(args["guest_path"], handle)
            os.replace(temporary, local_path)
        finally:
            if os.path.exists(temporary):
                os.remove(temporary)
        return self.text(
            {
                "guest_path": args["guest_path"],
                "local_path": local_path,
                "size": size,
                "crc32": "%08X" % crc,
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
                    destination = base + "\\" + name
                    result = client.put_file(os.path.join(root, name), destination)
                    uploaded.append(destination)
                    total += result["size"]
        return self.text(
            {"guest_dir": guest_dir, "files": len(uploaded), "bytes": total}
        )

    def v9x_stat(self, args):
        with self.connect() as client:
            return self.text(client.stat(args["guest_path"]))

    def v9x_read_text(self, args):
        with self.connect() as client:
            return self.text(client.read_text(args["guest_path"], int(args.get("tail", 0)),
                                              args.get("match", ""),
                                              int(args.get("max_bytes", 16384))))

    def v9x_hash(self, args):
        with self.connect() as client:
            result = client.hash_file(args["guest_path"])
        local = args.get("local_path")
        if local:
            digest = hashlib.sha256()
            with open(local, "rb") as handle:
                for block in iter(lambda: handle.read(65536), b""):
                    digest.update(block)
            result["local_path"] = os.path.abspath(local)
            result["local_sha256"] = digest.hexdigest().upper()
            result["matches"] = result["local_sha256"] == result["sha256"]
        return self.text(result)

    def v9x_delete(self, args):
        with self.connect() as client:
            return self.text(client.delete(args["guest_path"], bool(args.get("recursive")),
                                           bool(args.get("force"))))

    def v9x_ps(self, args):
        with self.connect() as client:
            processes = client.processes()
        if args.get("name"):
            processes = [p for p in processes
                         if fnmatch.fnmatch(p["name"].upper(), args["name"].upper())]
        return self.text({"count": len(processes), "processes": processes})

    def v9x_kill(self, args):
        pid, name = args.get("pid"), args.get("name")
        if not pid and not name:
            raise V9xError("kill needs pid or name")
        with self.connect() as client:
            if not pid:
                found = [p for p in client.processes() if p["name"].upper() == name.upper()]
                if len(found) != 1:
                    raise V9xError("%r matches %d processes; pass pid (see v9x_ps)"
                                   % (name, len(found)))
                pid = found[0]["pid"]
            return self.text(client.kill(int(pid)))

    def v9x_windows(self, args):
        with self.connect() as client:
            if args.get("wait_for"):
                return self.text(client.wait_window(args["wait_for"], int(args.get("wait_seconds", 60)),
                                                    bool(args.get("gone"))))
            windows = client.windows()
        if args.get("title"):
            windows = [w for w in windows if args["title"].lower() in w["title"].lower()]
        return self.text({"count": len(windows), "windows": windows})

    def v9x_wait_file(self, args):
        with self.connect() as client:
            return self.text(client.wait_file(args["guest_path"], int(args.get("wait_seconds", 120)),
                                              bool(args.get("gone")), int(args.get("stable_ms", 0))))

    def v9x_reg_query(self, args):
        with self.connect() as client:
            return self.text(client.reg_query(args["key"]))

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
        return self.input_step(actions, args)

    def v9x_move(self, args):
        if args.get("relative"):
            action = {"op": "moverel", "dx": args["x"], "dy": args["y"]}
        else:
            action = {"op": "move", "x": args["x"], "y": args["y"]}
        return self.input_step([action], args)

    def v9x_type(self, args):
        return self.input_step([{"op": "type", "text": args["text"]}], args)

    def v9x_key(self, args):
        vks = [virtual_key(name) for name in args["keys"].split("+")]
        actions = [{"op": "key", "vk": vk, "down": True} for vk in vks]
        actions += [{"op": "key", "vk": vk, "down": False} for vk in reversed(vks)]
        return self.input_step(actions, args)

    def v9x_scroll(self, args):
        return self.input_step([{"op": "wheel", "notches": args["notches"]}], args)

    def v9x_input(self, args):
        actions = []
        for raw in args["actions"]:
            action = dict(raw)
            if action.get("op") == "key" and "vk" not in action and "name" in action:
                action["vk"] = virtual_key(action.pop("name"))
            actions.append(action)
        return self.input_step(actions, args)

    def input_step(self, actions, args):
        """Send input; with screenshot=true, settle and capture on the same
        connection and return the input result followed by the image."""
        settle_ms = int(args.get("settle_ms", DEFAULT_SETTLE_MS))
        if not 0 <= settle_ms <= MAX_SETTLE_MS:
            raise V9xError("settle_ms must be 0..%d" % MAX_SETTLE_MS)
        shape = self.shot_args(args)  # reject bad options before any input
        with self.connect() as client:
            if any(action["op"] == "moverel" for action in actions):
                client.require(CAP_INPUT_RELATIVE, "relative mouse moves")
            result = client.send_input(actions)
            if not args.get("screenshot"):
                return self.text(result)
            time.sleep(settle_ms / 1000.0)
            capture = client.screenshot(**shape)
        result["settle_ms"] = settle_ms
        return self.text(result) + self.screenshot_content(capture, args.get("save_path"))

    def v9x_screenshot(self, args):
        shape = self.shot_args(args)
        with self.connect() as client:
            capture = client.screenshot(**shape)
        return self.screenshot_content(capture, args.get("save_path"))

    @staticmethod
    def shot_args(args):
        """region / scale / gray tool arguments as client.screenshot kwargs."""
        region = args.get("region")
        if region is not None and (not isinstance(region, list) or len(region) != 4):
            raise V9xError("region is [x, y, width, height] in screen pixels")
        scale = float(args.get("scale", 1.0))
        if not 0.01 <= scale <= 1.0:
            raise V9xError("scale must be 0.01..1")
        window = args.get("window")
        if window is not None and region is not None:
            raise V9xError("pass region or window, not both")
        return {"region": region, "scale_percent": max(1, int(round(scale * 100))),
                "gray": bool(args.get("gray", False)), "window": window}

    @staticmethod
    def screenshot_content(capture, save_path):
        metadata, bmp = capture
        width, height, rows = decode_bmp(bmp)
        host_shape = metadata.pop("host_shape", None)
        if host_shape:
            region, scale_percent, gray = host_shape
            width, height, rows = shape_rgb(width, height, rows, region, scale_percent, gray)
            metadata["width"], metadata["height"] = width, height
        png = encode_png(width, height, rows, bool(metadata.get("gray")))
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

    def v9x_download(self, args):
        with self.connect() as client:
            return self.text(client.download(args["url"], args["guest_path"]))

    def v9x_update(self, args):
        package_dir = os.path.abspath(args["package_dir"])
        agent_path = os.path.join(package_dir, "V9XAGNT.EXE")
        helper_path = os.path.join(package_dir, "V9XSHOT.EXE")
        if not os.path.isfile(agent_path) or not os.path.isfile(helper_path):
            raise V9xError(
                "install package must contain V9XAGNT.EXE and V9XSHOT.EXE: %s"
                % package_dir
            )
        with open(agent_path, "rb") as handle:
            agent_data = handle.read()
        with open(helper_path, "rb") as handle:
            helper_data = handle.read()
        agent_crc = zlib.crc32(agent_data) & 0xFFFFFFFF
        helper_crc = zlib.crc32(helper_data) & 0xFFFFFFFF
        wait_seconds = int(args.get("wait_seconds", 120))
        with self.connect() as client:
            old_boot = client.hello["boot_counter"]
            old_build = client.hello["build_id"]
            # Hot update arrived in 0.6.0, but 0.5.x already sets its capability
            # bit (0x0200) and answers UPDATE with UNSUPPORTED_OPERATION, so
            # check the version before uploading anything.
            version = client.info().get("agent_version", "")
            try:
                too_old = tuple(int(part) for part in version.split(".")[:3]) < (0, 6, 0)
            except ValueError:
                too_old = False
            if too_old:
                raise V9xError(
                    "agent %s predates hot update (0.6.0); upgrade it once with "
                    "'scripts\\v9xctl.ps1 update -Source <package>', which stages "
                    "the update and reboots, then this tool works" % version
                )
            vnc_updated = vnc_restarted = False
            vnc_path = os.path.join(package_dir, "V9XVNC.EXE")
            if os.path.isfile(vnc_path):
                with open(vnc_path, "rb") as handle:
                    vnc_data = handle.read()
                try:
                    vnc_restarted = self._vnc_replace(client, vnc_data)
                    vnc_updated = True
                except V9xError:
                    pass  # the agent update goes ahead; v9xctl warns likewise
            # The tray's Settings... runs the installed SETUP.EXE; replace it
            # too, or it keeps the old version's options page.
            setup_updated = False
            setup_path = os.path.join(package_dir, "SETUP.EXE")
            if os.path.isfile(setup_path):
                try:
                    client.put_file(setup_path, "C:\\V9XREMOTE\\SETUP.EXE")
                    setup_updated = True
                except V9xError:
                    pass  # open in the guest; the agent update goes ahead
            client.put_bytes(agent_data, "C:\\V9XREMOTE\\V9XNEW.EXE")
            client.put_bytes(helper_data, "C:\\V9XREMOTE\\V9XSNEW.EXE")
            client.update_apply(len(agent_data), agent_crc, len(helper_data), helper_crc)
        hello = wait_for_update(
            self.host, self.port, old_boot, wait_seconds,
            self.reresolve if self.install_id else None, connect=self.reconnector(),
        )
        return self.text(
            {
                "updated": True,
                "previous_build": old_build,
                "previous_boot_counter": old_boot,
                "build_id": hello["build_id"],
                "boot_counter": hello["boot_counter"],
                "agent_bytes": len(agent_data),
                "vnc_updated": vnc_updated,
                "vnc_restarted": vnc_restarted,
                "setup_updated": setup_updated,
            }
        )

    def _vnc_probe(self, client):
        """True when a V9XVNC.EXE runs, False when not, None when it is not
        installed. Exit 3 ("already running") comes from builds before -status."""
        result = client.execute(0, application=VNC_EXE, arguments="-status",
                                timeout_ms=15000)
        if result["result"] != "ok":
            return None
        return result["exit_code"] in (0, 3)

    def _vnc_wait(self, client, running, seconds=10.0):
        deadline = time.time() + seconds
        while time.time() < deadline:
            time.sleep(0.5)
            if self._vnc_probe(client) == running:
                return True
        return False

    def _vnc_status(self, client):
        enabled, port, password_set = False, 5900, False
        try:
            ini = client.get_bytes("C:\\V9XREMOTE\\AGENT.INI").decode("ascii", "replace")
            enabled, port = parse_vnc_ini(ini)
            password_set = vnc_password_set(ini)
        except V9xError:
            pass
        running = self._vnc_probe(client)
        return {"installed": running is not None, "enabled": enabled,
                "running": bool(running), "vnc_port": port,
                "password_set": password_set}

    def v9x_vnc(self, args):
        action = args.get("action", "status")
        if action not in ("status", "start", "stop"):
            raise V9xError("action must be status, start or stop")
        with self.connect() as client:
            status = self._vnc_status(client)
            status["was_running"] = status["running"]
            if action != "status" and not status["installed"]:
                raise V9xError("%s is not installed on this guest; install agent "
                               "package 0.11.0 or later with SETUP.EXE" % VNC_EXE)
            if action == "start" and not status["running"]:
                if not status["enabled"]:
                    raise V9xError(
                        "VNC remote control is disabled on this guest ([vnc] "
                        "enabled=0 in AGENT.INI); its owner turns it on with the "
                        "VNC option in SETUP.EXE (or SETUP.EXE /S /vnc)")
                client.execute(0, application=VNC_EXE, detach=True)
                if not self._vnc_wait(client, True):
                    raise V9xError("V9XVNC.EXE did not start; see "
                                   "C:\\V9XREMOTE\\VNC.LOG on the guest")
                status["running"] = True
            elif action == "stop" and status["running"]:
                client.execute(0, application=VNC_EXE, arguments="-stop",
                               timeout_ms=15000)
                if not self._vnc_wait(client, False):
                    raise V9xError("V9XVNC.EXE did not stop within 10 s")
                status["running"] = False
        status["action"] = action
        return self.text(status)

    def _vnc_replace(self, client, data):
        """Replace a package's V9XVNC.EXE beside the agent's hot swap: stop it
        if it runs, upload, restart only if it was running."""
        was_running = self._vnc_probe(client)
        if was_running:
            client.execute(0, application=VNC_EXE, arguments="-stop", timeout_ms=15000)
            self._vnc_wait(client, False)
        client.put_bytes(data, VNC_EXE)
        restarted = False
        if was_running:
            client.execute(0, application=VNC_EXE, detach=True)
            restarted = self._vnc_wait(client, True)
        return restarted

    def v9x_reboot_with_proof(self, args):
        job_id = args.get("job_id") or ("mcp-%d" % (int(time.time()) % 100000000))
        wait_seconds = int(args.get("wait_seconds", 180))
        with self.connect() as client:
            old_boot = client.hello["boot_counter"]
            client.power(MSG_REBOOT, MSG_REBOOT_ACCEPTED, job_id)
        hello = wait_for_reboot(
            self.host, self.port, old_boot, job_id, wait_seconds,
            self.reresolve if self.install_id else None, connect=self.reconnector(),
        )
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

    def v9x_reset_with_proof(self, args):
        job_id = args.get("job_id") or ("mcp-%d" % (int(time.time()) % 100000000))
        wait_seconds = int(args.get("wait_seconds", 180))
        with self.connect() as client:
            client.require(CAP_HARD_RESET, "reset (agent 0.10.0+ on Windows 9x)")
            old_boot = client.hello["boot_counter"]
            client.power(MSG_RESET, MSG_RESET_ACCEPTED, job_id)
        hello = wait_for_reboot(
            self.host, self.port, old_boot, job_id, wait_seconds,
            self.reresolve if self.install_id else None, connect=self.reconnector(),
        )
        return self.text(
            {
                "reset": True,
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
        with self.connect(wait_seconds) as client:
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
    parser.add_argument(
        "--id",
        help="find the guest by install ID over mDNS instead of --host/--port, and "
        "again after it reboots onto a new address",
    )
    parser.add_argument("--query-address", help="send discovery queries unicast to ip[:port]")
    parser.add_argument(
        "--discover-seconds", type=float, default=3.0, help="discovery listen time (default 3)"
    )
    parser.add_argument(
        "--relay",
        help="default remote-help machine (M-XXXXXX) for every tool; a tool's own "
        "relay argument overrides it",
    )
    parser.add_argument(
        "--relay-server",
        help="update server for remote help (default V9X_RELAY_SERVER or "
        "https://michaeldale.com.au)",
    )
    args = parser.parse_args(argv)
    if args.id and not re.match(
        r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$", args.id
    ):
        parser.error("--id must be an install ID (8-4-4-4-12 hex)")
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
    McpServer(
        ToolHandler(
            args.host,
            args.port,
            args.timeout,
            install_id=args.id,
            query_address=args.query_address,
            discover_seconds=args.discover_seconds,
            allow_remote=args.allow_remote,
            relay=args.relay,
            relay_server=args.relay_server,
        )
    ).run()


if __name__ == "__main__":
    main()
