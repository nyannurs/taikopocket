#!/usr/bin/env python3
"""Draws the core icon (36x36) and the platform image (521x165) the Pocket's
menus show, in Analogue's .bin format: one brightness byte and a zero byte per
pixel, the picture rotated 90 degrees counter-clockwise. Original art."""

import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "gateware", "pkg", "pocket")


def write_bin(im: Image.Image, path: str) -> None:
    rotated = im.convert("L").rotate(90, expand=True)
    data = bytearray()
    for v in rotated.tobytes():
        data += bytes((v, 0))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def drum(d: ImageDraw.ImageDraw, x0: int, y0: int, size: int) -> None:
    """Ring with a dot, in a size x size box. Whole-pixel boxes with equal insets on
    every side, so the dot is exactly in the middle of the ring."""
    x1, y1 = x0 + size - 1, y0 + size - 1
    inset = round(size * 0.225)
    d.ellipse([x0, y0, x1, y1], outline=255, width=max(2, size // 12))
    d.ellipse([x0 + inset, y0 + inset, x1 - inset, y1 - inset], fill=200)


def symmetric(im: Image.Image, box) -> bool:
    part = im.crop(box)
    return (part.tobytes() == part.transpose(Image.FLIP_LEFT_RIGHT).tobytes()
            and part.tobytes() == part.transpose(Image.FLIP_TOP_BOTTOM).tobytes())


def main() -> None:
    icon = Image.new("L", (36, 36), 0)
    drum(ImageDraw.Draw(icon), 2, 2, 32)
    assert symmetric(icon, (0, 0, 36, 36))
    write_bin(icon, os.path.join(ROOT, "Cores", "nyannurs.TaikoPocket", "icon.bin"))

    banner = Image.new("L", (521, 165), 0)
    d = ImageDraw.Draw(banner)
    drum(d, 38, 30, 105)
    assert symmetric(banner, (38, 30, 143, 135))
    font = ImageFont.load_default(54)
    d.text((170, 28), "TAIKO", fill=255, font=font)
    d.text((170, 84), "POCKET", fill=200, font=font)
    write_bin(banner, os.path.join(ROOT, "Platforms", "_images", "taikopocket.bin"))


if __name__ == "__main__":
    main()
