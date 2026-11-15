"""Turns a Taikojiro-format skin folder (img/*.png, img/imgdet.ini, snd/*.wav)
into the sprite, sound and settings tables of the library.

Without a skin folder the placeholder skin from default_skin.py is used, so the
project itself ships no third-party art.

Sprite encoding (one frame): `h` u32 row offsets (bytes from the frame start),
then per row a stream of u16 commands, (kind << 14) | count:
  kind 0  skip `count` transparent pixels
  kind 1  `count` opaque RGB565 pixels follow
  kind 2  `count` blended pixels follow as pairs (RGB565, alpha 1..31)
  kind 3  end of row
"""

from __future__ import annotations

import configparser
import io
import os
import re
import struct
import wave
from dataclasses import dataclass

from PIL import Image

SCREEN_W, SCREEN_H = 400, 360
HIT_RATE = 48000

_DEF = os.path.join(os.path.dirname(__file__), "..", "..", "..", "src", "game", "assets.def")


def load_ids(path: str = _DEF) -> dict[str, list[str]]:
    """Reads SPRITE()/SOUND()/CFG() ids from src/game/assets.def."""
    ids: dict[str, list[str]] = {"SPRITE": [], "SOUND": [], "CFG": []}
    with open(path, encoding="utf-8") as f:
        for m in re.finditer(r"^(SPRITE|SOUND|CFG)\((\w+)\)", f.read(), re.M):
            ids[m.group(1)].append(m.group(2))
    return ids


def rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def encode_frame(im: Image.Image) -> bytes:
    im = im.convert("RGBA")
    w, h = im.size
    px = im.tobytes()
    rows = []
    for y in range(h):
        out = []
        x = 0
        base = y * w * 4
        while x < w:
            a = px[base + x * 4 + 3] >> 3
            kind = 0 if a == 0 else 1 if a == 31 else 2
            start = x
            while x < w and x - start < 16383:
                a2 = px[base + x * 4 + 3] >> 3
                if (0 if a2 == 0 else 1 if a2 == 31 else 2) != kind:
                    break
                x += 1
            if kind == 0 and x == w:
                break  # trailing transparency is implied
            out.append((kind << 14) | (x - start))
            for i in range(start, x) if kind else ():
                o = base + i * 4
                out.append(rgb565(px[o], px[o + 1], px[o + 2]))
                if kind == 2:
                    out.append(px[o + 3] >> 3)
        out.append(3 << 14)
        rows.append(struct.pack(f"<{len(out)}H", *out))
    table = bytearray()
    off = 4 * h
    for r in rows:
        table += struct.pack("<I", off)
        off += len(r)
    return bytes(table) + b"".join(rows)


