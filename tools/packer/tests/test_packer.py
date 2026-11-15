"""Packer tests. Run with `make test` (they need Pillow, not ffmpeg)."""

import os
import random
import shutil
import struct
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import pack  # noqa: E402
from PIL import Image  # noqa: E402
from taikopack import library, scan, skin, tja_meta  # noqa: E402

# scratch space stays inside the repository's build folder
TMP = os.path.join(HERE, "..", "..", "..", "build", "tests", "packer")

CHART = """TITLE:{title}
SUBTITLE:--someone
BPM:120
WAVE:song.ogg
OFFSET:-1.5
DEMOSTART:12.5
COURSE:Oni
LEVEL:8
BALLOON:5
#START
1011,
#BRANCHSTART p,50,80
#N
1,
#E
2,
#M
3,
#BRANCHEND
#LYRIC 歌詞
7008,
#END
COURSE:Easy
LEVEL:3
#START
1,
#END
"""


class TjaMeta(unittest.TestCase):
    def test_encodings(self):
        text = "TITLE:太鼓\n"
        self.assertEqual(tja_meta.decode_tja(text.encode("utf-8")), (text, "utf-8"))
        self.assertEqual(tja_meta.decode_tja(b"\xef\xbb\xbf" + text.encode("utf-8")), (text, "utf-8-sig"))
        self.assertEqual(tja_meta.decode_tja(text.encode("cp932")), (text, "cp932"))
        self.assertEqual(tja_meta.decode_tja(text.encode("utf-16"))[0].lstrip("﻿"), text)

    def test_clean_lines(self):
        self.assertEqual(tja_meta.clean_lines("a // x\r\n// gone\n  \n b\t\n"), ["a", "b"])

    def test_meta(self):
        m = tja_meta.read_meta(CHART.format(title="曲"))
        self.assertEqual((m.title, m.subtitle, m.subtitle_full), ("曲", "someone", False))
        self.assertEqual(m.wave, "song.ogg")
        self.assertAlmostEqual(m.demostart, 12.5)
        self.assertEqual(sorted(m.courses), [0, 3])
        self.assertEqual((m.courses[3].level, m.courses[3].branching, m.courses[3].has_chart), (8, True, True))
        self.assertEqual((m.courses[0].level, m.courses[0].branching), (3, False))
        self.assertEqual(m.lyrics, ["歌詞"])

    def test_taikojiro_habits(self):
        # no COURSE: at all -> Oni; headers before the first COURSE: belong to it;
        # COURSEDESIGNER4: is not a COURSE: line; a missing #END is tolerated
        m = tja_meta.read_meta("TITLE:a\nLEVEL:7\n#START\n1,\n")
        self.assertEqual((sorted(m.courses), m.courses[3].level, m.courses[3].has_chart), ([3], 7, True))
        m = tja_meta.read_meta("LEVEL:9\nCOURSE:3\n#START\n1,\n#END\n")
        self.assertEqual(m.courses[3].level, 9)
        m = tja_meta.read_meta("COURSE:4\nCOURSEDESIGNER4:x\nLEVEL:12\n#START\n1,\n#END\n")
        self.assertEqual((sorted(m.courses), m.courses[4].level), ([4], 12))


class Sprites(unittest.TestCase):
    def test_round_trip(self):
        rnd = random.Random(1)
        im = Image.new("RGBA", (37, 11))
        for y in range(11):
            for x in range(37):
                a = rnd.choice([0, 0, 255, 255, 255, 96, 200])
                im.putpixel((x, y), (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256), a))
        back = skin.decode_frame(skin.encode_frame(im), 37, 11)
        for y in range(11):
            for x in range(37):
                r, g, b, a = im.getpixel((x, y))
                r2, g2, b2, a2 = back.getpixel((x, y))
                self.assertEqual(a2, 255 if a >> 3 == 31 else (a >> 3) << 3)
                if a >> 3:
                    self.assertEqual((r2, g2, b2), (r & 0xF8, g & 0xFC, b & 0xF8))

    def test_ids_and_placeholder_skin(self):
        ids = skin.load_ids()
        self.assertEqual(ids["SPRITE"][0], "NOTES")
        sprites, sounds, cfg, info = skin.build_sections(None)
        self.assertEqual(struct.unpack_from("<I", sprites)[0], len(ids["SPRITE"]))
        w, h, frames = struct.unpack_from("<HHH", sprites, 4)
        self.assertEqual((w, h, frames), (48, 48, 15))
        self.assertEqual(struct.unpack_from("<I", sounds)[0], len(ids["SOUND"]))
        self.assertEqual(len(cfg), 4 + 4 * len(ids["CFG"]))


    def test_dancers(self):
        # a folder of single pictures: three rows of DancerFrames, 1 x 1 files are empty frames
        folder = os.path.join(TMP, "skin_dancers")
        shutil.rmtree(folder, ignore_errors=True)
        os.makedirs(os.path.join(folder, "img", "dancer"))
        with open(os.path.join(folder, "img", "imgdet.ini"), "w") as f:
            f.write("[ImageSettings]\nDancerFrames=2\nDancerLoopBeats=2\n")
        for name, size in (("dancer_n1", (20, 30)), ("dancer_n2", (20, 30)), ("dancer_a1", (1, 1)),
                           ("dancer_a2", (20, 30)), ("dancer_d1", (20, 30)), ("dancer_3n1", (20, 30))):
            Image.new("RGBA", size, (200, 0, 0, 255 if size != (1, 1) else 0)).save(
                os.path.join(folder, "img", "dancer", name + ".png"))
        src = skin.SkinSource(folder)
        frames = src.dancer("", 2)
        self.assertEqual([f.size for f in frames], [(20, 30)] * 6)
        self.assertEqual([f.getbbox() is not None for f in frames], [True, True, False, True, True, False])
        self.assertEqual([f.getbbox() is not None for f in src.dancer("3", 2)], [True] + [False] * 5)
        self.assertIsNone(src.dancer("1", 2))
        cfg = skin.build_cfg(src)
        self.assertEqual((cfg["DANCER_FRAMES"], cfg["DANCER_LOOP_BEATS"]), (2, 2))


