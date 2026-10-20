"""Inflate the cases written by `test_vnc.exe deflate <file>` with Python's
zlib, as one continuous stream the way a ZRLE viewer does, and check every
case comes back byte for byte (each must be complete after its sync flush).

    python tests/host/Test-VncDeflate.py <cases file>
"""

import struct
import sys
import zlib


def main(path):
    data = open(path, "rb").read()
    stream = zlib.decompressobj()
    offset = 0
    case = 0
    while offset < len(data):
        raw_length = struct.unpack_from("<I", data, offset)[0]
        raw = data[offset + 4:offset + 4 + raw_length]
        offset += 4 + raw_length
        packed_length = struct.unpack_from("<I", data, offset)[0]
        packed = data[offset + 4:offset + 4 + packed_length]
        offset += 4 + packed_length
        if packed[-4:] != b"\x00\x00\xff\xff":
            raise SystemExit("FAIL: case %d does not end with a sync flush" % case)
        out = stream.decompress(packed)
        if out != raw:
            raise SystemExit("FAIL: case %d inflated to %d bytes, expected %d"
                             % (case, len(out), raw_length))
        case += 1
    if stream.unconsumed_tail or stream.unused_data:
        raise SystemExit("FAIL: trailing data after the last case")
    print("PASS: %d deflate cases inflate with zlib" % case)


if __name__ == "__main__":
    main(sys.argv[1])
