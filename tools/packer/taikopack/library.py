"""Library file writer and reader. The format is specified in docs/library-format.md."""

from __future__ import annotations

import os
import struct
import zlib
from dataclasses import dataclass, field

MAGIC = b"TAIKOLIB"
VERSION = 1
HEADER_BYTES = 512
BLOB_ALIGN = 4096
SECTION_ALIGN = 16
(SEC_CATEGORIES, SEC_SONGS, SEC_STRINGS, SEC_FONTS, SEC_SOUNDS, SEC_SPRITES, SEC_SKINCFG) = range(7)
SECTION_COUNT = 8
SONG_BYTES = 64
CATEGORY_BYTES = 24
NO_COURSE = 255


@dataclass
class Song:
    title: str
    subtitle: str
    category: int
    levels: list  # 5 entries, NO_COURSE when the course does not exist
    branch_mask: int
    flags: int
    demostart: float
    tja: bytes
    audio_path: str | None = None  # file with the encoded audio blob
    audio_codec: int = 0
    audio_channels: int = 0
    audio_block_bytes: int = 0
    audio_block_samples: int = 0
    audio_samples: int = 0
    audio_bytes: int = 0
    # filled in by write()
    tja_offset: int = 0
    audio_offset: int = 0


@dataclass
class Category:
    name: str
    color: int = 0x404040
    text_color: int = 0xFFFFFF
    first_song: int = 0
    song_count: int = 0


class _Strings:
    def __init__(self):
        self.data = bytearray(b"\0")  # offset 0 is the empty string
        self.index: dict[str, int] = {"": 0}

    def add(self, s: str) -> int:
        if s not in self.index:
            self.index[s] = len(self.data)
            self.data += s.encode("utf-8") + b"\0"
        return self.index[s]


def volume_path(path: str, volume: int) -> str:
    """library.tpl, library.tp1, library.tp2, ..."""
    return path if volume == 0 else path[:-1] + str(volume)


MAX_VOLUMES = 8  # the Pocket core has eight data slots for the library


class _Volumes:
    """Writes a virtual address space that may be split into several files."""

    def __init__(self, path: str, volume_bytes: int):
        self.path, self.volume_bytes = path, volume_bytes
        self.volume, self.file, self.pos = 0, open(path, "wb"), 0
        self.paths = [path]

    def place(self, size: int, align: int) -> int:
        """Advances to an aligned position where `size` bytes fit in one volume; returns it."""
        pos = self.pos + (-self.pos % align)
        if self.volume_bytes:
            if size > self.volume_bytes:
                raise SystemExit("--volume-size is smaller than a single song")
            volume = pos // self.volume_bytes
            if size and (pos + size - 1) // self.volume_bytes != volume:
                volume += 1  # a blob never straddles two volumes
                pos = volume * self.volume_bytes
            if volume != self.volume:
                self._open(volume)
        local = pos - self.volume * self.volume_bytes if self.volume_bytes else pos
        self.file.seek(local)
        self.pos = pos
        return pos

    def _open(self, volume: int):
        if volume >= MAX_VOLUMES:
            raise SystemExit(f"more than {MAX_VOLUMES} volumes; use a larger --volume-size")
        self.file.close()
        self.volume = volume
        p = volume_path(self.path, volume)
        self.file = open(p, "wb")
        self.paths.append(p)

    def write(self, data: bytes):
        self.file.write(data)
        self.pos += len(data)

    def close(self) -> int:
        self.file.close()
        return self.pos


