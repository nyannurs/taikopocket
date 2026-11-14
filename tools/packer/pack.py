#!/usr/bin/env python3
"""Taiko Pocket packer: turns a folder of songs into one library file.

    pack.py SONGS_DIR -o build/library.tpl [--skin SKIN_DIR] [--audio adpcm]
    pack.py --list build/library.tpl

See docs/packer.md. Needs Python 3.9+, Pillow, and ffmpeg on the PATH.
"""

from __future__ import annotations

import argparse
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from taikopack import audio, font, library, scan, skin, tja_meta  # noqa: E402


def parse_size(text: str) -> int:
    units = {"k": 1 << 10, "m": 1 << 20, "g": 1 << 30}
    text = text.strip().lower()
    if text and text[-1] in units:
        return int(float(text[:-1]) * units[text[-1]])
    return int(text)


def list_library(path: str) -> int:
    lib = library.read(path)
    names = ["Easy", "Normal", "Hard", "Oni", "Edit"]
    print(f"{path}: version {lib.version}, {len(lib.songs)} songs, {len(lib.categories)} categories, "
          f"{lib.total_bytes / 1e6:.1f} MB, boot region {lib.boot_bytes / 1e3:.0f} kB")
    for ci, cat in enumerate(lib.categories):
        print(f"[{cat.name}]  #{cat.color:06x}  {cat.song_count} songs")
        for s in lib.songs[cat.first_song:cat.first_song + cat.song_count]:
            courses = " ".join(f"{names[i]}:{lv}{'*' if s.branch_mask >> i & 1 else ''}"
                               for i, lv in enumerate(s.levels) if lv != library.NO_COURSE)
            secs = s.audio_samples / audio.RATE
            sub = f" / {s.subtitle}" if s.subtitle else ""
            print(f"  {s.title}{sub}  [{courses}]  {int(secs // 60)}:{int(secs % 60):02d}")
    return 0