def decode_frame(data: bytes, w: int, h: int) -> Image.Image:
    """Inverse of encode_frame (alpha comes back in 5-bit steps). For tests."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    put = im.putpixel
    for y in range(h):
        (pos,) = struct.unpack_from("<I", data, y * 4)
        x = 0
        while True:
            (cmd,) = struct.unpack_from("<H", data, pos)
            pos += 2
            kind, count = cmd >> 14, cmd & 0x3FFF
            if kind == 3:
                break
            if kind == 0:
                x += count
                continue
            for _ in range(count):
                (c,) = struct.unpack_from("<H", data, pos)
                pos += 2
                a = 255
                if kind == 2:
                    (a5,) = struct.unpack_from("<H", data, pos)
                    pos += 2
                    a = a5 << 3
                r, g, b = (c >> 11) << 3, ((c >> 5) & 63) << 2, (c & 31) << 3
                put((x, y), (r, g, b, a))
                x += 1
    return im


class SkinSource:
    """Images, settings and sounds of a skin folder (or of the placeholder skin)."""

    def __init__(self, folder: str | None, sounds: str | None = None):
        self.folder = folder
        self.sounds = sounds  # folder whose .wav files are used before the skin's own
        self.cfg: dict[str, str] = {}
        self._generated: dict[str, Image.Image] = {}
        if folder:
            ini = os.path.join(folder, "img", "imgdet.ini")
            if os.path.exists(ini):
                cp = configparser.ConfigParser(strict=False, interpolation=None)
                cp.optionxform = str
                with open(ini, "rb") as f:
                    cp.read_string(f.read().decode("cp932", errors="replace"))
                for section in cp.sections():
                    self.cfg.update(cp[section])
        else:
            from . import default_skin

            self._generated = default_skin.images()

    def image(self, stem: str) -> Image.Image | None:
        if not self.folder:
            return self._generated.get(stem)
        base = os.path.join(self.folder, "img", stem)
        if os.path.exists(base):  # an extension-less dummy file disables the image
            return None
        for ext in (".png", ".bmp"):
            if os.path.exists(base + ext):
                im = Image.open(base + ext).convert("RGBA")
                if ext == ".bmp":  # black is transparent in bmp skins
                    px = im.load()
                    for y in range(im.size[1]):
                        for x in range(im.size[0]):
                            if px[x, y][:3] == (0, 0, 0):
                                px[x, y] = (0, 0, 0, 0)
                return im
        return None

    def dancer(self, place: str, count: int) -> list | None:
        """The 3 * count pictures of a dancer (dancing, coming, going): rows of one image
        dancer.png (dancer_1.png for a place), or files dancer_n1.., dancer_a1.., dancer_d1..
        (dancer_1n1.. for a place) in the folder "dancer". `place` is "" or "1" .. "5". A
        missing or 1 x 1 picture is an empty one."""
        if not self.folder or count < 1:
            return None
        folder = os.path.join(self.folder, "img", "dancer")
        if not os.path.isdir(folder):
            im = self.image("dancer_" + place if place else "dancer")
            return _grid(im, count, 3) if im else None
        frames = []
        for row in "nad":
            for i in range(1, count + 1):
                path = os.path.join(folder, f"dancer_{place}{row}{i}.png")
                frames.append(Image.open(path).convert("RGBA") if os.path.exists(path) else None)
        sizes = [f.size for f in frames if f and f.size != (1, 1)]
        if not sizes:
            return None
        size = max(sizes)
        return [f if f and f.size == size else Image.new("RGBA", size, (0, 0, 0, 0)) for f in frames]

    def int(self, key: str, default: int) -> int:
        try:
            return int(float(self.cfg.get(key, default)))
        except ValueError:
            return default

    def color(self, key: str, default=(1.0, 1.0, 1.0)):
        try:
            parts = [float(v) for v in self.cfg[key].split(",")]
            return tuple(parts[:3]) if len(parts) >= 3 else default
        except (KeyError, ValueError):
            return default

    def sound(self, name: str) -> tuple[int, bytes] | None:
        """(rate, mono s16le) of <sounds>/<name>.wav, else snd/<name>.wav, else the placeholder sound."""
        path = os.path.join(self.sounds, name + ".wav") if self.sounds else ""
        if not os.path.exists(path):
            if not self.folder:
                from . import default_skin

                return default_skin.sound(name)
            path = os.path.join(self.folder, "snd", name + ".wav")
        if not os.path.exists(path):
            return None
        with wave.open(path, "rb") as w:
            if w.getsampwidth() != 2:
                return None
            ch, rate, data = w.getnchannels(), w.getframerate(), w.readframes(w.getnframes())
        samples = struct.unpack(f"<{len(data) // 2}h", data)
        if ch > 1:
            samples = [sum(samples[i : i + ch]) // ch for i in range(0, len(samples), ch)]
        return rate, struct.pack(f"<{len(samples)}h", *samples)


def _grid(im, cols, rows):
    cw, ch = im.size[0] // cols, im.size[1] // rows
    return [im.crop((c * cw, r * ch, (c + 1) * cw, (r + 1) * ch)) for r in range(rows) for c in range(cols)]


def _cells(im, cw, ch, cells):
    return [im.crop((c * cw, r * ch, (c + 1) * cw, (r + 1) * ch)) for c, r in cells]


def _tint(im, color):
    r, g, b, a = im.split()
    lut = lambda k: [min(255, int(v * k)) for v in range(256)]
    return Image.merge("RGBA", (r.point(lut(color[0])), g.point(lut(color[1])), b.point(lut(color[2])), a))


def _crop(im, w, h, ax, ay):
    """Crop to at most w x h; ax/ay pick the kept side: 0 left/top, 0.5 centre, 1 right/bottom."""
    iw, ih = im.size
    x0 = int(max(0, iw - w) * ax)
    y0 = int(max(0, ih - h) * ay)
    return im.crop((x0, y0, min(iw, x0 + w), min(ih, y0 + h)))


def _gradient(color, w=SCREEN_W, h=48, reach=160):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = im.load()
    for x in range(reach):
        a = int(200 * (1 - x / reach))
        for y in range(h):
            px[x, y] = (*color, a)
    return im


def build_frames(src: SkinSource) -> dict[str, list]:
    """Sprite name -> list of PIL frames. Names that are missing are left out."""
    out: dict[str, list] = {}
    img = src.image
    digits = [(c, 2) for c in range(2, 12)]

    def put(name, frames):
        if frames:
            out[name] = frames

    def single(name, stem):
        im = img(stem)
        if im:
            put(name, [im])

    def grid(name, stem, cols, rows=1):
        im = img(stem)
        if im and cols > 0 and rows > 0:
            put(name, _grid(im, cols, rows))

    notes = img("notes")
    if notes is None:
        raise SystemExit("skin: img/notes.png is required")
    cell = notes.size[1] // max(1, src.int("NoteFrames", 1))
    cols = notes.size[0] // cell
    frames = _grid(notes.crop((0, 0, cols * cell, cell)), cols, 1)
    if cols < 15:  # no kusudama images: reuse the balloon ones
        frames += frames[11:13]
    put("NOTES", frames[:15])

    se = img("senotes")
    if se:
        n = se.size[0] // 48
        frames = _grid(se, n, 1)
        if n < 12:
            frames.append(frames[10])
        put("SENOTES", frames[:12])

    single("MTAIKO", "mtaiko")
    grid("MTAIKO_RED", "mtaikoflash_red", 2)
    grid("MTAIKO_BLUE", "mtaikoflash_blue", 2)
    grid("JUDGE", "judgement", 1, 3)
    grid("SOUL", "soul", src.int("SoulFrames", 4) + 1)

    ex = img("explosion_upper")
    if ex:
        n = src.int("ExplosionFrames", 0)
        if n >= 1:
            put("EXPLOSION", _grid(ex, n, 4))
        else:
            put("EXPLOSION", [_tint(ex, src.color("ExplosionColorGreat", (1.0, 0.92, 0.22))),
                              _tint(ex, src.color("ExplosionColorGood"))])
    exl = img("explosion_lower")
    if exl:
        n = src.int("ExplosionFramesLower", 0)
        put("EXPLOSION_LOWER", _grid(exl, n, 4) if n >= 1 else [exl])
    grid("FIRE", "fire", src.int("FireFrames", 4))

    font = img("font")
    if font:
        put("NUM_S", _cells(font, font.size[0] // 12, font.size[1] // 7, digits))
    font_m = img("font_m") or font
    if font_m:
        put("NUM_M", _cells(font_m, font_m.size[0] // 12, font_m.size[1] // 7, digits + [(5, 5)]))
    for name, stems in (("NUM_COMBO", ["combonumber"]), ("NUM_COMBO_L", ["combonumber_l", "combonumber"]),
                        ("NUM_ROLL", ["rollnumber", "combonumber"]), ("NUM_POTATO", ["potatonumber", "rollnumber"])):
        for stem in stems:
            if img(stem):
                grid(name, stem, 10)
                break
        else:
            if "NUM_M" in out:
                put(name, out["NUM_M"][:10])
    nb = img("combonumber_balloon_p1") or img("combonumber_balloon")
    if nb:
        if img("combonumber_balloon_p1") is None:
            nb = _tint(nb, src.color("ComboBalloonNumberColorP1"))
        put("NUM_BALLOON", _grid(nb, 10, 1))

    single("ROLL_BALLOON", "rollballoon")
    single("BURST_BALLOON", "burstballoon")
    single("POTATO_BALLOON", "potatoballoon" if img("potatoballoon") else "rollballoon")
    single("COMBO_BALLOON", "comboballoon_p1")
    grid("BRANCH_BALLOON", "branchballoon", 1, 2)
    grid("CLEARMARK", "clearmark", 5)
    single("MARK_FULLCOMBO", "resultmark_fullcombo")
    single("MARK_NEWRECORD", "resultmark_newrecord")
    unit = src.int("NormaGaugeUnitWidth", 16)
    rg = img("resultgauge")
    if rg:
        put("RESULT_GAUGE", _grid(rg, rg.size[0] // unit, 2))
    grid("RESULT_MSG", "resultmsg", 1, 2)
    board = img("resultboard")
    if board:
        put("RESULT_BOARD", [_grid(board, 2, 1)[0]])
    elif img("resultboard_p1"):
        single("RESULT_BOARD", "resultboard_p1")
    gauge = img("normagauge")
    if gauge:
        put("GAUGE", _grid(gauge, gauge.size[0] // unit, 4))

    for name, stem, ax in (("HR_SENOTES", "hr_senotes", 0), ("HR_SONGTITLE", "hr_songtitle", 0),
                           ("LAYER_NORMAL", "normallayer", 1), ("LAYER_EXPERT", "expertlayer", 1),
                           ("LAYER_MASTER", "masterlayer", 1), ("FLASH_HIT", "sfieldflash_hit", 0),
                           ("FLASH_RED", "sfieldflash_red", 0), ("FLASH_BLUE", "sfieldflash_blue", 0),
                           ("FLASH_YELLOW", "sfieldflash_yellow", 0), ("LANE_BG", "sfieldbg", 0),
                           ("LANE_BG_GOGO", "sfieldbg_gogo", 0)):
        im = img(stem)
        if im:
            put(name, [_crop(im, SCREEN_W, im.size[1], ax, 0)])
    # what Taikojiro draws when these images are missing
    out.setdefault("FLASH_RED", [_gradient((255, 64, 32))])
    out.setdefault("FLASH_BLUE", [_gradient((64, 160, 255))])
    out.setdefault("FLASH_YELLOW", [_gradient((255, 220, 40))])
    out.setdefault("LANE_BG", [Image.new("RGBA", (SCREEN_W, 56), (0, 0, 0, 255))])
    out.setdefault("LANE_BG_GOGO", [Image.new("RGBA", (SCREEN_W, 56), (72, 0, 88, 255))])

    symbols = []
    for course in ("easy", "normal", "hard", "oni"):
        im = img("coursesymbol_" + course)
        if im is None:
            symbols = []
            break
        symbols += _grid(im, 2, 1)
    put("COURSE_SYMBOL", symbols)
    grid("GOGO_SPLASH", "gogosplash", src.int("GoGoSplashFrames", 8))
    for name, place in [("DANCER", "")] + [(f"DANCER_{k}", str(k)) for k in range(1, 6)]:
        put(name, src.dancer(place, src.int("DancerFrames", 0)))

    for name, stem, ax, ay in (("BG", "bg", 0.5, 0.5), ("BG_CLEAR", "bg_clear", 0.5, 0.5),
                               ("BG_HOT", "bg_hot", 0.5, 0.5), ("BG_MISS", "bg_miss", 0.5, 0.5),
                               ("SONGSELECT_BG", "songselectbg", 1, 1),
                               ("RESULT_BG_CLEAR", "resultbg_clear", 1, 1),
                               ("RESULT_BG_FAIL", "resultbg_failure", 1, 1)):
        im = img(stem)
        if im:
            canvas = Image.new("RGBA", (SCREEN_W, SCREEN_H), (0, 0, 0, 255))
            part = _crop(im, SCREEN_W, SCREEN_H, ax, ay)
            canvas.alpha_composite(part, ((SCREEN_W - part.size[0]) // 2, (SCREEN_H - part.size[1]) // 2))
            put(name, [canvas])
    return out


def build_cfg(src: SkinSource) -> dict[str, int]:
    bar = src.color("BarLineColor")
    gauge = src.image("normagauge")
    unit = src.int("NormaGaugeUnitWidth", 16)
    return {
        "SOUL_INTERVAL_MS": src.int("SoulInterval", 50),
        "FIRE_INTERVAL_MS": src.int("FireInterval", 50),
        "GAUGE_INTERVAL_MS": src.int("NormaGaugeInterval", 100),
        "GAUGE_FRAMES": max(1, gauge.size[0] // (unit * 6)) if gauge else 1,
        "EXPLOSION_INTERVAL_MS": src.int("ExplosionInterval", 200),
        "EXPLOSION_FRAMES": src.int("ExplosionFrames", 0),
        "GOGOSPLASH_INTERVAL_MS": src.int("GoGoSplashInterval", 50),
        "BARLINE_COLOR": rgb565(*[min(255, int(c * 255)) for c in bar]),
        "DANCER_FRAMES": max(0, src.int("DancerFrames", 0)),
        "DANCER_LOOP_BEATS": max(1, src.int("DancerLoopBeats", 4)),
    }


SOUND_FILES = {"DON": "dong", "KA": "ka", "BALLOON": "balloon", "POTATO": "potato"}


def resample(rate: int, pcm: bytes, to_rate: int = HIT_RATE) -> bytes:
    """Linear resampling of mono s16le; hit sounds are short, so this is cheap."""
    if rate == to_rate:
        return pcm
    src = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    n = len(src) * to_rate // rate
    out = []
    for i in range(n):
        pos = i * rate / to_rate
        j = int(pos)
        frac = pos - j
        a = src[j]
        b = src[j + 1] if j + 1 < len(src) else a
        out.append(int(round(a + (b - a) * frac)))
    return struct.pack(f"<{n}h", *out)


def build_sections(folder: str | None, sounds: str | None = None) -> tuple[bytes, bytes, bytes, dict]:
    """Returns the SPRITES, SOUNDS and SKINCFG sections plus a summary for the log."""
    ids = load_ids()
    src = SkinSource(folder, sounds)
    frames = build_frames(src)

    # sprites: u32 count, entries {u16 w, h, frames, flags; u32 frame_table}, then data
    names = ids["SPRITE"]
    head = io.BytesIO()
    body = io.BytesIO()
    head_size = 4 + 12 * len(names)
    for name in names:
        fr = frames.get(name, [])
        if not fr:
            head.write(struct.pack("<HHHHI", 0, 0, 0, 0, 0))
            continue
        w, h = fr[0].size
        encoded = [encode_frame(f.resize((w, h)) if f.size != (w, h) else f) for f in fr]
        table_off = head_size + body.tell()
        off = table_off + 4 * len(encoded)
        table = bytearray()
        for e in encoded:
            table += struct.pack("<I", off)
            off += len(e)
            off += -off % 4
        body.write(table)
        for e in encoded:
            body.write(e)
            body.write(b"\0" * (-len(e) % 4))
        head.write(struct.pack("<HHHHI", w, h, len(encoded), 0, table_off))
    sprites = struct.pack("<I", len(names)) + head.getvalue() + body.getvalue()

    # sounds: u32 count, entries {u32 rate, samples, data offset, reserved}, then s16le data
    snd_names = ids["SOUND"]
    head = io.BytesIO()
    body = io.BytesIO()
    head_size = 4 + 16 * len(snd_names)
    silent = []
    for name in snd_names:
        s = src.sound(SOUND_FILES[name])
        if s is None:
            silent.append(SOUND_FILES[name] + ".wav")
            head.write(struct.pack("<IIII", 0, 0, 0, 0))
            continue
        pcm = resample(*s)
        head.write(struct.pack("<IIII", HIT_RATE, len(pcm) // 2, head_size + body.tell(), 0))
        body.write(pcm)
        body.write(b"\0" * (-len(pcm) % 4))
    sounds = struct.pack("<I", len(snd_names)) + head.getvalue() + body.getvalue()

    cfg_values = build_cfg(src)
    cfg = struct.pack("<I", len(ids["CFG"])) + b"".join(
        struct.pack("<i", cfg_values.get(n, 0)) for n in ids["CFG"])

    summary = {
        "sprites": sum(1 for n in names if frames.get(n)),
        "sprites_missing": [n for n in names if not frames.get(n)],
        "sprite_bytes": len(sprites),
        "sounds_missing": silent,  # not there, or not 16-bit PCM: that sound will be silent
    }
    return sprites, sounds, cfg, summary