def write(path: str, songs: list[Song], categories: list[Category], fonts: bytes, sounds: bytes,
          sprites: bytes, skincfg: bytes, volume_bytes: int = 0) -> dict:
    """Writes the library. Songs must be grouped by category. Returns a summary."""
    strings = _Strings()
    cat_blob = bytearray()
    for c in categories:
        cat_blob += struct.pack("<IIIIII", strings.add(c.name), c.first_song, c.song_count,
                                c.color, c.text_color, 0)
    for s in songs:
        strings.add(s.title)
        strings.add(s.subtitle)

    # layout of the boot region
    sections = [b""] * SECTION_COUNT
    counts = [0] * SECTION_COUNT
    sections[SEC_CATEGORIES], counts[SEC_CATEGORIES] = bytes(cat_blob), len(categories)
    sections[SEC_SONGS], counts[SEC_SONGS] = b"\0" * (SONG_BYTES * len(songs)), len(songs)
    sections[SEC_STRINGS] = bytes(strings.data)
    sections[SEC_FONTS], sections[SEC_SOUNDS] = fonts, sounds
    sections[SEC_SPRITES], sections[SEC_SKINCFG] = sprites, skincfg
    offsets = []
    pos = HEADER_BYTES
    for data in sections:
        pos += -pos % SECTION_ALIGN
        offsets.append(pos)
        pos += len(data)
    boot_bytes = pos + (-pos % SECTION_ALIGN)

    if volume_bytes and (volume_bytes % BLOB_ALIGN or volume_bytes <= boot_bytes):
        raise SystemExit(f"--volume-size must be a multiple of {BLOB_ALIGN} bytes and larger than the "
                         f"{boot_bytes} bytes of song list, skin and font")
    for v in range(1, MAX_VOLUMES + 2):  # volumes of an earlier, larger pack would be read as part of this one
        if os.path.exists(volume_path(path, v)):
            os.remove(volume_path(path, v))

    # place the blobs: all charts, then all audio
    out = _Volumes(path, volume_bytes)
    out.pos = boot_bytes
    for s in songs:
        s.tja_offset = out.place(len(s.tja), BLOB_ALIGN)
        out.write(s.tja)
    for s in songs:
        if not s.audio_path:
            continue
        s.audio_offset = out.place(s.audio_bytes, BLOB_ALIGN)
        with open(s.audio_path, "rb") as f:
            while True:
                buf = f.read(1 << 20)
                if not buf:
                    break
                out.write(buf)
    total = out.close()

    song_blob = bytearray()
    for s in songs:
        song_blob += struct.pack(
            "<IIHBB5sBBBIQQIIfIHHI",
            strings.index[s.title], strings.index[s.subtitle], s.category,
            s.audio_codec + 1 if s.audio_path else 0, s.audio_channels,
            bytes(s.levels), s.branch_mask, s.flags, 0,
            len(s.tja), s.tja_offset, s.audio_offset, s.audio_bytes, s.audio_samples,
            s.demostart, zlib.crc32(s.tja), s.audio_block_bytes, s.audio_block_samples, 0)
    assert len(song_blob) == SONG_BYTES * len(songs)
    sections[SEC_SONGS] = bytes(song_blob)

    boot = bytearray(boot_bytes)
    for off, data in zip(offsets, sections):
        boot[off:off + len(data)] = data
    table = b"".join(struct.pack("<IIII", off, len(data), cnt, 0)
                     for off, data, cnt in zip(offsets, sections, counts))
    header = struct.pack("<8sIIQQIIIII", MAGIC, VERSION, HEADER_BYTES, total, volume_bytes,
                         boot_bytes, len(songs), len(categories), 0, 0)
    header += b"\0" * (64 - len(header)) + table
    boot[:len(header)] = header
    build_id = zlib.crc32(bytes(boot))
    struct.pack_into("<I", boot, 48, build_id)
    with open(path, "r+b") as f:
        f.write(boot)
    return {"total_bytes": total, "boot_bytes": boot_bytes, "volumes": out.paths, "build_id": build_id}


@dataclass
class Library:
    """Parsed boot region (for tests and `pack.py --list`)."""
    version: int = 0
    total_bytes: int = 0
    volume_bytes: int = 0
    boot_bytes: int = 0
    build_id: int = 0
    songs: list = field(default_factory=list)
    categories: list = field(default_factory=list)
    sections: list = field(default_factory=list)  # (offset, bytes, count)
    path: str = ""

    def read_blob(self, offset: int, size: int) -> bytes:
        volume = offset // self.volume_bytes if self.volume_bytes else 0
        local = offset - volume * self.volume_bytes
        with open(volume_path(self.path, volume), "rb") as f:
            f.seek(local)
            return f.read(size)


def read(path: str) -> Library:
    with open(path, "rb") as f:
        head = f.read(HEADER_BYTES)
        magic, version, header_bytes, total, volume_bytes, boot_bytes, n_songs, n_cats, _, build_id = \
            struct.unpack_from("<8sIIQQIIIII", head, 0)
        if magic != MAGIC:
            raise ValueError("not a Taiko Pocket library")
        if version != VERSION:
            raise ValueError(f"library version {version}, this tool reads {VERSION}")
        f.seek(0)
        boot = f.read(boot_bytes)
    lib = Library(version, total, volume_bytes, boot_bytes, build_id, path=path)
    for i in range(SECTION_COUNT):
        off, size, count, _ = struct.unpack_from("<IIII", boot, 64 + 16 * i)
        lib.sections.append((off, size, count))
    strings = boot[lib.sections[SEC_STRINGS][0]:][:lib.sections[SEC_STRINGS][1]]

    def text(off: int) -> str:
        return strings[off:strings.index(b"\0", off)].decode("utf-8")

    base = lib.sections[SEC_CATEGORIES][0]
    for i in range(n_cats):
        name, first, count, color, text_color, _ = struct.unpack_from("<IIIIII", boot, base + CATEGORY_BYTES * i)
        lib.categories.append(Category(text(name), color, text_color, first, count))
    base = lib.sections[SEC_SONGS][0]
    for i in range(n_songs):
        (title, subtitle, category, fmt, channels, levels, branch_mask, flags, _, tja_bytes, tja_off,
         audio_off, audio_bytes, audio_samples, demostart, crc, block_bytes, block_samples, _) = \
            struct.unpack_from("<IIHBB5sBBBIQQIIfIHHI", boot, base + SONG_BYTES * i)
        song = Song(text(title), text(subtitle), category, list(levels), branch_mask, flags, demostart, b"")
        song.tja_offset, song.audio_offset = tja_off, audio_off
        song.audio_codec, song.audio_channels = fmt - 1 if fmt else 0, channels
        song.audio_bytes, song.audio_samples = audio_bytes, audio_samples
        song.audio_block_bytes, song.audio_block_samples = block_bytes, block_samples
        song.audio_path = "library" if fmt else None
        song.tja = b"\0" * tja_bytes  # length only; read_blob() fetches the text
        lib.songs.append(song)
    return lib
