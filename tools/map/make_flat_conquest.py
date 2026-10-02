"""Generates mods/flat_conquest: a flat square Conquest test map ("Flat Test", world multi19) with one outpost per team.

Usage: python tools/map/make_flat_conquest.py

It reads Geonosis Conquest (multi8) from extracted_iso/data.zwp and writes, into mods/flat_conquest/data/ (derived
from the game files, so not committed):
  multi19.xxw  multi8's terrain flattened to the height of its base plateau (textures and blend maps kept)
  multi19.wld  two bases, three spawn points each, one outpost per team with its four turrets, two powerups
  multi19.pth  Fac<outpost>_<team> routes from each outpost to each base (troops and AI players follow them)
  multi19.aim  the AI grid, all open
mods/flat_conquest/edits.json (committed) copies multi8's camera and radar files, adds the mission to the Conquest
list and names it.

Map files, as the game uses them (heights above the ground measured on multi8):
  Item("Base")        Team 1/2, +81.7     Item("Spawnpoint")  Team, +2
  Item("Zone")        Type(2) = outpost, Radius, Id, Base1Path/Base2Path = "Fac<Id>_1"/"Fac<Id>_2", +25
  Item("Turret")      Owner = the outpost's Id, +24.5 (four per outpost)
  Item("Powerup")     Type, Respawntime, +5
  .pth  "PATH", u32 6, u32 count; per path: char[0x30] name, u32 points, points of 11 floats (x, y, z, then
        0, 0, 1, 0, 0, 0, 0, 1 for ground routes)
"""

import json
import math
import re
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "zwp"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import xxw  # noqa: E402
import zwp  # noqa: E402

SOURCE = "multi8"
WORLD = "multi19"
TITLE = "Flat Test"
DESCRIPTION = "Flat test ground: one outpost on each side, near each team's base."
MOD = ROOT / "mods" / "flat_conquest"

GROUND_STEP = -259           # height value of multi8's base plateau
BASE_DISTANCE = 650.0        # each base this far from the centre (team 1 at +z, team 2 at -z)
OUTPOST_DISTANCE = 300.0     # each team's outpost this far from the centre, on its own side
UP_NORMAL = (-2048, 31)      # packed normal 0x001FF800 = straight up, as two int16


def read_source():
    data = (ROOT / "extracted_iso" / "data.zwp").read_bytes()
    files = {}
    for entry in zwp.read_directory(data):
        stem, _, extension = entry.name.lower().rpartition(".")
        if stem == SOURCE:
            files[extension] = zwp.read_entry(data, entry)
    return files


def flatten(source_path, target_path):
    t = xxw.read(source_path)
    t.heights = [GROUND_STEP] * len(t.heights)
    # Baked vertex colour: the most common one of the original's flat (up-facing) vertices.
    colours = {}
    for _, verts in t.leaves:
        for v in verts:
            if (v[2], v[3]) == UP_NORMAL:
                colours[(v[4], v[5])] = colours.get((v[4], v[5]), 0) + 1
    colour = max(colours, key=colours.get)
    leaves = []
    for meta, verts in t.leaves:
        meta = bytearray(meta)
        struct.pack_into("<hh", meta, 4, GROUND_STEP, GROUND_STEP)  # leaf min / max height
        leaves.append((bytes(meta), [[GROUND_STEP, 0, UP_NORMAL[0], UP_NORMAL[1], colour[0], colour[1]] for _ in verts]))
    t.leaves = leaves
    t.levels = [(level, [[node[0], node[1], GROUND_STEP, GROUND_STEP] for node in nodes]) for level, nodes in t.levels]
    xxw.write(t, target_path)
    return GROUND_STEP * t.height_scale


def item(kind, fields):
    body = "".join(f"\t{line}\r\n" for line in fields)
    return f'Item("{kind}")\r\n{{\r\n{body}}}\r\n\r\n'


def vec(x, y, z):
    return f"{x:.6f}, {y:.6f}, {z:.6f}"


def items_of(world, kind):
    return [m.group(1) for m in re.finditer(r'Item\("' + kind + r'"\)\s*\{(.*?)\}', world, re.S)]


