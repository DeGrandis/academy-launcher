"""Generates mods/night_maps/edits.json: a night copy of each Conquest map and the Academy (night<N>.wld for
multi<N>.wld) listed in the multiplayer map selector, with "<Map> Night" titles. The runtime renders night<N> missions
as night under RTX Remix (Device.cpp remixApplyAtmosphere).

Two constraints of the game:
- the selector drops entries whose mission id is already listed, and the id (a hash of the mission file name) only
  covers the first 7 characters, so "multi12n" would collide with "multi12": hence night<N>;
- the string sections of localize.cfg are sorted by name (case-insensitive) and searched by bisection, so the new
  strings go in their sorted places (appended after Multi8, they broke every lookup: no menu text, no HUD).

Usage: python tools/make_night_maps.py
"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "zwp"))
import zwp  # noqa: E402

MAPS = {"multi5": "Thule Night", "multi8": "Geonosis Night", "multi12": "Kashyyyk Night", "multi10": "Thule Night",
        "multi6": "Rhen Var Night"}
BLOCK = r'([ \t]*)VarBinary\("([^"]+)"\)\r?\n\s*\{\r?\n\s*Size\(\d+\);((?:\r?\n\s*Value\("[0-9A-F]+"\);)+)\r?\n\s*\}'


def night(mission):
    """multi12 -> night12, Multi8 -> Night8."""
    return ("Night" if mission[0] == "M" else "night") + mission[5:]


def swap(byte):
    return ((byte >> 4) | ((byte & 15) << 4)) & 0xFF


def encode(prefix, text):
    raw = bytes([prefix, 0, 0, 0]) + text.encode("utf-16-le")
    return "".join("%02X" % swap(byte) for byte in raw), len(raw)


def decode(hexes):
    raw = bytes(swap(byte) for byte in bytes.fromhex(hexes))
    return raw[0], raw[4:].decode("utf-16-le")


def block(name, prefix, text, indent):
    hexes, size = encode(prefix, text)
    values = "".join(f'\r\n{indent}  Value("{hexes[i:i + 64]}");' for i in range(0, len(hexes), 64))
    return f'{indent}VarBinary("{name}")\r\n{indent}{{\r\n{indent}  Size({size});{values}\r\n{indent}}}'


def sorted_position(text, found, name):
    """(anchor block, insert before it?) for a new block called name in the section of block found."""
    previous = found
    pattern = re.compile(BLOCK)
    position = found.end()
    while True:
        candidate = pattern.search(text, position)
        if candidate is None or "}" in text[previous.end():candidate.start()] or candidate.group(1) != found.group(1):
            return previous, False  # the section ended
        if candidate.group(2).lower() > name.lower():
            return candidate, True
        previous = candidate
        position = candidate.end()


def main():
    data = (ROOT / "extracted_iso" / "data.zwp").read_bytes()
    entries = {entry.name.lower(): entry for entry in zwp.read_directory(data)}
    localize = zwp.read_entry(data, entries["localize.cfg"]).decode("latin-1")
    # The loading screen is "<world name without digits>_loadscreen.xbt": night<N> asks for night_loadscreen.xbt
    # (missing, the load fails in a loop of exceptions). The Thule Moon one suits all of them.
    # Some maps derive "loadingscreen_loadscreen.xbt" instead (Thule Moon Conquest); both point at the Thule screen.
    edits = [{"file": "night_loadscreen.xbt", "from": "thule_loadscreen.xbt"},
             {"file": "loadingscreen_loadscreen.xbt", "from": "thule_loadscreen.xbt"}]
    insertions = {}  # anchor block text -> ([blocks before it], [blocks after it])
    for mission, title in MAPS.items():
        for extension in ("wld", "pth", "aim", "cin", "xbt", "xxw"):  # all of a world's own files
            if f"{mission}.{extension}" in entries:
                edits.append({"file": f"{night(mission)}.{extension}", "from": f"{mission}.{extension}"})
        edits.append({"file": "multiplayerbuttons.cfg",
                      "find": r'(\s*Mission\(\)\s*\{[^{}]*?)MissionFile\("%s\.wld"\);([^{}]*\})' % mission,
                      "replace": r'\1MissionFile("%s.wld");\2' % mission + "\r\n" + r'\1MissionFile("%s.wld");\2' % night(mission),
                      "count": 1})
        blocks = [found for found in re.finditer(BLOCK, localize) if found.group(2).lower() == mission]
        assert len(blocks) == 2, (mission, len(blocks))  # title, then description
        for index, found in enumerate(blocks):
            prefix, text = decode("".join(re.findall(r'Value\("([0-9A-F]+)"\)', found.group(3))))
            name = night(found.group(2))
            if index == 0:
                added = block(name, len(title.split()), title, found.group(1))
            else:
                added = block(name, prefix, text + " under the night sky", found.group(1))
            anchor, before = sorted_position(localize, found, name)
            insertions.setdefault(anchor.group(0), ([], []))[0 if before else 1].append((name.lower(), added))
    for anchor, (before, after) in insertions.items():
        replace = "".join(part + "\r\n" for _, part in sorted(before)) + r"\g<0>" + "".join("\r\n" + part for _, part in sorted(after))
        edits.append({"file": "localize.cfg", "find": re.escape(anchor), "replace": replace, "count": 1})
    out = ROOT / "mods" / "night_maps" / "edits.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(edits, indent=1))
    print(f"{len(edits)} edits -> {out}")


if __name__ == "__main__":
    main()
