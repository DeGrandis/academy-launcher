"""Converts Clone Wars .xbt textures to and from PNG.

.xbt layout (little endian):
  u32 width, u32 height, u32 mip count, u32 Xbox D3D format, u32 size of mip 0, 12 zero bytes
  then every mip level, largest first. Block formats store whole 4x4 blocks (at least one per level);
  16-bit formats are swizzled (Morton order) and each level is padded to 4 bytes.

Formats: 0x0C DXT1, 0x0E DXT3, 0x05 R5G6B5 (swizzled), 0x04 A4R4G4B4 (swizzled).

Usage:
  python tools/assets/xbt.py info   <file.xbt> ...
  python tools/assets/xbt.py decode <file.xbt> <out.png>
  python tools/assets/xbt.py encode <in.png> <out.xbt> [--like original.xbt | --format dxt1|dxt3|r5g6b5|a4r4g4b4]
  python tools/assets/xbt.py export <dir-with-xbt> <out-dir>      (every .xbt to .png)

encode keeps the original's format and mip count with --like; image sizes must be powers of two.
"""

import io
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

DXT1, DXT3, R5G6B5, A4R4G4B4 = 0x0C, 0x0E, 0x05, 0x04
FORMAT_NAMES = {DXT1: "dxt1", DXT3: "dxt3", R5G6B5: "r5g6b5", A4R4G4B4: "a4r4g4b4"}
HEADER = struct.Struct("<5I12x")


def level_size(fmt, width, height):
    if fmt in (DXT1, DXT3):
        blocks = max(1, (width + 3) // 4) * max(1, (height + 3) // 4)
        return blocks * (8 if fmt == DXT1 else 16)
    return (width * height * 2 + 3) & ~3


def morton_table(width, height):
    """Index of each (y, x) texel in the Xbox swizzled layout (interleaved x/y bits, x first)."""
    xs = np.arange(width)
    ys = np.arange(height)
    x_bits = np.zeros(width, dtype=np.int64)
    y_bits = np.zeros(height, dtype=np.int64)
    shift = 0
    bit = 0
    while (1 << bit) < max(width, height):
        if (1 << bit) < width:
            x_bits |= ((xs >> bit) & 1) << shift
            shift += 1
        if (1 << bit) < height:
            y_bits |= ((ys >> bit) & 1) << shift
            shift += 1
        bit += 1
    return y_bits[:, None] | x_bits[None, :]


def dds_header(fmt, width, height):
    four_cc = b"DXT1" if fmt == DXT1 else b"DXT3"
    size = level_size(fmt, width, height)
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, 0x81007, height, width, size, 0, 1)
    header += struct.pack("<II4s20x", 32, 0x4, four_cc)
    header += struct.pack("<I16x", 0x1000)
    return header


def decode_level(fmt, data, width, height):
    if fmt in (DXT1, DXT3):
        image = Image.open(io.BytesIO(dds_header(fmt, width, height) + data))
        image.load()
        return image.convert("RGBA")
    texels = np.frombuffer(data[:width * height * 2], dtype="<u2")
    linear = texels[morton_table(width, height)]
    if fmt == R5G6B5:
        r = ((linear >> 11) & 0x1F) * 255 // 31
        g = ((linear >> 5) & 0x3F) * 255 // 63
        b = (linear & 0x1F) * 255 // 31
        a = np.full_like(r, 255)
    elif fmt == A4R4G4B4:
        a = ((linear >> 12) & 0xF) * 17
        r = ((linear >> 8) & 0xF) * 17
        g = ((linear >> 4) & 0xF) * 17
        b = (linear & 0xF) * 17
    else:
        raise ValueError(f"unsupported format 0x{fmt:X}")
    return Image.fromarray(np.stack([r, g, b, a], axis=-1).astype(np.uint8), "RGBA")


def encode_level(fmt, image, width, height):
    if fmt in (DXT1, DXT3):
        buffer = io.BytesIO()
        source = image if fmt == DXT3 else image.convert("RGB") if image.mode == "RGBA" and _opaque(image) else image
        if width < 4 or height < 4:
            # The encoder works on whole blocks: encode a 4x4 image whose top-left holds the level.
            padded = Image.new(source.mode, (max(4, width), max(4, height)))
            padded.paste(source, (0, 0))
            source = padded
        source.save(buffer, "DDS", pixel_format="DXT1" if fmt == DXT1 else "DXT3")
        return buffer.getvalue()[128:128 + level_size(fmt, width, height)]
    pixels = np.asarray(image.convert("RGBA"), dtype=np.uint32)
    r, g, b, a = (pixels[..., channel] for channel in range(4))
    if fmt == R5G6B5:
        linear = ((r * 31 + 127) // 255) << 11 | ((g * 63 + 127) // 255) << 5 | ((b * 31 + 127) // 255)
    else:
        linear = ((a + 8) // 17) << 12 | ((r + 8) // 17) << 8 | ((g + 8) // 17) << 4 | ((b + 8) // 17)
    swizzled = np.zeros(width * height, dtype="<u2")
    swizzled[morton_table(width, height)] = linear.astype("<u2")
    data = swizzled.tobytes()
    return data + bytes(level_size(fmt, width, height) - len(data))


def _opaque(image):
    return image.getextrema()[3][0] == 255


def read_xbt(path):
    data = Path(path).read_bytes()
    width, height, mips, fmt, size0 = HEADER.unpack_from(data, 0)
    levels = []
    offset = HEADER.size
    for level in range(mips):
        w, h = max(1, width >> level), max(1, height >> level)
        size = level_size(fmt, w, h)
        levels.append((w, h, data[offset:offset + size]))
        offset += size
    return width, height, mips, fmt, levels


def decode(path, out):
    width, height, mips, fmt, levels = read_xbt(path)
    w, h, data = levels[0]
    decode_level(fmt, data, w, h).save(out)


def encode(png, out, fmt, mips):
    image = Image.open(png).convert("RGBA")
    width, height = image.size
    if width & (width - 1) or height & (height - 1):
        raise ValueError(f"{png}: size {width}x{height} is not a power of two")
    if mips is None:
        mips = max(width, height).bit_length()
    body = bytearray()
    for level in range(mips):
        w, h = max(1, width >> level), max(1, height >> level)
        scaled = image if level == 0 else image.resize((w, h), Image.LANCZOS)
        body += encode_level(fmt, scaled, w, h)
    header = HEADER.pack(width, height, mips, fmt, level_size(fmt, width, height))
    Path(out).write_bytes(header + body)


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    command = argv[1]
    if command == "info":
        for path in argv[2:]:
            width, height, mips, fmt, _ = read_xbt(path)
            print(f"{path}: {width}x{height} {FORMAT_NAMES.get(fmt, hex(fmt))} {mips} mips")
    elif command == "decode":
        decode(argv[2], argv[3])
    elif command == "encode":
        fmt, mips = DXT1, None
        if "--like" in argv:
            _, _, mips, fmt, _ = read_xbt(argv[argv.index("--like") + 1])
        if "--format" in argv:
            name = argv[argv.index("--format") + 1].lower()
            fmt = {v: k for k, v in FORMAT_NAMES.items()}[name]
        encode(argv[2], argv[3], fmt, mips)
    elif command == "export":
        out = Path(argv[3])
        out.mkdir(parents=True, exist_ok=True)
        for path in sorted(Path(argv[2]).glob("*.xbt")):
            try:
                decode(path, out / (path.stem + ".png"))
            except Exception as error:  # report and continue with the rest
                print(f"{path.name}: {error}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv)
