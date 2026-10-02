"""Draws launcher/academy.ico (a simple emblem; no game artwork). Run: python launcher/make_icon.py"""
from pathlib import Path

from PIL import Image, ImageDraw


def emblem(size):
    scale = 4
    s = size * scale
    image = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.ellipse((s * 0.03, s * 0.03, s * 0.97, s * 0.97), fill=(18, 32, 58, 255), outline=(90, 170, 255, 255), width=max(1, s // 22))
    # A stylised "A": two legs and a crossbar, like a starfighter seen head-on
    w = max(1, s // 10)
    top = (s * 0.5, s * 0.2)
    draw.line([(s * 0.26, s * 0.78), top, (s * 0.74, s * 0.78)], fill=(235, 240, 250, 255), width=w, joint="curve")
    draw.line([(s * 0.36, s * 0.58), (s * 0.64, s * 0.58)], fill=(255, 200, 60, 255), width=w)
    return image.resize((size, size), Image.LANCZOS)


out = Path(__file__).resolve().parent / "academy.ico"
sizes = [16, 24, 32, 48, 64, 128, 256]
emblem(256).save(out, sizes=[(n, n) for n in sizes], append_images=[emblem(n) for n in sizes])
print(f"wrote {out}")
