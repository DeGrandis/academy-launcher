"""Reads and writes Clone Wars compiled terrain (.xxw), as loaded by the terrain constructor at 0x248DD0.

Layout (little endian):
  header (0x480 bytes)
    +0x00 u32 header size (0x480)      +0x04 u32 version (3)
    +0x08 u32 0x400                    +0x0C u32 grid size N (heights per side, 2^k + 1)
    +0x10 u32 flag: the first 0x20000-byte map follows
    +0x14 f32 grid scale (world units per cell)
    +0x18 f32 height scale (world units per height step)
    +0x1C i32[4] extents in cells (x min, x max, z min, z max)
    +0x40 f32[16] texture tiling per layer
    +0x80 char[16][0x20] main textures      +0x280 char[16][0x20] detail textures
  heights  i16[N][N]
  u32 0x12345678
  [flag] u8[0x20000]   then u8[0x40000], u8[0x20000]   (texture blend / colour maps, fixed size)
  u32 0x12345678
  render quadtree (0x2474F0), magic 'eRtQ' (0x51547265):
    'eRtQ', u32 1, u16 size, u16 a, u16 b, u16 patch, u16 c, f32 d, f32 e, u32 levels, u32 0x3E0, u32 8, 'eRtQ'
    leaves ((size / patch)^2): u32, u16, u16, u16, u16, u32, u8[4], then 81 vertices x 12 bytes (9 x 9 patch)
    per level, levels-1 down to 0: 'eRtQ', then n*n nodes of 8 bytes (n halves each level, rounded up)
    'eRtQ'
"""

import struct
import sys
from pathlib import Path

MAGIC = 0x12345678
QT = 0x51547265


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        chunk = self.data[self.pos:self.pos + n]
        if len(chunk) != n:
            raise ValueError(f"truncated at {self.pos:#x}")
        self.pos += n
        return chunk

    def unpack(self, fmt):
        return struct.unpack(fmt, self.take(struct.calcsize(fmt)))


class Terrain:
    pass


def read(path):
    data = Path(path).read_bytes()
    r = Reader(data)
    t = Terrain()
    t.header = bytearray(r.take(0x480))
    t.n = struct.unpack_from("<I", t.header, 0x0C)[0]
    flag = struct.unpack_from("<I", t.header, 0x10)[0]
    t.grid_scale, t.height_scale = struct.unpack_from("<ff", t.header, 0x14)
    t.extents = struct.unpack_from("<4i", t.header, 0x1C)
    t.heights = list(struct.unpack(f"<{t.n * t.n}h", r.take(t.n * t.n * 2)))
    assert r.unpack("<I")[0] == MAGIC
    t.map0 = r.take(0x20000) if flag else None
    t.map1 = r.take(0x40000)
    t.map2 = r.take(0x20000)
    assert r.unpack("<I")[0] == MAGIC
    qt_start = r.pos
    assert r.unpack("<II") == (QT, 1)
    t.qt_header = r.unpack("<HHHHHffI")
    size, _, _, patch, _, _, _, levels = t.qt_header
    assert r.unpack("<II") == (0x3E0, 8)
    assert r.unpack("<I")[0] == QT
    side = size // patch
    t.leaves = []
    for _ in range(side * side):
        meta = r.take(4 + 2 * 4 + 4 + 4)
        verts = [list(r.unpack("<6h")) for _ in range(81)]
        t.leaves.append((meta, verts))
    t.levels = []
    counts = [0] * (levels + 1)
    counts[levels] = side
    for level in range(levels - 1, -1, -1):
        assert r.unpack("<I")[0] == QT
        counts[level] = (counts[level + 1] + 1) // 2
        n = counts[level]
        t.levels.append((level, [list(r.unpack("<4h")) for _ in range(n * n)]))
    assert r.unpack("<I")[0] == QT
    t.rest = data[r.pos:]
    t.side = side
    return t


def write(t, path):
    out = bytearray(t.header)
    out += struct.pack(f"<{t.n * t.n}h", *t.heights)
    out += struct.pack("<I", MAGIC)
    if t.map0 is not None:
        out += t.map0
    out += t.map1 + t.map2
    out += struct.pack("<I", MAGIC)
    out += struct.pack("<II", QT, 1) + struct.pack("<HHHHHffI", *t.qt_header) + struct.pack("<III", 0x3E0, 8, QT)
    for meta, verts in t.leaves:
        out += meta
        for v in verts:
            out += struct.pack("<6h", *v)
    for _, nodes in t.levels:
        out += struct.pack("<I", QT)
        for node in nodes:
            out += struct.pack("<4h", *node)
    out += struct.pack("<I", QT)
    out += t.rest
    Path(path).write_bytes(bytes(out))


if __name__ == "__main__":
    t = read(sys.argv[1])
    print(f"N={t.n} grid scale {t.grid_scale} height scale {t.height_scale} extents {t.extents}")
    print(f"heights {min(t.heights)}..{max(t.heights)}; quadtree {t.qt_header}; {len(t.leaves)} leaves, {len(t.levels)} levels; rest {len(t.rest)} bytes")
    for i in (0, 1, len(t.leaves) // 2):
        meta, verts = t.leaves[i]
        print("leaf", i, meta.hex(), verts[:3], verts[80])
    for level, nodes in t.levels[:2] + t.levels[-1:]:
        print("level", level, len(nodes), nodes[:3])
