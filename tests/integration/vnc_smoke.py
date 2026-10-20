"""Smoke test for V9XVNC.EXE: a small RFB client that needs no viewer.

Connects, negotiates RFB 3.3/3.8 with security None, asks for a pixel format
and encodings, decodes full and incremental framebuffer updates (Raw,
Hextile, DesktopSize), and writes the result as a PNG. Optionally sends
pointer and key events, and compares the frame with a BMP from
`v9xctl screenshot -OutFile x.bmp`.

    python tests/integration/vnc_smoke.py --port 5970 --out frame.png
    python tests/integration/vnc_smoke.py --port 5970 --bpp 8 --encoding raw
    python tests/integration/vnc_smoke.py --port 5970 --compare shot.bmp
    python tests/integration/vnc_smoke.py --port 5970 --move 100 200 --click
    python tests/integration/vnc_smoke.py --port 5970 --type "hello"

Exit code 0 on success, 1 on any protocol or comparison failure.
"""

import argparse
import os
import subprocess
import socket
import struct
import sys
import time
import zlib

ENC_RAW = 0
ENC_COPYRECT = 1
ENC_ZRLE = 16
ENC_HEXTILE = 5
ENC_DESKTOP_SIZE = -223
ENC_RICH_CURSOR = -239
ENC_POINTER_POS = -232

AUTH_HELPER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..",
                           "build", "host-tests", "test_vnc.exe")

FORMATS = {
    # bpp: (bpp, depth, big_endian, true_colour, rmax, gmax, bmax, rsh, gsh, bsh)
    32: (32, 24, 0, 1, 255, 255, 255, 16, 8, 0),
    16: (16, 16, 0, 1, 31, 63, 31, 11, 5, 0),
    8: (8, 8, 0, 1, 7, 7, 3, 0, 3, 6),  # BGR233, as TigerVNC's low-colour mode
}