def field(block, name):
    return [float(v) for v in re.search(name + r"\(([^)]*)\)", block).group(1).split(",")]


def make_world(source_world, ground):
    header = source_world[:source_world.index("Item(")]
    header = header.replace(f'TerrainName("{SOURCE}.ter")', f'TerrainName("{WORLD}.ter")')
    assert f'TerrainName("{WORLD}.ter")' in header and f'SkyName("{SOURCE}.sky")' in header

    bases = {int(field(b, "Team")[0]): b for b in items_of(source_world, "Base")}
    spawns = items_of(source_world, "Spawnpoint")
    zones = {int(field(z, "Id")[0]): z for z in items_of(source_world, "Zone")}
    turrets = items_of(source_world, "Turret")
    out = [header]

    new_base = {1: (0.0, BASE_DISTANCE), 2: (0.0, -BASE_DISTANCE)}
    for team in (1, 2):
        old = field(bases[team], "Position")
        x, z = new_base[team]
        out.append(item("Base", ["Type(0);", f"Position({vec(x, ground + 81.7, z)});",
                                 f"Rotation({vec(*field(bases[team], 'Rotation')[:3])}, {field(bases[team], 'Rotation')[3]:.6f});",
                                 f"Team({team:.6f});"]))
        # Spawn points keep their place relative to their base.
        for s in spawns:
            if int(field(s, "Team")[0]) != team:
                continue
            p = field(s, "Position")
            r = field(s, "Rotation")
            out.append(item("Spawnpoint", ["Type(0);", f"Position({vec(x + p[0] - old[0], ground + 2.0, z + p[2] - old[2])});",
                                           f"Rotation({vec(*r[:3])}, {r[3]:.6f});", f"Team({team:.6f});"]))

    # Outposts: Id 0 next to team 1 (copied from multi8's outpost 0, which sits by team 1's base), Id 1 next to
    # team 2 (from multi8's outpost 3, by team 2's base); their turrets keep their places around them.
    outposts = {0: (zones[0], 0, (0.0, OUTPOST_DISTANCE)), 1: (zones[3], 3, (0.0, -OUTPOST_DISTANCE))}
    paths = []
    for new_id, (zone, old_id, (x, z)) in outposts.items():
        old = field(zone, "Position")
        r = field(zone, "Rotation")
        out.append(item("Zone", ["Type(2);", f"Position({vec(x, ground + 25.0, z)});", f"Rotation({vec(*r[:3])}, {r[3]:.6f});",
                                 f"Radius({field(zone, 'Radius')[0]:.6f});", f"Id({new_id:.6f});",
                                 f'Base1Path("Fac{new_id}_1");', f'Base2Path("Fac{new_id}_2");']))
        for t in turrets:
            if int(field(t, "Owner")[0]) != old_id:
                continue
            p = field(t, "Position")
            tr = field(t, "Rotation")
            out.append(item("Turret", ["Type(0);", f"Position({vec(x + p[0] - old[0], ground + 24.5, z + p[2] - old[2])});",
                                       f"Rotation({vec(*tr[:3])}, {tr[3]:.6f});", f"Owner({new_id:.6f});"]))
        for team in (1, 2):
            paths.append((f"Fac{new_id}_{team}", route((x, z), new_base[team], ground)))

    for x, kind in ((-120.0, 1), (120.0, 6)):
        out.append(item("Powerup", [f"Type({kind});", f"Position({vec(x, ground + 5.0, 0.0)});",
                                    "Rotation(1.000000, 0.000000, 0.000000, 0.000000);", "Respawntime(60.000000);"]))
    out.append("\r\nNextSequence(1);\r\n")
    return "".join(out), paths


def route(outpost, base, ground):
    """Straight route from the edge of an outpost to the front of a base, a point every ~60 units."""
    dx, dz = base[0] - outpost[0], base[1] - outpost[1]
    length = math.hypot(dx, dz)
    ux, uz = dx / length, dz / length
    start = (outpost[0] + ux * 100.0, outpost[1] + uz * 100.0)
    end = (base[0] - ux * 120.0, base[1] - uz * 120.0)
    span = math.hypot(end[0] - start[0], end[1] - start[1])
    steps = max(1, round(span / 60.0))
    return [(start[0] + (end[0] - start[0]) * i / steps, ground, start[1] + (end[1] - start[1]) * i / steps) for i in range(steps + 1)]


