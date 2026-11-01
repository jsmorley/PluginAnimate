"""Generate WebP fixtures. Requires Pillow only when regenerating these files.

RIFF animation controls are assembled explicitly to exercise offsets, blend,
disposal, zero delays, alpha, and malformed inputs without encoder optimization.
Run: python -m pip install Pillow; python tests/generate_webp_fixtures.py
"""
from io import BytesIO
from pathlib import Path
import struct
from PIL import Image

root = Path(__file__).parent / "fixtures"


def chunk(kind, data):
    return kind + struct.pack("<I", len(data)) + data + b"\0" * (len(data) % 2)


def u24(value):
    return value.to_bytes(3, "little")


def image(width, height, pixels, lossless=True):
    out = BytesIO()
    Image.frombytes("RGBA", (width, height), pixels).save(
        out, format="WEBP", lossless=lossless, quality=95, exact=True)
    data = out.getvalue()
    result = b""
    pos = 12
    while pos < len(data):
        length = int.from_bytes(data[pos + 4:pos + 8], "little")
        end = pos + 8 + length + length % 2
        if data[pos:pos + 4] in (b"VP8 ", b"VP8L", b"ALPH"):
            result += data[pos:end]
        pos = end
    return result


def animation(name, width, height, frames):
    data = chunk(b"VP8X", bytes([0x12, 0, 0, 0]) + u24(width - 1) + u24(height - 1))
    data += chunk(b"ANIM", struct.pack("<IH", 0, 3))
    for w, h, x, y, delay, flags, payload in frames:
        control = b"".join(u24(n) for n in (x // 2, y // 2, w - 1, h - 1, delay)) + bytes([flags])
        data += chunk(b"ANMF", control + payload)
    result = b"RIFF" + struct.pack("<I", len(data) + 4) + b"WEBP" + data
    (root / name).write_bytes(result)
    return result


red = image(4, 1, bytes([255, 0, 0, 255]) * 4)
blue = image(2, 1, bytes([0, 0, 255, 128]) * 2)
green = image(2, 1, bytes([0, 255, 0, 255]) * 2)
clear = image(4, 1, bytes(16))
blend = animation("blend.webp", 4, 1, [
    (4, 1, 0, 0, 80, 2, red),
    (2, 1, 0, 0, 250, 1, blue),  # OVER then BACKGROUND disposal.
    (2, 1, 2, 0, 0, 2, green),  # Even offset; preserve disposed left half.
    (4, 1, 0, 0, 333, 2, clear),  # Transparent SOURCE replaces entire canvas.
])
alpha = image(4, 1, b"".join(bytes((40, 80, 200, a)) for a in (0, 64, 128, 255)))
animation("alpha.webp", 4, 1, [(4, 1, 0, 0, 100, 2, alpha)])
lossy = image(4, 2, bytes([255, 0, 0, 255]) * 8, False)
animation("lossy.webp", 4, 2, [(4, 2, 0, 0, 40, 2, lossy), (4, 2, 0, 0, 60, 2, lossy)])
animation("excessive-duration.webp", 4, 1, [(4, 1, 0, 0, 0xffffff, 2, red)] * 129)
bad = bytearray(blend)
bad[bad.index(b"VP8L") + 8] = 0  # Invalid lossless bitstream signature.
(root / "bad.webp").write_bytes(bad)
truncated = blend[:-12]
truncated = truncated[:4] + struct.pack("<I", len(truncated) - 8) + truncated[8:]
(root / "truncated.webp").write_bytes(truncated)
oversized = bytearray(blend)
oversized[24:30] = u24(8191) * 2
(root / "oversized.webp").write_bytes(oversized)
(root / "static.webp").write_bytes(b"RIFF" + struct.pack("<I", len(red) + 4) + b"WEBP" + red)
print("Generated WebP fixtures")