class Rfb:
    def __init__(self, host, port, version, timeout, password=None):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.buffer = b""
        self.zstream = zlib.decompressobj()
        self.cut_text = []
        server = self.read(12)
        if not server.startswith(b"RFB 003."):
            raise RuntimeError("bad server version %r" % server)
        # Speak the lower of the two versions, as viewers do (a busy server
        # answers 3.3 to turn a second viewer away).
        version = min(version, int(server[8:11]))
        self.sock.sendall(("RFB 003.%03d\n" % version).encode("ascii"))
        if version >= 7:
            count = self.read(1)[0]
            if count == 0:
                self.refused()
            types = self.read(count)
            if password is not None and 2 in types:
                self.sock.sendall(b"\x02")
                self.vnc_auth(password, version)
            elif 1 in types:
                self.sock.sendall(b"\x01")
                if version >= 8 and struct.unpack(">I", self.read(4))[0] != 0:
                    raise RuntimeError("security result failed")
            else:
                raise RuntimeError("server wants security types %r" % list(types))
        else:
            security = struct.unpack(">I", self.read(4))[0]
            if security == 0:
                self.refused()
            if security == 2 and password is not None:
                self.vnc_auth(password, version)
            elif security != 1:
                raise RuntimeError("RFB 3.3 security type %d" % security)
        self.sock.sendall(b"\x01")  # shared
        width, height = struct.unpack(">HH", self.read(4))
        self.server_format = self.read(16)
        name_length = struct.unpack(">I", self.read(4))[0]
        self.name = self.read(name_length).decode("latin-1")
        self.resize(width, height)
        self.set_format(32)

    def vnc_auth(self, password, version):
        challenge = self.read(16)
        # The response comes from test_vnc.exe, i.e. the same C DES the
        # server uses, which the host tests check against FIPS vectors.
        answer = subprocess.run([AUTH_HELPER, "auth", password, challenge.hex()],
                                capture_output=True, text=True, check=True)
        self.sock.sendall(bytes.fromhex(answer.stdout.strip()))
        if struct.unpack(">I", self.read(4))[0] != 0:
            if version >= 8:
                self.refused()
            raise RuntimeError("authentication failed")

    def refused(self):
        length = struct.unpack(">I", self.read(4))[0]
        raise RuntimeError("server refused the connection: %s"
                           % self.read(length).decode("latin-1"))

    def read(self, count):
        while len(self.buffer) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("server closed the connection")
            self.buffer += chunk
        data, self.buffer = self.buffer[:count], self.buffer[count:]
        return data

    def resize(self, width, height):
        self.width = width
        self.height = height
        self.frame = [bytearray(width * 3) for _ in range(height)]

    def set_format(self, bpp):
        fields = FORMATS[bpp]
        self.bpp = bpp
        self.bytes = bpp // 8
        (_, _, self.big_endian, _, self.rmax, self.gmax, self.bmax,
         self.rsh, self.gsh, self.bsh) = fields
        self.sock.sendall(b"\x00\x00\x00\x00" + struct.pack(
            ">BBBBHHHBBBxxx", *fields))

    def set_encodings(self, encodings):
        self.sock.sendall(struct.pack(">BxH", 2, len(encodings)) +
                          b"".join(struct.pack(">i", e) for e in encodings))

    def request(self, incremental):
        self.sock.sendall(struct.pack(">BBHHHH", 3, 1 if incremental else 0,
                                      0, 0, self.width, self.height))

    def pointer(self, mask, x, y):
        self.sock.sendall(struct.pack(">BBHH", 5, mask, x, y))

    def key(self, down, keysym):
        self.sock.sendall(struct.pack(">BBxxI", 4, 1 if down else 0, keysym))

    def pixel(self, data, offset=0):
        raw = data[offset:offset + self.bytes]
        value = int.from_bytes(raw, "big" if self.big_endian else "little")
        r = ((value >> self.rsh) & self.rmax) * 255 // self.rmax
        g = ((value >> self.gsh) & self.gmax) * 255 // self.gmax
        b = ((value >> self.bsh) & self.bmax) * 255 // self.bmax
        return bytes((r, g, b))

    def fill(self, x, y, w, h, rgb):
        for row in range(y, y + h):
            self.frame[row][x * 3:(x + w) * 3] = rgb * w

    def read_update(self):
        """Read one server message; return the rectangle count of an update,
        or None for a non-update message."""
        kind = self.read(1)[0]
        if kind == 1:  # SetColourMapEntries (not used with true colour)
            _, _, count = struct.unpack(">BHH", self.read(5))
            self.read(count * 6)
            return None
        if kind == 2:  # Bell
            return None
        if kind == 3:
            self.read(3)
            self.cut_text.append(self.read(struct.unpack(">I", self.read(4))[0]))
            return None
        if kind != 0:
            raise RuntimeError("unexpected server message %d" % kind)
        self.read(1)
        rects = struct.unpack(">H", self.read(2))[0]
        for _ in range(rects):
            x, y, w, h, encoding = struct.unpack(">HHHHi", self.read(12))
            if encoding == ENC_DESKTOP_SIZE:
                self.resize(w, h)
                self.stats["desktop_size"] += 1
            elif encoding == ENC_RICH_CURSOR:
                pixels = self.read(w * h * self.bytes)
                mask = self.read(((w + 7) // 8) * h)
                self.cursor = (x, y, w, h, pixels, mask)
                self.stats["cursor_shapes"] += 1
            elif encoding == ENC_POINTER_POS:
                self.pointer_pos = (x, y)
                self.stats["pointer_pos"] += 1
            elif encoding == ENC_COPYRECT:
                sx, sy = struct.unpack(">HH", self.read(4))
                if sx + w > self.width or sy + h > self.height:
                    raise RuntimeError("CopyRect source outside the screen")
                block = [bytes(self.frame[sy + row][sx * 3:(sx + w) * 3]) for row in range(h)]
                for row in range(h):
                    self.frame[y + row][x * 3:(x + w) * 3] = block[row]
                self.stats["copyrect"] += 1
            elif encoding == ENC_ZRLE:
                self.check_rect(x, y, w, h)
                self.zrle(x, y, w, h)
                self.stats["zrle"] += 1
            elif encoding == ENC_RAW:
                self.check_rect(x, y, w, h)
                data = self.read(w * h * self.bytes)
                for row in range(h):
                    line = bytearray()
                    for col in range(w):
                        line += self.pixel(data, (row * w + col) * self.bytes)
                    self.frame[y + row][x * 3:(x + w) * 3] = line
                self.stats["raw"] += 1
            elif encoding == ENC_HEXTILE:
                self.check_rect(x, y, w, h)
                self.hextile(x, y, w, h)
                self.stats["hextile"] += 1
            else:
                raise RuntimeError("unexpected encoding %d" % encoding)
        return rects

    def check_rect(self, x, y, w, h):
        if w == 0 or h == 0 or x + w > self.width or y + h > self.height:
            raise RuntimeError("rectangle %r outside %dx%d" %
                               ((x, y, w, h), self.width, self.height))

    def cpixel(self, data, offset):
        # This client's formats are little endian; 32 bpp depth 24 keeps its
        # colour in the low three bytes, so CPIXEL is 3 bytes there.
        if self.bpp == 32:
            return self.pixel(data[offset:offset + 3] + b"\x00"), 3
        return self.pixel(data, offset), self.bytes

    def zrle(self, x, y, w, h):
        length = struct.unpack(">I", self.read(4))[0]
        data = self.zstream.decompress(self.read(length))
        o = 0
        for ty in range(y, y + h, 64):
            th = min(64, y + h - ty)
            for tx in range(x, x + w, 64):
                tw = min(64, x + w - tx)
                sub = data[o]
                o += 1
                pixels = []
                if sub == 0:
                    for _ in range(tw * th):
                        colour, n = self.cpixel(data, o)
                        o += n
                        pixels.append(colour)
                elif sub == 1:
                    colour, n = self.cpixel(data, o)
                    o += n
                    pixels = [colour] * (tw * th)
                elif 2 <= sub <= 16:
                    palette = []
                    for _ in range(sub):
                        colour, n = self.cpixel(data, o)
                        o += n
                        palette.append(colour)
                    bits = 1 if sub == 2 else 2 if sub <= 4 else 4
                    for _ in range(th):
                        row_bytes = (tw * bits + 7) // 8
                        value = int.from_bytes(data[o:o + row_bytes], "big")
                        total = row_bytes * 8
                        for col in range(tw):
                            shift = total - bits * (col + 1)
                            pixels.append(palette[(value >> shift) & ((1 << bits) - 1)])
                        o += row_bytes
                elif sub == 128 or sub >= 130:
                    palette = []
                    if sub >= 130:
                        for _ in range(sub - 128):
                            colour, n = self.cpixel(data, o)
                            o += n
                            palette.append(colour)
                    while len(pixels) < tw * th:
                        if sub == 128:
                            colour, n = self.cpixel(data, o)
                            o += n
                            run_follows = True
                        else:
                            index = data[o]
                            o += 1
                            colour = palette[index & 127]
                            run_follows = bool(index & 128)
                        run = 1
                        if run_follows:
                            while data[o] == 255:
                                run += 255
                                o += 1
                            run += data[o]
                            o += 1
                        pixels.extend([colour] * run)
                    if len(pixels) != tw * th:
                        raise RuntimeError("ZRLE run overflows its tile")
                    self.stats["zrle_rle_tiles"] += 1
                else:
                    raise RuntimeError("invalid ZRLE subencoding %d" % sub)
                for row in range(th):
                    self.frame[ty + row][tx * 3:(tx + tw) * 3] = b"".join(
                        pixels[row * tw:(row + 1) * tw])
                self.stats["zrle_tiles"] += 1
        if o != len(data):
            raise RuntimeError("ZRLE rectangle has %d stray bytes" % (len(data) - o))

    def cut(self, text):
        data = text.encode("latin-1")
        self.sock.sendall(struct.pack(">BxxxI", 6, len(data)) + data)

    def hextile(self, x, y, w, h):
        bg = fg = None
        for ty in range(y, y + h, 16):
            th = min(16, y + h - ty)
            for tx in range(x, x + w, 16):
                tw = min(16, x + w - tx)
                flags = self.read(1)[0]
                if flags & 1:
                    data = self.read(tw * th * self.bytes)
                    for row in range(th):
                        line = bytearray()
                        for col in range(tw):
                            line += self.pixel(data, (row * tw + col) * self.bytes)
                        self.frame[ty + row][tx * 3:(tx + tw) * 3] = line
                    self.stats["hex_raw_tiles"] += 1
                    continue
                if flags & 2:
                    bg = self.pixel(self.read(self.bytes))
                if bg is None:
                    raise RuntimeError("hextile tile without a background")
                self.fill(tx, ty, tw, th, bg)
                if flags & 4:
                    fg = self.pixel(self.read(self.bytes))
                if flags & 8:
                    count = self.read(1)[0]
                    for _ in range(count):
                        if flags & 16:
                            colour = self.pixel(self.read(self.bytes))
                        else:
                            if fg is None:
                                raise RuntimeError("hextile subrect without a foreground")
                            colour = fg
                        xy, wh = self.read(2)
                        sx, sy = xy >> 4, xy & 15
                        sw, sh = (wh >> 4) + 1, (wh & 15) + 1
                        if sx + sw > tw or sy + sh > th:
                            raise RuntimeError("hextile subrect outside its tile")
                        self.fill(tx + sx, ty + sy, sw, sh, colour)
                    self.stats["subrects"] += count
                self.stats["tiles"] += 1

    def cursor_png(self, path):
        hot_x, hot_y, w, h, pixels, mask = self.cursor
        rows = []
        for row in range(h):
            line = bytearray()
            for col in range(w):
                opaque = mask[row * ((w + 7) // 8) + col // 8] & (0x80 >> (col % 8))
                # transparent pixels shown as magenta
                line += (self.pixel(pixels, (row * w + col) * self.bytes)
                         if opaque else b"\xff\x00\xff")
            rows.append(line)
        write_png(path, w, h, rows)
        return hot_x, hot_y, w, h

    def png(self, path):
        write_png(path, self.width, self.height, self.frame)


def write_png(path, width, height, frame):
    raw = b"".join(b"\x00" + bytes(row) for row in frame)

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width,
                                                height, 8, 2, 0, 0, 0)))
        handle.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        handle.write(chunk(b"IEND", b""))


def load_bmp(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if data[:2] != b"BM":
        raise RuntimeError("%s is not a BMP" % path)
    offset = struct.unpack_from("<I", data, 10)[0]
    width, height = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise RuntimeError("expected a 24-bit BMP, got %d" % bpp)
    stride = (width * 3 + 3) & ~3
    rows = []
    for row in range(abs(height)):
        source = row if height < 0 else abs(height) - 1 - row
        line = data[offset + source * stride:offset + source * stride + width * 3]
        rows.append(bytes(b for i in range(0, len(line), 3)
                          for b in (line[i + 2], line[i + 1], line[i])))
    return width, abs(height), rows


def compare(client, path, tolerance):
    width, height, rows = load_bmp(path)
    if (width, height) != (client.width, client.height):
        raise RuntimeError("size mismatch: BMP %dx%d, VNC %dx%d" %
                           (width, height, client.width, client.height))
    same = 0
    for y in range(height):
        a = rows[y]
        b = client.frame[y]
        for i in range(0, width * 3, 3):
            if (abs(a[i] - b[i]) <= tolerance and
                    abs(a[i + 1] - b[i + 1]) <= tolerance and
                    abs(a[i + 2] - b[i + 2]) <= tolerance):
                same += 1
    return same / float(width * height)


def keysym(char):
    special = {"\n": 0xff0d, "\r": 0xff0d, "\t": 0xff09, "\b": 0xff08}
    return special.get(char, ord(char))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5900)
    parser.add_argument("--rfb", type=int, choices=(3, 7, 8), default=8,
                        help="RFB minor version to ask for")
    parser.add_argument("--bpp", type=int, choices=(8, 16, 32), default=32)
    parser.add_argument("--encoding", choices=("zrle", "hextile", "raw"), default="hextile")
    parser.add_argument("--copyrect", action="store_true", help="offer CopyRect")
    parser.add_argument("--password", help="answer VNC authentication with this password")
    parser.add_argument("--clip-send", help="send this text as the viewer's clipboard")
    parser.add_argument("--clip-expect", help="wait (with --watch) for the server to send this clipboard text")
    parser.add_argument("--out", help="write the decoded frame as PNG")
    parser.add_argument("--compare", help="BMP to compare the frame against")
    parser.add_argument("--min-match", type=float, default=0.99)
    parser.add_argument("--incremental", type=int, default=2,
                        help="incremental updates to wait for after the full one")
    parser.add_argument("--move", type=int, nargs=2, metavar=("X", "Y"))
    parser.add_argument("--click", action="store_true",
                        help="left click at the --move position")
    parser.add_argument("--type", dest="text", help="type this text with key events")
    parser.add_argument("--rich-cursor", action="store_true",
                        help="ask for the RichCursor and PointerPos pseudo-encodings")
    parser.add_argument("--cursor-out", help="write the last cursor shape as PNG")
    parser.add_argument("--hold", type=float, default=0.0, metavar="SECONDS",
                        help="stay connected this long at the end (to test a second viewer)")
    parser.add_argument("--watch", type=float, default=0.0, metavar="SECONDS",
                        help="keep requesting updates this long, reporting size changes")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    client = Rfb(args.host, args.port, args.rfb, args.timeout, args.password)
    client.stats = dict.fromkeys(("raw", "hextile", "desktop_size", "tiles",
                                  "hex_raw_tiles", "subrects", "cursor_shapes",
                                  "pointer_pos", "copyrect", "zrle",
                                  "zrle_tiles", "zrle_rle_tiles"), 0)
    client.cursor = None
    client.pointer_pos = None
    print("server %r %dx%d" % (client.name, client.width, client.height))
    if args.bpp != 32:
        client.set_format(args.bpp)
    encodings = {"zrle": [ENC_ZRLE, ENC_HEXTILE, ENC_RAW],
                 "hextile": [ENC_HEXTILE, ENC_RAW], "raw": [ENC_RAW]}[args.encoding]
    if args.copyrect:
        encodings = [ENC_COPYRECT] + encodings
    pseudo = [ENC_DESKTOP_SIZE]
    if args.rich_cursor:
        pseudo += [ENC_RICH_CURSOR, ENC_POINTER_POS]
    client.set_encodings(encodings + pseudo)

    started = time.time()
    client.request(False)
    while client.read_update() is None:
        pass
    elapsed = time.time() - started
    print("full update %.2fs %r" % (elapsed, client.stats))

    if args.clip_send is not None:
        client.cut(args.clip_send)
    if args.move:
        client.pointer(0, args.move[0], args.move[1])
        if args.click:
            client.pointer(1, args.move[0], args.move[1])
            client.pointer(0, args.move[0], args.move[1])
    if args.text:
        for char in args.text:
            client.key(True, keysym(char))
            client.key(False, keysym(char))

    client.sock.settimeout(5.0)
    for index in range(args.incremental):
        client.request(True)
        try:
            while True:
                rects = client.read_update()
                if rects is not None:
                    break
            print("incremental %d: %d rects" % (index + 1, rects))
        except socket.timeout:
            print("incremental %d: no change within 5s" % (index + 1))
            break
    if args.watch:
        # Keep a viewer's request loop going (to see a mode change arrive).
        deadline = time.time() + args.watch
        sizes = [(client.width, client.height)]
        client.sock.settimeout(1.0)
        client.request(True)
        while time.time() < deadline:
            try:
                if client.read_update() is None:
                    continue
            except socket.timeout:
                continue
            if (client.width, client.height) != sizes[-1]:
                sizes.append((client.width, client.height))
                print("desktop size now %dx%d" % sizes[-1])
            client.request(True)
        print("sizes seen: %s" % " -> ".join("%dx%d" % s for s in sizes))
        print("stats after watch: %r" % client.stats)
    if client.cut_text:
        print("clipboard from server: %r" % [t.decode("latin-1") for t in client.cut_text])
    if args.clip_expect is not None and args.clip_expect.encode("latin-1") not in client.cut_text:
        print("FAIL: the server never sent clipboard text %r" % args.clip_expect)
        return 1
    client.sock.settimeout(args.timeout)

    if args.out:
        client.png(args.out)
        print("wrote %s" % args.out)
    if args.cursor_out:
        if client.cursor is None:
            print("FAIL: no cursor shape received")
            return 1
        print("cursor hotspot %d,%d size %dx%d -> %s"
              % (client.cursor_png(args.cursor_out) + (args.cursor_out,)))
    if client.pointer_pos is not None:
        print("guest pointer position %d,%d" % client.pointer_pos)
    if args.hold:
        time.sleep(args.hold)
    if args.compare:
        # BGR233's 2-bit blue steps by 85, so 8 bpp can be 42 off per channel.
        ratio = compare(client, args.compare, 48 if args.bpp == 8 else 8)
        print("match %.4f against %s" % (ratio, args.compare))
        if ratio < args.min_match:
            print("FAIL: match below %.4f" % args.min_match)
            return 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError) as error:
        print("FAIL: %s" % error)
        sys.exit(1)
