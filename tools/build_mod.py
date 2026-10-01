"""Builds a mod into a folder the runtime overlays on the game files (CW_MOD_ROOT).

Usage: python tools/build_mod.py <mod-name> [<mod-name> ...]

Mod layout (mods/<name>/):
  data/       files that replace or add data.zwp entries by file name (ODF, MSH, XBT, ...)
  edits.json  text edits applied to data.zwp entries, so a mod need not ship copies of game files:
              [{"file": "bonus.cfg", "find": "<regex>", "replace": "<text>", "count": <expected matches, optional>}]
              Edits apply to the file as earlier mods (and this mod's data/) left it. Add "from": "<entry>" to
              create a new entry as a copy of another one (find/replace are then optional).
  files/  loose files that replace or add files on the game disc, by relative path (D:\\...)

Several mods are applied in the order given; later mods win. The result goes to
work/mod_root/<first-name>[+<other-name>...]/ with a rebuilt data.zwp and an empty Bins/
folder, where the game records fresh level caches on first load.

Run the game with:  CW_MOD_ROOT=<printed folder> clone_wars.exe
"""

import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "zwp"))
import zwp  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


def apply_edits(edits, staging, archive, mod_name):
    data = None
    entries = None
    def original(name):
        nonlocal data, entries
        if (staging / name).exists():
            return (staging / name).read_bytes(), name
        if data is None:
            data = archive.read_bytes()
            entries = {entry.name.lower(): entry for entry in zwp.read_directory(data)}
        entry = entries.get(name.lower())
        if entry is None:
            sys.exit(f"{mod_name}: edits.json names {name}, which is not in data.zwp")
        return zwp.read_entry(data, entry), entry.name

    for edit in edits:
        if "from" in edit:
            content, _ = original(edit["from"])
            target = staging / edit["file"]
        else:
            content, name = original(edit["file"])
            target = staging / name
        if "find" not in edit:
            target.write_bytes(content)
            continue
        text = content.decode("latin-1")
        updated, matches = re.subn(edit["find"], edit["replace"], text, flags=re.MULTILINE)
        expected = edit.get("count")
        if matches == 0 or (expected is not None and matches != expected):
            sys.exit(f"{mod_name}: edit of {edit['file']} matched {matches} times (expected {expected or 'at least 1'}): {edit['find']}")
        target.write_bytes(updated.encode("latin-1"))


def main(names):
    if not names:
        sys.exit(__doc__)
    game = ROOT / "extracted_iso"
    out = ROOT / "work" / "mod_root" / "+".join(names)
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    with tempfile.TemporaryDirectory() as staging:
        staging = Path(staging)
        for name in names:
            mod = ROOT / "mods" / name
            if not mod.is_dir():
                sys.exit(f"no such mod: {mod}")
            data = mod / "data"
            if data.is_dir():
                for file in data.rglob("*"):
                    if file.is_file():
                        shutil.copy2(file, staging / file.name)
            edits = mod / "edits.json"
            if edits.is_file():
                apply_edits(json.loads(edits.read_text(encoding="utf-8")), staging, game / "data.zwp", name)
            if (mod / "plugin").is_dir():
                dll = ROOT / "native_port" / "build-x86" / "bin" / "mods" / f"{name}.dll"
                if not dll.exists():
                    sys.exit(f"{dll} not found: build the port first (cmake --build native_port/build-x86)")
                (out / "plugins").mkdir(exist_ok=True)
                shutil.copy2(dll, out / "plugins" / dll.name)
            files = mod / "files"
            if files.is_dir():
                shutil.copytree(files, out, dirs_exist_ok=True)
        if any(staging.iterdir()):
            zwp.build(game / "data.zwp", staging, out / "data.zwp")
    (out / "Bins").mkdir(exist_ok=True)
    print(f"mod root: {out}")


if __name__ == "__main__":
    main(sys.argv[1:])
