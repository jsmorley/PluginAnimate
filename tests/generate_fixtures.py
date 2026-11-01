"""Generate small deterministic GIF/APNG fixtures using only Python's standard library."""
from pathlib import Path
import binascii
import struct
import zlib

root = Path(__file__).parent / "fixtures"
root.mkdir(exist_ok=True)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", binascii.crc32(kind + data))


def apng(name, poster=False):
    header = struct.pack(">IIBBBBB", 2, 1, 8, 6, 0, 0, 0)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
    out += chunk(b"acTL", struct.pack(">II", 4, 0))
    if poster:
        out += chunk(b"IDAT", zlib.compress(b"\0" + bytes([255, 0, 255, 255]) * 2))
    seq = 0
    # SOURCE, OVER (partial alpha), PREVIOUS, then transparent SOURCE.
    frames = [(2, 0, 0, 0, bytes([255, 0, 0, 255]) * 2),
              (1, 0, 2, 1, bytes([0, 0, 255, 128])),
              (1, 1, 1, 0, bytes([0, 255, 0, 255])),
              (1, 0, 0, 0, bytes([0, 0, 0, 0]))]
    for i, (width, x, disposal, blend, pixels) in enumerate(frames):
        numerator, denominator = [(1, 10), (25, 100), (0, 0), (1, 3)][i]
        control = struct.pack(">IIIIIHHBB", seq, width, 1, x, 0, numerator, denominator, disposal, blend)
        out += chunk(b"fcTL", control)
        seq += 1
        data = zlib.compress(b"\0" + pixels)
        if i == 0 and not poster:
            out += chunk(b"IDAT", data)
        else:
            out += chunk(b"fdAT", struct.pack(">I", seq) + data)
            seq += 1
    out += chunk(b"IEND", b"")
    (root / name).write_bytes(out)


apng("blend.apng")
apng("poster.png", True)
# Denominator 0 means 100, rather than a divide-by-zero or a missing delay.
denominator_zero = bytearray((root / "blend.apng").read_bytes())
pos = denominator_zero.index(b"fcTL")
data = bytearray(denominator_zero[pos + 4:pos + 30])
data[20:24] = struct.pack(">HH", 8, 0)
denominator_zero[pos - 4:pos + 34] = chunk(b"fcTL", data)
(root / "denominator-zero.apng").write_bytes(denominator_zero)
bad = bytearray((root / "blend.apng").read_bytes())
bad[-5] ^= 1
(root / "bad.apng").write_bytes(bad)

# A 2x1 GIF with transparent background and partial frames. Use a clear code
# before each pixel so every LZW code has the fixed initial 3-bit width.
gif = b"GIF89a" + struct.pack("<HHBBB", 2, 1, 0x81, 0, 0)
gif += bytes([0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255])
for i, (width, x, disposal, pixels) in enumerate([(2, 0, 1, [1, 1]), (1, 0, 3, [3]),
                                     (1, 1, 2, [2]), (1, 0, 1, [0])]):
    gif += b"\x21\xf9\x04" + bytes([(disposal << 2) | 1, [10, 25, 0, 33][i], 0, 0, 0])
    gif += b"\x2c" + struct.pack("<HHHHB", x, 0, width, 1, 0)
    codes = [c for pixel in pixels for c in (4, pixel)] + [5]
    packed = sum(code << (3 * i) for i, code in enumerate(codes))
    data = packed.to_bytes((len(codes) * 3 + 7) // 8, "little")
    gif += bytes([2, len(data)]) + data + b"\0"
(root / "disposal.gif").write_bytes(gif + b"\x3b")
(root / "no-delay.gif").write_bytes(gif[:25] + gif[33:] + b"\x3b")
# Standalone SOURCE frame with transparent, partially transparent, and opaque pixels.
alpha = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 4, 1, 8, 6, 0, 0, 0))
alpha += chunk(b"acTL", struct.pack(">II", 1, 0))
alpha += chunk(b"fcTL", struct.pack(">IIIIIHHBB", 0, 4, 1, 0, 0, 1, 10, 0, 0))
alpha += chunk(b"IDAT", zlib.compress(b"\0" + b"".join(bytes((40, 80, 200, a)) for a in (0, 64, 128, 255))))
alpha += chunk(b"IEND", b"")
(root / "alpha.apng").write_bytes(alpha)
print("Generated GIF/APNG fixtures")