def make_paths(source_pth, paths):
    # Keep the source's camera flythrough (the last path) so the intro works.
    count = struct.unpack_from("<I", source_pth, 8)[0]
    pos = 12
    flythrough = None
    for _ in range(count):
        name = source_pth[pos:pos + 0x30].split(b"\0")[0].decode()
        points = struct.unpack_from("<I", source_pth, pos + 0x30)[0]
        end = pos + 0x34 + points * 44
        if name == "flythrough":
            flythrough = source_pth[pos:end]
        pos = end
    out = bytearray(b"PATH" + struct.pack("<II", 6, len(paths) + (flythrough is not None)))
    for name, points in paths:
        out += name.encode().ljust(0x30, b"\0") + struct.pack("<I", len(points))
        for x, y, z in points:
            out += struct.pack("<11f", x, y, z, 0, 0, 1, 0, 0, 0, 0, 1)
    if flythrough is not None:
        out += flythrough
    return bytes(out)


def localize_edits():
    def block(text, lines):
        data = lines.to_bytes(4, "little") + text.encode("utf-16-le")
        data = bytes(((b & 0xF) << 4) | (b >> 4) for b in data)
        hexed = data.hex().upper()
        values = "".join(f'\r\n        Value("{hexed[i:i + 64]}");' for i in range(0, len(hexed), 64))
        return f'\r\n      VarBinary("Multi19")\r\n      {{\r\n        Size({len(data)});{values}\r\n      }}'

    edits = []
    # The title and description tables each have a Multi8 entry; add Multi19 after each.
    for first_value, text, lines in (("1000000074005600F600E600F600370096003700", TITLE, 1),
                                     ("200000002400160047004700C60056000200F60007000700F60037009600E600", DESCRIPTION, 2)):
        find = r'(VarBinary\("Multi8"\)\r?\n\s*\{\r?\n\s*Size\(\d+\);\r?\n\s*Value\("' + first_value + r'"\);(?:\r?\n\s*Value\("[0-9A-F]+"\);)*\r?\n\s*\})'
        edits.append({"file": "localize.cfg", "find": find, "replace": "\\1" + block(text, lines), "count": 1})
    return edits


def main():
    files = read_source()
    data = MOD / "data"
    data.mkdir(parents=True, exist_ok=True)
    work = ROOT / "work" / "map_source"
    work.mkdir(parents=True, exist_ok=True)
    (work / f"{SOURCE}.xxw").write_bytes(files["xxw"])
    ground = flatten(work / f"{SOURCE}.xxw", data / f"{WORLD}.xxw")
    world, paths = make_world(files["wld"].decode("latin-1"), ground)
    (data / f"{WORLD}.wld").write_bytes(world.encode("latin-1"))
    (data / f"{WORLD}.pth").write_bytes(make_paths(files["pth"], paths))
    (data / f"{WORLD}.aim").write_bytes(bytes(len(files["aim"])))

    mission = (r'(\s*Mission\(\)\s*\{[^{}]*?Title\(")Geonosis("\);\s*Descrition\("CONQUEST"\);[^{}]*?)MissionFile\("multi8\.wld"\);([^{}]*\})',
               '\\1Geonosis\\2MissionFile("multi8.wld");\\3\r\n\\1' + TITLE + '\\2MissionFile("' + WORLD + '.wld");\\3')
    edits = [
        {"file": f"{WORLD}.cin", "from": f"{SOURCE}.cin"},
        {"file": f"{WORLD}.xbt", "from": f"{SOURCE}.xbt"},
        {"file": "multiplayerbuttons.cfg", "find": mission[0], "replace": mission[1], "count": 1},
    ] + localize_edits()
    (MOD / "edits.json").write_text("[\n" + ",\n".join("  " + json.dumps(e) for e in edits) + "\n]\n", encoding="utf-8")
    print(f"ground at y={ground:.3f}; wrote {data} and {MOD / 'edits.json'}")
    for name, points in paths:
        print(f"  {name}: {len(points)} points")


if __name__ == "__main__":
    main()
