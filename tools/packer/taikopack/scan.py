"""Finds charts under a root folder and assigns them to categories.

A category is the nearest folder, from the chart upwards, that has a genre.ini
(Taikojiro) or box.def (TJAPlayer). Without one, it is the top-level folder the
chart sits in. The category's name and colours come from that file if present.
"""

from __future__ import annotations

import configparser
import os
import re
from dataclasses import dataclass

SKIP_DIRS = {"skins", "img", "snd", "lang", ".git", "__pycache__"}


@dataclass
class Found:
    path: str      # chart file
    rel: str       # path relative to the root, with forward slashes
    category: str  # category folder relative to the root ("" = root)


def natural_key(s: str):
    """Sort key that orders "2" before "10". Only ASCII digits count as numbers: int()
    rejects others that str.isdigit() accepts (a circled 1, a superscript 2)."""
    return [int(t) if t and t.isascii() and t.isdigit() else t.casefold() for t in re.split(r"([0-9]+)", s)]


def _color(value: str, default: int) -> int:
    m = re.match(r"\s*#?([0-9a-fA-F]{6})", value or "")
    return int(m.group(1), 16) if m else default


def _read_text(path: str) -> str:
    with open(path, "rb") as f:
        raw = f.read()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    try:
        return raw.decode("utf-8")
    except UnicodeDecodeError:
        return raw.decode("cp932", errors="replace")


def category_info(folder: str, fallback_name: str) -> tuple[str, int, int]:
    """(display name, colour, text colour) from genre.ini / box.def, or defaults."""
    name, color, text = fallback_name, 0x404040, 0xFFFFFF
    entries = {e.lower(): e for e in os.listdir(folder)} if os.path.isdir(folder) else {}
    if "genre.ini" in entries:
        cp = configparser.ConfigParser(strict=False, interpolation=None)
        try:
            cp.read_string(_read_text(os.path.join(folder, entries["genre.ini"])))
            sec = cp["Genre"] if cp.has_section("Genre") else {}
            name = (sec.get("GenreName") or name).strip()
            color = _color(sec.get("GenreColor"), color)
            text = _color(sec.get("FontColor"), text)
        except configparser.Error:
            pass
    elif "box.def" in entries:
        for line in _read_text(os.path.join(folder, entries["box.def"])).splitlines():
            key, _, value = line.partition(":")
            key = key.strip().upper()
            if key == "#TITLE" and value.strip():
                name = value.strip()
            elif key == "#BACKCOLOR":
                color = _color(value, color)
            elif key == "#FORECOLOR":
                text = _color(value, text)
    return name, color, text


def find_charts(root: str) -> list[Found]:
    found = []
    marked: dict[str, bool] = {}

    def is_category(folder: str) -> bool:
        if folder not in marked:
            try:
                names = {e.lower() for e in os.listdir(folder)}
            except OSError:
                names = set()
            marked[folder] = "genre.ini" in names or "box.def" in names
        return marked[folder]

    for dirpath, dirnames, filenames in os.walk(root, followlinks=True):
        dirnames[:] = sorted((d for d in dirnames if d.lower() not in SKIP_DIRS and not d.startswith(".")),
                             key=natural_key)
        for name in sorted(filenames, key=natural_key):
            if not name.lower().endswith(".tja"):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            folder = dirpath
            category = None
            while True:
                if folder != root and is_category(folder):
                    category = os.path.relpath(folder, root).replace(os.sep, "/")
                    break
                if os.path.samefile(folder, root):
                    break
                folder = os.path.dirname(folder)
            if category is None:
                category = rel.split("/")[0] if "/" in rel else ""
            found.append(Found(path, rel, category))
    # the names themselves break ties ("01" and "1", "Pop" and "pop"): a category's songs
    # must lie together, the library stores a category as a first song and a count
    found.sort(key=lambda f: (natural_key(f.category), f.category, natural_key(f.rel), f.rel))
    return found
