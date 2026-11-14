"""Glyph atlas: every character the library needs, rasterised at the UI's sizes.

Section layout (little endian):
  u32 font_count
  per font  {u16 px, u16 line_height, i16 ascent, i16 descent,
             u32 glyph_count, u32 glyphs_offset, u32 bitmaps_offset, u32 0}
  glyph     {u32 codepoint, u32 bitmap_offset, u8 w, u8 h, i8 xoff, i8 yoff, u8 advance, u8[3] 0}
            sorted by codepoint; yoff is measured down from the top of the line
  bitmap    4 bits per pixel coverage, rows padded to whole bytes, high nibble first
"""

from __future__ import annotations

import os
import struct

from PIL import ImageFont

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
UI_EXTRA = "★☆♪←→↑↓○×…・ー"
DEFAULT_FONT = os.path.join(os.path.dirname(__file__), "..", "..", "..", "third_party", "fonts",
                            "NotoSansJP-Regular.otf")


def load_font(path: str | None, px: int):
    if path == "builtin":
        return ImageFont.load_default(px)
    path = path or DEFAULT_FONT
    if not os.path.exists(path):
        raise SystemExit(f"font not found: {path}\nrun tools/fetch_third_party.sh or pass --font")
    return ImageFont.truetype(path, px)


def _raster(font, ch):
    mask = font.getmask(ch, mode="L")
    w, h = mask.size
    bbox = font.getbbox(ch) or (0, 0, 0, 0)
    return w, h, bbox[0], bbox[1], bytes(mask), int(round(font.getlength(ch)))


def build_section(font_path: str | None, sizes: list[int], chars: set[str]) -> tuple[bytes, dict]:
    wanted = sorted(set(ASCII) | set(UI_EXTRA) | {c for c in chars if c >= " " and c not in "\x7f﻿"})
    head = struct.pack("<I", len(sizes))
    bodies = []
    missing: set[str] = set()
    offset = 4 + 24 * len(sizes)
    for px in sizes:
        font = load_font(font_path, px)
        ascent, descent = font.getmetrics()
        notdef = _raster(font, "\U0010fffe")[:5]
        glyphs = bytearray()
        bitmaps = bytearray()
        count = 0
        for ch in wanted:
            w, h, xoff, yoff, data, adv = _raster(font, ch)
            if (w, h, xoff, yoff, data) == notdef and not ch.isspace():
                missing.add(ch)
                continue
            if w > 255 or h > 255:
                continue
            bmp_off = len(bitmaps)
            stride = (w + 1) // 2
            for y in range(h):
                row = bytearray(stride)
                for x in range(w):
                    v = data[y * w + x] >> 4
                    row[x >> 1] |= v << 4 if x % 2 == 0 else v
                bitmaps += row
            glyphs += struct.pack("<IIBBbbB3x", ord(ch), bmp_off, w, h,
                                  max(-128, min(127, xoff)), max(-128, min(127, yoff)), min(255, adv))
            count += 1
        bitmaps += b"\0" * (-len(bitmaps) % 4)
        head += struct.pack("<HHhhIIII", px, ascent + descent, ascent, descent, count,
                            offset, offset + len(glyphs), 0)
        bodies.append(bytes(glyphs) + bytes(bitmaps))
        offset += len(glyphs) + len(bitmaps)
    section = head + b"".join(bodies)
    return section, {"glyphs": len(wanted) - len(missing), "missing": "".join(sorted(missing)),
                     "bytes": len(section)}
