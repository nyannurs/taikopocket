"""Placeholder skin: original, deliberately plain art and synthesised hit sounds,
drawn in code so the repository ships no third-party assets. It produces images
under the same names and layouts as a Taikojiro skin folder (see skin.py)."""

from __future__ import annotations

import math
import struct

from PIL import Image, ImageDraw, ImageFont

RED, BLUE, YELLOW = (232, 64, 40), (72, 168, 216), (248, 184, 0)
WHITE, DARK = (240, 240, 240), (32, 32, 36)


def _note(d, cx, cy, r, color, ring=WHITE):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=ring, outline=DARK)
    d.ellipse([cx - r + 3, cy - r + 3, cx + r - 3, cy + r - 3], fill=color)


def _notes():
    im = Image.new("RGBA", (48 * 15, 48), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = lambda i: i * 48 + 24
    d.ellipse([c(0) - 14, 10, c(0) + 14, 38], outline=(160, 160, 160), width=2)
    _note(d, c(1), 24, 14, RED)
    _note(d, c(2), 24, 14, BLUE)
    _note(d, c(3), 24, 21, RED)
    _note(d, c(4), 24, 21, BLUE)
    for base, r in ((5, 14), (8, 21)):
        _note(d, c(base), 24, r, YELLOW)
        d.rectangle([c(base + 1) - 24, 24 - r, c(base + 1) + 23, 24 + r], fill=YELLOW)
        d.pieslice([c(base + 2) - 24 - r, 24 - r, c(base + 2) - 24 + r, 24 + r], -90, 90, fill=YELLOW)
    for base, color in ((11, RED), (13, YELLOW)):
        _note(d, c(base), 24, 14, color)
        d.ellipse([c(base + 1) - 22, 6, c(base + 1) + 14, 42], fill=color, outline=DARK)
    return im


def _double(im):
    """Twice the size, pixel for pixel. (Image.resize fails on some Pillow builds for these tiny images.)"""
    w, h = im.size
    out = Image.new(im.mode, (w * 2, h * 2))
    src, dst = im.load(), out.load()
    for y in range(h * 2):
        for x in range(w * 2):
            dst[x, y] = src[x // 2, y // 2]
    return out


def _font_m():
    """12 x 7 cells of 12x18; only the digits (row 2, columns 2..11) and '+' are used."""
    im = Image.new("RGBA", (144, 126), (0, 0, 0, 0))
    font = ImageFont.load_default()
    for i, ch in enumerate("0123456789"):
        glyph = Image.new("L", (6, 9), 0)
        ImageDraw.Draw(glyph).text((0, -2), ch, fill=255, font=font)
        glyph = _double(glyph)
        im.paste(WHITE, ((2 + i) * 12, 36), glyph)
    plus = Image.new("L", (6, 9), 0)
    ImageDraw.Draw(plus).text((0, -2), "+", fill=255, font=font)
    im.paste(WHITE, (5 * 12, 90), _double(plus))
    return im


def _label(text, color, size=(32, 16)):
    im = Image.new("RGBA", size, (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    font = ImageFont.load_default()
    w = d.textlength(text, font=font)
    d.text(((size[0] - w) // 2, 2), text, fill=color, font=font)
    return im


def _stack(frames):
    im = Image.new("RGBA", (frames[0].size[0], sum(f.size[1] for f in frames)), (0, 0, 0, 0))
    y = 0
    for f in frames:
        im.paste(f, (0, y))
        y += f.size[1]
    return im


def _gauge():
    """6 units x 4 rows of 16x16: empty, filled, filled past the clear line, full."""
    im = Image.new("RGBA", (96, 64), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rows = [((40, 40, 60), (60, 40, 40)), ((40, 160, 240), (60, 40, 40)),
            ((40, 160, 240), (248, 200, 40)), ((248, 120, 40), (248, 220, 80))]
    for r, (low, high) in enumerate(rows):
        for u in range(6):
            color = low if u < 3 else high
            d.rectangle([u * 16, r * 16 + 3, u * 16 + 15, r * 16 + 12], fill=color, outline=DARK)
    return im


def _background(top, bottom, label=None):
    im = Image.new("RGBA", (512, 384))
    d = ImageDraw.Draw(im)
    for y in range(384):
        t = y / 383
        d.line([(0, y), (511, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    if label:
        d.text((380, 350), label, fill=(120, 120, 120), font=ImageFont.load_default())
    return im


def _layer(color, text):
    im = Image.new("RGBA", (512, 48), color + (120,))
    ImageDraw.Draw(im).text((440, 18), text, fill=WHITE, font=ImageFont.load_default())
    return im


def images() -> dict[str, Image.Image]:
    out = {"notes": _notes(), "font_m": _font_m(), "normagauge": _gauge()}

    drum = Image.new("RGBA", (48, 48), (0, 0, 0, 0))
    d = ImageDraw.Draw(drum)
    d.ellipse([2, 2, 45, 45], fill=(70, 70, 76), outline=WHITE, width=2)
    d.ellipse([10, 10, 37, 37], fill=(200, 190, 170))
    out["mtaiko"] = drum
    for name, color, box in (("mtaikoflash_red", RED, [10, 10, 37, 37]), ("mtaikoflash_blue", BLUE, [2, 2, 45, 45])):
        im = Image.new("RGBA", (48, 48), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        if name.endswith("red"):
            d.ellipse(box, fill=color)
            d.line([(24, 8), (24, 39)], fill=(0, 0, 0, 0), width=2)
        else:
            d.ellipse(box, outline=color, width=4)
            d.line([(24, 0), (24, 47)], fill=(0, 0, 0, 0), width=2)
        out[name] = im

    out["judgement"] = _stack([_label("GOOD", (255, 220, 60)), _label("OK", WHITE), _label("BAD", (120, 140, 255))])

    soul = Image.new("RGBA", (160, 32), (0, 0, 0, 0))
    d = ImageDraw.Draw(soul)
    for i in range(5):
        glow = (255, 160 + 20 * (i % 2), 40) if i else (110, 110, 120)
        d.ellipse([i * 32 + 3, 3, i * 32 + 28, 28], fill=glow, outline=WHITE)
    out["soul"] = soul

    ex = Image.new("RGBA", (96, 96), (0, 0, 0, 0))
    ImageDraw.Draw(ex).ellipse([14, 14, 81, 81], outline=(255, 255, 255, 200), width=6)
    out["explosion_upper"] = ex

    out["bg"] = _background((16, 18, 28), (36, 40, 60))
    out["songselectbg"] = _background((20, 24, 40), (8, 8, 12), "Song Select")
    out["resultbg_clear"] = _background((40, 36, 16), (12, 12, 12), "Result")
    out["resultbg_failure"] = _background((20, 20, 40), (8, 8, 12), "Result")
    out["normallayer"] = _layer((60, 60, 60), "NORMAL")
    out["expertlayer"] = _layer((40, 90, 140), "EXPERT")
    out["masterlayer"] = _layer((120, 50, 140), "MASTER")
    return out


def sound(name: str):
    """Synthesised drum sounds: a decaying low thump (don) and a short click (ka)."""
    rate = 48000
    if name == "dong":
        n = int(rate * 0.22)
        data = [int(26000 * math.exp(-i / (rate * 0.045)) * math.sin(2 * math.pi * (70 + 90 * math.exp(-i / 900.0)) * i / rate))
                for i in range(n)]
    elif name == "ka":
        n = int(rate * 0.10)
        seed = 12345
        data = []
        for i in range(n):
            seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
            noise = (seed >> 15) / 32768.0 - 1.0
            tone = math.sin(2 * math.pi * 1900 * i / rate)
            data.append(int(20000 * math.exp(-i / (rate * 0.018)) * (0.6 * tone + 0.4 * noise)))
    else:
        return None
    return rate, struct.pack(f"<{len(data)}h", *data)
