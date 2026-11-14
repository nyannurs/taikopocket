"""TJA decoding and the little metadata the song-select menu needs.

The packer never interprets a chart: it transcodes the text to UTF-8, stores it
verbatim, and reads just enough headers for the table of contents. The rules
here mirror YataiDON's get_metadata()/find_chart() so that the menu agrees with
what the on-device parser will find.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field

COURSE_ALIASES = {
    "0": 0, "easy": 0, "1": 1, "normal": 1, "2": 2, "hard": 2, "3": 3, "oni": 3,
    "4": 4, "edit": 4, "ura": 4, "5": 5, "tower": 5, "6": 6, "dan": 6,
}
PLAYABLE_COURSES = 5  # Easy, Normal, Hard, Oni, Edit(Ura); Tower and Dan are out of scope

_FLOAT = re.compile(r"\s*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)")


def decode_tja(raw: bytes) -> tuple[str, str]:
    """Returns (text, encoding). UTF-8 with or without BOM, UTF-16 with BOM, else CP932."""
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", errors="replace"), "utf-8-sig"
    if raw.startswith(b"\xff\xfe") or raw.startswith(b"\xfe\xff"):
        return raw.decode("utf-16"), "utf-16"
    try:
        return raw.decode("utf-8"), "utf-8"
    except UnicodeDecodeError:
        pass
    try:
        return raw.decode("cp932"), "cp932"
    except UnicodeDecodeError:
        return raw.decode("cp932", errors="replace"), "cp932?"


def clean_lines(text: str) -> list[str]:
    """YataiDON's line cleaning: drop // comments, trim, skip empty lines."""
    out = []
    for line in text.split("\n"):
        pos = line.find("//")
        if pos == 0:
            continue
        if pos > 0:
            line = line[:pos]
        line = line.strip(" \t\r\n")
        if line:
            out.append(line)
    return out


def parse_float(value: str, default: float = 0.0) -> float:
    m = _FLOAT.match(value)
    return float(m.group(1)) if m else default


def has_implicit_course(lines: list[str]) -> bool:
    """A chart that reaches #START before any COURSE: is an Oni chart (Taikojiro)."""
    for line in lines:
        if line.split(":", 1)[0] == "COURSE":
            return False
        if line.startswith("#START"):
            return True
    return False


@dataclass
class Course:
    level: int = 0
    branching: bool = False
    has_chart: bool = False


@dataclass
class Meta:
    title: str = ""
    subtitle: str = ""
    subtitle_full: bool = False
    wave: str = ""
    genre: str = ""
    demostart: float = 0.0
    courses: dict[int, Course] = field(default_factory=dict)
    lyrics: list[str] = field(default_factory=list)


def read_meta(text: str) -> Meta:
    lines = clean_lines(text)
    meta = Meta()
    titles: dict[str, str] = {}
    subtitles: dict[str, str] = {}
    cur = -1
    first_course = True
    pending_level = 0
    if has_implicit_course(lines):
        cur = 3
        first_course = False
        meta.courses[3] = Course()

    for line in lines:
        if line.startswith("#"):
            if cur != -1 and cur in meta.courses:
                if line.startswith("#BRANCH"):
                    meta.courses[cur].branching = True
                elif line in ("#START", "#START P1", "#START P2"):
                    meta.courses[cur].has_chart = True  # a missing #END means "to the end"
            if line.startswith("#LYRIC"):
                meta.lyrics.append(line[6:].strip())
            continue
        if line[0].isdigit():
            continue
        value = line.split(":", 1)[1] if ":" in line else ""
        key = line.split(":", 1)[0]
        if line.startswith("SUBTITLE"):
            region = "en" if len(line) <= 8 or line[8] == ":" else line[8:10].lower()
            if value.startswith("++") or value.startswith("/ "):
                meta.subtitle_full = True
                value = value[2:]
            elif value.startswith("--"):
                meta.subtitle_full = False
                value = value[2:]
            subtitles[region] = value
        elif line.startswith("TITLE"):
            region = "en" if len(line) <= 5 or line[5] == ":" else line[5:7].lower()
            titles[region] = value
        elif key == "DEMOSTART":
            meta.demostart = parse_float(value)
        elif key == "WAVE":
            meta.wave = value.strip()
        elif key == "GENRE":
            meta.genre = value.strip()
        elif key == "COURSE":  # exact: COURSEDESIGNER4: is not a course
            cur = COURSE_ALIASES.get(value.strip().lower(), -1)
            if cur != -1:
                # headers written before the first COURSE: belong to that course
                meta.courses[cur] = Course(level=pending_level if first_course else 0)
                first_course = False
        elif key == "LEVEL":
            if cur != -1:
                meta.courses[cur].level = int(parse_float(value))
            elif first_course:
                pending_level = int(parse_float(value))

    def pick(table: dict[str, str]) -> str:
        if table.get("en"):
            return table["en"]
        for region in sorted(table):
            if table[region]:
                return table[region]
        return ""

    meta.title = pick(titles).strip()
    meta.subtitle = pick(subtitles).strip()
    return meta
