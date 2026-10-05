"""Upscales the game's textures on the player's GPU for the renderer's texture pack (native_port/runtime/d3d/TexturePack).

Usage: python tools/upscale_textures.py <dump folder> <pack folder> [--upscaler <realesrgan-ncnn-vulkan.exe>] [--model <name>]

The dump folder is what the game collects with CW_TEXTURE_DUMP=<folder> (index.txt plus <hash>.bin, each texture's
top level as uploaded). Every texture not yet in the pack is upscaled 4x with Real-ESRGAN (its GPU build,
realesrgan-ncnn-vulkan): colors by the model, with the texture wrapped around its edges first so tiling textures stay
seamless; alpha by a smooth resize, so cut-out edges keep their shape. Results larger than 2048 texels on a side are
scaled to fit. The pack holds <hash>.png; the game loads them from <exe dir>\\upscaled (or CW_TEXTURE_PACK).

These are made from the game's own textures, so a pack is for the player's own machine only.
"""
import argparse
import io
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
D3DFMT_A8R8G8B8, D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5 = 21, 0x31545844, 0x33545844, 0x35545844
SCALE = 4
LIMIT = 2048


def decode(data, width, height, format_code):
    if format_code == D3DFMT_A8R8G8B8:
        return Image.frombuffer("RGBA", (width, height), data, "raw", "BGRA", 0, 1)
    four_cc = {D3DFMT_DXT1: b"DXT1", D3DFMT_DXT3: b"DXT3", D3DFMT_DXT5: b"DXT5"}[format_code]
    header = struct.pack("<4sIIIIIII44x", b"DDS ", 124, 0x81007, height, width, len(data), 0, 1)
    header += struct.pack("<II4s20x", 32, 0x4, four_cc) + struct.pack("<I16x", 0x1000)
    return Image.open(io.BytesIO(header + data)).convert("RGBA")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("dump")
    parser.add_argument("pack")
    parser.add_argument("--upscaler", default=str(ROOT / "work" / "tools" / "realesrgan" / "realesrgan-ncnn-vulkan.exe"))
    parser.add_argument("--model", default="realesrgan-x4plus")
    args = parser.parse_args()
    dump, pack = Path(args.dump), Path(args.pack)
    pack.mkdir(parents=True, exist_ok=True)

    entries = {}
    for line in (dump / "index.txt").read_text().splitlines():
        parts = line.split()
        if len(parts) == 4:
            entries[parts[0]] = (int(parts[1]), int(parts[2]), int(parts[3]))
    todo = [name for name in entries if not (pack / f"{name}.png").exists() and (dump / f"{name}.bin").exists()]
    print(f"{len(entries)} textures, {len(todo)} to upscale")
    if not todo:
        return

    with tempfile.TemporaryDirectory() as temporary:
        source, result = Path(temporary) / "in", Path(temporary) / "out"
        source.mkdir()
        result.mkdir()
        images, pads = {}, {}
        for name in todo:
            width, height, format_code = entries[name]
            try:
                image = decode((dump / f"{name}.bin").read_bytes(), width, height, format_code)
            except Exception as error:  # noqa: BLE001 - one unreadable texture should not stop the pack
                print(f"  skipping {name}: {error}")
                continue
            images[name] = image
            pad = max(4, min(width, height) // 8)
            pads[name] = pad
            rgb = np.asarray(image.convert("RGB"))
            Image.fromarray(np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode="wrap")).save(source / f"{name}.png")
        print(f"upscaling {len(images)} textures on the GPU ({args.model}) ...")
        subprocess.run([args.upscaler, "-i", str(source), "-o", str(result), "-n", args.model, "-s", str(SCALE), "-f", "png"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        written = 0
        for name, image in images.items():
            upscaled_path = result / f"{name}.png"
            if not upscaled_path.exists():
                print(f"  {name}: the upscaler produced nothing")
                continue
            width, height = image.size
            pad = pads[name] * SCALE
            color = Image.open(upscaled_path).convert("RGB")
            color = color.crop((pad, pad, pad + width * SCALE, pad + height * SCALE))
            alpha = image.getchannel("A").resize((width * SCALE, height * SCALE), Image.LANCZOS)
            final = Image.merge("RGBA", (*color.split(), alpha))
            if max(final.size) > LIMIT:
                ratio = LIMIT / max(final.size)
                final = final.resize((max(1, round(final.width * ratio)), max(1, round(final.height * ratio))), Image.LANCZOS)
            final.save(pack / f"{name}.png", optimize=False)
            written += 1
    print(f"wrote {written} textures to {pack}")


if __name__ == "__main__":
    main()
