"""Generate the public example's loading GIF; no third-party dependencies."""
from pathlib import Path
import math
import struct

size = 125
out = bytearray(b"GIF89a" + struct.pack("<HHBBB", size, size, 0xF3, 0, 0))
palette = [(0, 0, 0)] + [(30 + i * 27, 70 + i * 23, 110 + i * 20) for i in range(8)]
palette += [(0, 0, 0)] * (16 - len(palette))
out.extend(component for color in palette for component in color)
out.extend(b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00")

for frame in range(8):
    pixels = [0] * (size * size)
    for dot in range(8):
        angle = 2 * math.pi * dot / 8 - math.pi / 2
        cx, cy = 62 + 40 * math.cos(angle), 62 + 40 * math.sin(angle)
        color = 8 - ((frame - dot) % 8)
        for y in range(size):
            for x in range(size):
                if (x - cx) ** 2 + (y - cy) ** 2 <= 7 ** 2:
                    pixels[y * size + x] = color
    out.extend(b"\x21\xf9\x04\x09\x08\x00\x00\x00")  # 80 ms, transparent, clear after frame
    out.extend(b"\x2c" + struct.pack("<HHHHB", 0, 0, size, size, 0))
    # Reset before each pixel, keeping every LZW code five bits wide.
    data = bytearray()
    bits = bit_count = 0
    for code in [c for pixel in pixels for c in (16, pixel)] + [17]:
        bits |= code << bit_count
        bit_count += 5
        while bit_count >= 8:
            data.append(bits & 255)
            bits >>= 8
            bit_count -= 8
    if bit_count:
        data.append(bits & 255)
    out.append(4)
    for start in range(0, len(data), 255):
        block = data[start:start + 255]
        out.append(len(block))
        out.extend(block)
    out.append(0)
out.append(0x3B)
path = Path(__file__).parent / "Animate" / "@Resources" / "Loading.gif"
path.parent.mkdir(parents=True, exist_ok=True)
path.write_bytes(out)
print(f"Generated {path}: 125x125, 8 frames, 80 ms/frame")