DEFAULT_SKIN = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "skins", "taikojiro-original")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("songs", nargs="?", help="root folder, searched recursively for .tja files")
    ap.add_argument("-o", "--output", default="build/library.tpl", help="library file to write (.tpl)")
    ap.add_argument("--skin", default=DEFAULT_SKIN if os.path.isdir(DEFAULT_SKIN) else "none",
                    help="Taikojiro-format skin folder (img/, snd/); default: the default skin, "
                         "skins/taikojiro-original. 'none': plain built-in shapes")
    ap.add_argument("--sounds", help="folder with dong.wav, ka.wav, balloon.wav, potato.wav to use instead of the "
                                     "skin's own snd/ (a file that is missing there falls back to the skin)")
    ap.add_argument("--audio", choices=[*audio.FORMATS, "none"], default="adpcm",
                    help="audio format (default: adpcm, 4:1 stereo)")
    ap.add_argument("--font", help="TTF/OTF font for the glyph atlas (default: Noto Sans JP from third_party/)")
    ap.add_argument("--font-sizes", default="16,12", help="pixel sizes of the large and small UI font")
    ap.add_argument("--volume-size", type=parse_size, default=0,
                    help="split into volumes of at most this size, e.g. 4000m (default: one file)")
    ap.add_argument("--cache", help="audio cache folder (default: <output>.cache)")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="parallel ffmpeg processes")
    ap.add_argument("--limit", type=int, default=0, help="pack only the first N charts (for quick tests)")
    ap.add_argument("--list", metavar="LIBRARY", help="print the contents of a library and exit")
    args = ap.parse_args(argv)

    if args.skin == "none":
        args.skin = None
    for name, folder in (("--skin", args.skin), ("--sounds", args.sounds)):
        if folder and not os.path.isdir(folder):
            ap.error(f"{name}: not a folder: {folder}")
    if args.list:
        return list_library(args.list)
    if not args.songs:
        ap.error("SONGS_DIR is required")
    if not args.output.endswith(".tpl"):
        ap.error("the output file must end in .tpl")
    root = os.path.abspath(args.songs)
    if not os.path.isdir(root):
        ap.error(f"not a folder: {root}")
    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    cache = args.cache or args.output + ".cache"
    warnings: list[str] = []
    warn = lambda rel, msg: warnings.append(f"{rel}: {msg}")
    t0 = time.time()

    charts = scan.find_charts(root)
    if args.limit:
        charts = charts[:args.limit]
    print(f"{len(charts)} charts under {root}")

    # 1. charts: decode, read the menu metadata
    entries = []  # (Found, Meta, tja bytes, audio source path or None)
    chars: set[str] = set()
    for c in charts:
        try:
            with open(c.path, "rb") as f:
                raw = f.read()
            text, encoding = tja_meta.decode_tja(raw)
            if encoding.endswith("?"):
                warn(c.rel, "unknown encoding, read as Shift-JIS with replacement characters")
            meta = tja_meta.read_meta(text)
        except Exception as e:  # never abort the whole pack
            warn(c.rel, f"unreadable ({e}); skipped")
            continue
        courses = {i: k for i, k in meta.courses.items() if i < tja_meta.PLAYABLE_COURSES and k.has_chart}
        if not courses:
            warn(c.rel, "no playable course (missing #START/#END); skipped")
            continue
        for i, k in meta.courses.items():
            if i < tja_meta.PLAYABLE_COURSES and not k.has_chart:
                warn(c.rel, f"course {i} has no chart")
        meta.courses = courses
        if not meta.title:
            meta.title = os.path.splitext(os.path.basename(c.path))[0]
        src = None
        if args.audio != "none":
            if not meta.wave:
                warn(c.rel, "no WAVE: line; skipped")
                continue
            src = os.path.join(os.path.dirname(c.path), meta.wave.replace("\\", os.sep))
            if not os.path.isfile(src):
                warn(c.rel, f"audio file not found: {meta.wave}; skipped")
                continue
        entries.append((c, meta, text.encode("utf-8"), src))

    # 2. audio: encode in parallel, through the cache
    encoded = {}
    if args.audio != "none":
        sources = sorted({e[3] for e in entries})
        done = 0

        def job(src):
            try:
                return src, audio.encode(src, args.audio, cache), None
            except Exception as e:
                return src, None, str(e)

        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            for src, enc, err in pool.map(job, sources):
                done += 1
                if err:
                    warn(os.path.relpath(src, root), f"audio decode failed ({(err.splitlines() or ['unknown error'])[-1]})")
                else:
                    encoded[src] = enc
                if done % 25 == 0 or done == len(sources):
                    print(f"\raudio {done}/{len(sources)}", end="", flush=True)
        print()

    # 3. songs and categories
    songs, categories, cat_index = [], [], {}
    for c, meta, tja, src in entries:
        enc = encoded.get(src) if src else None
        if src and enc is None:
            warn(c.rel, "skipped (no audio)")
            continue
        if c.category not in cat_index:
            folder = os.path.join(root, c.category) if c.category else root
            name, color, text_color = scan.category_info(folder, os.path.basename(folder) or "Songs")
            cat_index[c.category] = len(categories)
            categories.append(library.Category(name, color, text_color, len(songs), 0))
        ci = cat_index[c.category]
        categories[ci].song_count += 1
        levels = [library.NO_COURSE] * 5
        branch_mask = 0
        for i, k in meta.courses.items():
            levels[i] = max(0, min(254, k.level))
            branch_mask |= int(k.branching) << i
        song = library.Song(meta.title, meta.subtitle, ci, levels, branch_mask,
                            int(meta.subtitle_full), meta.demostart, tja)
        if enc:
            song.audio_path, song.audio_codec, song.audio_channels = enc.path, enc.codec, enc.channels
            song.audio_block_bytes, song.audio_block_samples = enc.block_bytes, enc.block_samples
            song.audio_samples, song.audio_bytes = enc.samples, enc.size
        songs.append(song)
        chars.update(meta.title, meta.subtitle, *meta.lyrics)
    # categories that share a display name get their parent folder's name in front
    names = [c.name for c in categories]
    for rel, ci in cat_index.items():
        if names.count(categories[ci].name) > 1 and "/" in rel:
            parent = os.path.dirname(rel)
            parent_name = scan.category_info(os.path.join(root, parent), os.path.basename(parent))[0]
            categories[ci].name = f"{parent_name} / {categories[ci].name}"
    for cat in categories:
        chars.update(cat.name)
    if not songs:
        print("no songs to pack")
        for w in warnings:
            print("warning:", w)
        return 1

    # 4. skin, glyph atlas, file
    sprites, sounds, skincfg, skin_info = skin.build_sections(args.skin, args.sounds)
    sizes = [int(v) for v in args.font_sizes.split(",")]
    fonts, font_info = font.build_section(args.font, sizes, chars)
    if font_info["missing"]:
        warnings.append(f"font has no glyph for: {font_info['missing']}")
    if skin_info["sounds_missing"]:
        warnings.append(f"no usable 16-bit WAV for: {', '.join(skin_info['sounds_missing'])}; that sound will be silent")
    info = library.write(args.output, songs, categories, fonts, sounds, sprites, skincfg, args.volume_size)

    for w in warnings:
        print("warning:", w)
    seconds = sum(s.audio_samples for s in songs) / audio.RATE
    print(f"{len(songs)} songs in {len(categories)} categories, {seconds / 3600:.1f} h of audio ({args.audio})")
    print(f"skin: {skin_info['sprites']} sprites, {skin_info['sprite_bytes'] / 1e3:.0f} kB"
          f"{' (plain built-in shapes)' if not args.skin else ''}; glyphs: {font_info['glyphs']} x {len(sizes)} sizes, "
          f"{font_info['bytes'] / 1e3:.0f} kB; boot region {info['boot_bytes'] / 1e3:.0f} kB")
    for p in info["volumes"]:
        print(f"wrote {p} ({os.path.getsize(p) / 1e6:.1f} MB)")
    if info["total_bytes"] > 0xFFFFFFFF and not args.volume_size:
        print("note: larger than 4 GB. Needs an exFAT card and 48-bit reads in the core; "
              "--volume-size 4000m splits it instead.")
    print(f"{len(warnings)} warnings, {time.time() - t0:.1f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
