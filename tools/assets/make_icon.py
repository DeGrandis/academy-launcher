"""Renders a lobby vehicle icon (128x128 on black, like cis_tank_assault_icon) from a glTF exported by msh.py.

Usage: python tools/assets/make_icon.py <in.gltf> <out.png> <yaw-degrees> <pitch-degrees>
Helper boxes (hp_* hardpoints, ground/root dummies) are left out. Encode the PNG for the game with
python tools/assets/xbt.py encode <out.png> <name>_icon.xbt --like <an existing *_icon.xbt>
"""
import json, subprocess, sys
from pathlib import Path
from PIL import Image

gltf_path, out_png, yaw, pitch = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3], sys.argv[4]
gltf = json.loads(gltf_path.read_text())
for node in gltf["nodes"]:
    name = node.get("name", "")
    if "mesh" in node and (name.startswith("hp_") or name.replace("_", "").lower() in ("grounddummy", "dummyroot")):
        del node["mesh"]
filtered = gltf_path.with_name(gltf_path.stem + "_icon.gltf")
filtered.write_text(json.dumps(gltf))
big = out_png.with_name(out_png.stem + "_512.png")
subprocess.check_call([sys.executable, "tools/assets/render_gltf.py", str(filtered), str(big), yaw, pitch])
image = Image.open(big).convert("RGB")
pixels = image.load()
for y in range(image.height):
    for x in range(image.width):
        if pixels[x, y] == (40, 40, 40):
            pixels[x, y] = (0, 0, 0)
image.resize((128, 128), Image.LANCZOS).save(out_png)
