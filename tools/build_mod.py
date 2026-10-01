"""Builds a mod into a folder the runtime overlays on the game files (CW_MOD_ROOT).

Usage: python tools/build_mod.py <mod-name> [<mod-name> ...]

Mod layout (mods/<name>/):
  data/   files that replace or add data.zwp entries by file name (ODF, MSH, XBT, ...)
  files/  loose files that replace or add files on the game disc, by relative path (D:\\...)

Several mods are applied in the order given; later mods win. The result goes to
work/mod_root/<first-name>[+<other-name>...]/ with a rebuilt data.zwp and an empty Bins/
folder, where the game records fresh level caches on first load.

Run the game with:  CW_MOD_ROOT=<printed folder> clone_wars.exe
"""

import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "zwp"))
import zwp  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent


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
