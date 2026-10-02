"""Draws the lobby icons of the added vehicles as original artwork (no game models) and encodes them as .xbt.

Usage: python tools/assets/draw_icons.py

Writes 128x128 DXT1 icons with full mip chains (the format of the game's own lobby icons):
  mods/cis_spiders/data/cis_walk_assault_icon.xbt   spider walker: round body on four long legs, beam cannon
  mods/rep_vehicles/data/rep_walk_sixleg_icon.xbt   six-legged walker: armoured body, turret, six legs
  mods/rep_vehicles/data/rep_tank_gtrans_icon.xbt   hover transport: wedge hull with red markings
"""

import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = Path(__file__).resolve().parents[2]
S = 512  # drawn at 4x, scaled down for smooth edges


def shade(image, polygon, light, dark):
    """Fills a polygon with a vertical gradient from light (top) to dark (bottom)."""
    ys = [p[1] for p in polygon]
    top, bottom = min(ys), max(ys)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).polygon(polygon, fill=255)
    gradient = Image.new("RGB", (S, S))
    g = ImageDraw.Draw(gradient)
    for y in range(int(top), int(bottom) + 1):
        t = (y - top) / max(1, bottom - top)
        g.line([(0, y), (S, y)], fill=tuple(int(l + (d - l) * t) for l, d in zip(light, dark)))
    image.paste(gradient, (0, 0), mask)


def leg(image, points, width, light, dark):
    draw = ImageDraw.Draw(image)
    for (x1, y1), (x2, y2) in zip(points, points[1:]):
        draw.line([(x1, y1), (x2, y2)], fill=dark, width=width + 6)
        draw.line([(x1 - 2, y1 - 2), (x2 - 2, y2 - 2)], fill=light, width=width)
    for x, y in points[1:-1]:
        draw.ellipse([x - width, y - width, x + width, y + width], fill=light, outline=dark, width=3)


def spider():
    image = Image.new("RGB", (S, S), (0, 0, 0))
    metal_light, metal_dark = (196, 204, 214), (70, 76, 88)
    cx, cy = 256, 210
    for side in (-1, 1):
        for spread in (0.55, 1.0):
            knee = (cx + side * 150 * spread + side * 40, cy - 70)
            foot = (cx + side * 175 * spread + side * 30, 460)
            leg(image, [(cx + side * 40, cy + 20), knee, foot], 14, metal_light, metal_dark)
    draw = ImageDraw.Draw(image)
    # body: a shaded sphere
    for r in range(110, 0, -2):
        t = r / 110
        c = tuple(int(a * (1 - t) + b * t) for a, b in zip((230, 236, 244), (60, 66, 80)))
        draw.ellipse([cx - r - 20 * (1 - t), cy - r - 25 * (1 - t), cx + r - 20 * (1 - t), cy + r - 25 * (1 - t)], fill=c)
    # beam cannon underneath and a sensor eye
    shade(image, [(cx - 18, cy + 90), (cx + 18, cy + 90), (cx + 12, cy + 190), (cx - 12, cy + 190)], (180, 186, 196), (60, 64, 74))
    draw.ellipse([cx + 30, cy - 10, cx + 70, cy + 30], fill=(255, 80, 40), outline=(90, 20, 10), width=4)
    return image


def sixleg():
    image = Image.new("RGB", (S, S), (0, 0, 0))
    armour_light, armour_dark = (226, 222, 210), (96, 92, 84)
    # legs: three pairs under the body
    for i, x in enumerate((130, 256, 382)):
        for side in (-1, 1):
            knee = (x + side * 46, 330)
            foot = (x + side * 70, 455)
            leg(image, [(x + side * 12, 270), knee, foot], 13, armour_light, armour_dark)
    # body: front cab and rear section
    shade(image, [(70, 200), (300, 170), (300, 290), (90, 300)], armour_light, armour_dark)
    shade(image, [(300, 170), (450, 190), (440, 285), (300, 290)], (210, 206, 194), (84, 80, 72))
    draw = ImageDraw.Draw(image)
    draw.rectangle([90, 228, 290, 244], fill=(170, 40, 30))  # red band
    draw.polygon([(76, 212), (120, 206), (118, 238), (80, 240)], fill=(40, 70, 110))  # cockpit glass
    # turret and cannon on top
    shade(image, [(250, 120), (340, 120), (350, 170), (240, 172)], (236, 232, 222), (120, 116, 106))
    shade(image, [(340, 132), (470, 112), (472, 128), (342, 150)], (200, 196, 186), (90, 86, 78))
    return image


def transport():
    image = Image.new("RGB", (S, S), (0, 0, 0))
    hull_light, hull_dark = (236, 232, 226), (110, 104, 98)
    # wedge hull seen from the front-left, hovering
    shade(image, [(40, 300), (200, 170), (470, 200), (480, 300), (300, 360), (60, 340)], hull_light, hull_dark)
    shade(image, [(200, 170), (330, 130), (470, 200)], (246, 244, 240), (170, 166, 160))  # roof
    draw = ImageDraw.Draw(image)
    draw.polygon([(60, 312), (300, 330), (300, 346), (62, 330)], fill=(176, 36, 30))     # red stripe
    draw.polygon([(300, 330), (476, 280), (476, 296), (300, 346)], fill=(150, 30, 26))
    draw.polygon([(150, 238), (230, 196), (300, 204), (232, 246)], fill=(46, 80, 120))   # canopy
    # repulsor glow under the hull
    glow = Image.new("RGB", (S, S), (0, 0, 0))
    ImageDraw.Draw(glow).ellipse([120, 360, 420, 410], fill=(60, 140, 255))
    glow = glow.filter(ImageFilter.GaussianBlur(18))
    return add(image, glow)


def add(a, b):
    """Additive blend (light from the glow adds to the picture)."""
    return Image.fromarray(np.clip(np.asarray(a, dtype=np.int16) + np.asarray(b, dtype=np.int16), 0, 255).astype(np.uint8))


ICONS = {
    ROOT / "mods" / "cis_spiders" / "data" / "cis_walk_assault_icon.xbt": spider,
    ROOT / "mods" / "rep_vehicles" / "data" / "rep_walk_sixleg_icon.xbt": sixleg,
    ROOT / "mods" / "rep_vehicles" / "data" / "rep_tank_gtrans_icon.xbt": transport,
}


def main():
    with tempfile.TemporaryDirectory() as temp:
        for target, draw in ICONS.items():
            png = Path(temp) / (target.stem + ".png")
            draw().resize((128, 128), Image.LANCZOS).save(png)
            target.parent.mkdir(parents=True, exist_ok=True)
            subprocess.run([sys.executable, str(ROOT / "tools" / "assets" / "xbt.py"), "encode", str(png), str(target), "--format", "dxt1"], check=True)
            print(f"wrote {target}")


if __name__ == "__main__":
    main()
