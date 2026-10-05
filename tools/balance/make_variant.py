"""Writes a balance variant: a throwaway mod mods/bal_<name>/edits.json that changes ODF values, for
tools/balance/run_conquest.ps1 -Mods <full preset> bal_<name>. Variants are not committed.

Usage: python tools/balance/make_variant.py <name> <odf>.<key>=<value> [...]
       python tools/balance/make_variant.py <name> --edits '<json list of edits.json entries>'

<odf>.<key>=<value> replaces the first `key = ...` line of data/<odf>.odf (as the game sees it after the other mods),
e.g. drone_red.maxHull=1100 or drone_blue_mortar_xpl.damageCenter="500.0 350.0".
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def main(argv):
    name = argv[1]
    edits = []
    if len(argv) > 3 and argv[2] == "--edits":
        edits = json.loads(argv[3])
    else:
        for setting in argv[2:]:
            target, value = setting.split("=", 1)
            odf, key = target.rsplit(".", 1)
            edits.append({
                "file": f"{odf}.odf",
                "find": rf"(?mi)^(\s*{re.escape(key)}\s*=\s*)[^\r\n/]*?([ \t]*(?://[^\r\n]*)?\r?)$",
                "replace": rf"\g<1>{value}\g<2>",
                "count": 1,
            })
    folder = ROOT / "mods" / f"bal_{name}"
    folder.mkdir(parents=True, exist_ok=True)
    (folder / "edits.json").write_text(json.dumps(edits, indent=2) + "\n")
    print(f"wrote {folder / 'edits.json'} ({len(edits)} edits)")


if __name__ == "__main__":
    main(sys.argv)