class Packing(unittest.TestCase):
    def setUp(self):
        shutil.rmtree(TMP, ignore_errors=True)
        songs = os.path.join(TMP, "songs")
        for folder, title, enc in (("A/one", "ひとつ", "cp932"), ("A/two", "two", "utf-8"), ("B", "three", "utf-8")):
            os.makedirs(os.path.join(songs, folder))
            with open(os.path.join(songs, folder, "chart.tja"), "wb") as f:
                f.write(CHART.format(title=title).encode(enc))
        with open(os.path.join(songs, "A", "genre.ini"), "wb") as f:
            f.write("[Genre]\nGenreName=ジャンル\nGenreColor=#102030\nFontColor=#ffffff\n".encode("cp932"))
        with open(os.path.join(songs, "broken.tja"), "w") as f:
            f.write("TITLE:nothing here\n")
        self.songs = songs

    def pack(self, name, *extra):
        out = os.path.join(TMP, name)
        with open(os.devnull, "w") as null:
            stdout, sys.stdout = sys.stdout, null
            try:
                self.assertEqual(pack.main([self.songs, "-o", out, "--audio", "none", "--font", "builtin", *extra]), 0)
            finally:
                sys.stdout = stdout
        return out

    def test_round_trip(self):
        lib = library.read(self.pack("lib.tpl"))
        self.assertEqual([c.name for c in lib.categories], ["ジャンル", "B"])
        self.assertEqual(lib.categories[0].color, 0x102030)
        self.assertEqual([(c.first_song, c.song_count) for c in lib.categories], [(0, 2), (2, 1)])
        self.assertEqual([s.title for s in lib.songs], ["ひとつ", "two", "three"])  # the broken chart is skipped
        song = lib.songs[0]
        self.assertEqual(song.levels, [3, 255, 255, 8, 255])
        self.assertEqual(song.branch_mask, 1 << 3)
        self.assertEqual(song.tja_offset % library.BLOB_ALIGN, 0)
        text = lib.read_blob(song.tja_offset, len(song.tja)).decode("utf-8")
        self.assertEqual(text, CHART.format(title="ひとつ"))  # stored verbatim, as UTF-8

    def test_deterministic(self):
        a, b = self.pack("a.tpl"), self.pack("b.tpl")
        with open(a, "rb") as fa, open(b, "rb") as fb:
            self.assertEqual(fa.read(), fb.read())

    def test_volumes(self):
        boot = library.read(self.pack("one.tpl")).boot_bytes
        size = boot + (-boot % 4096) + 4096  # room for exactly one chart after the boot region
        out = self.pack("split.tpl", "--volume-size", str(size))
        lib = library.read(out)
        self.assertTrue(os.path.exists(out[:-1] + "1"))
        self.assertEqual([s.tja_offset // size for s in lib.songs], [0, 1, 1])  # no chart straddles a volume
        for s, title in zip(lib.songs, ["ひとつ", "two", "three"]):
            self.assertIn(title, lib.read_blob(s.tja_offset, len(s.tja)).decode("utf-8"))

    def test_scan_order(self):
        self.assertEqual([f.rel for f in scan.find_charts(self.songs)],
                         ["broken.tja", "A/one/chart.tja", "A/two/chart.tja", "B/chart.tja"])


if __name__ == "__main__":
    unittest.main()
