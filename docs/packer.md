# Packer

`tools/packer/pack.py` turns a folder of songs into a library file
([format](library-format.md)). It needs Python 3.9+, Pillow and ffmpeg. It is used on
macOS and Linux; it is plain Python, so Windows should work too (`pip install pillow`,
pass a font with `--font`), but that is untested.

```sh
python3 tools/packer/pack.py SONGS_DIR -o build/library.tpl
python3 tools/packer/pack.py --list build/library.tpl
```

| Option | Meaning |
|---|---|
| `-o FILE.tpl` | output (default `build/library.tpl`) |
| `--skin DIR` | Taikojiro-format skin folder (`img/`, `snd/`). Default: the default skin, `skins/taikojiro-original`. `--skin none`: plain built-in shapes and no pictures. |
| `--sounds DIR` | Folder with `dong.wav`, `ka.wav`, `balloon.wav`, `potato.wav` used instead of the skin's `snd/`; a file missing there falls back to the skin. |
| `--audio FORMAT` | `adpcm` (default), `adpcm-mono`, `pcm`, `none` |
| `--font FILE` | font for the glyph atlas (default: Noto Sans JP in `third_party/fonts`) |
| `--font-sizes L,S` | pixel sizes of the large and small UI font (default `16,12`) |
| `--volume-size N` | split into files of at most N bytes, e.g. `4000m` |
| `--cache DIR` | audio cache (default `<output>.cache`) |
| `--jobs N` | parallel ffmpeg processes |
| `--limit N` | only the first N charts |

## What it does

- Finds every `.tja` under the root. A chart's **category** is the nearest folder
  with a `genre.ini` (Taikojiro) or `box.def`, else its top-level folder. Name and
  colours come from that file. Equal names get the parent's name in front.
- Detects the encoding (UTF-8, UTF-8 BOM, UTF-16 BOM, else Shift-JIS) and stores the
  chart as UTF-8, otherwise untouched. Charts are parsed on the device.
- Reads only what the menu needs: title, subtitle, levels, branch flag, `DEMOSTART`.
- Transcodes the `WAVE:` file to 48 kHz. `OFFSET`, `SONGVOL` and `DEMOSTART` are not
  applied; the game does that.
- Rasterises every character used in titles, subtitles, category names and `#LYRIC`
  lines, plus ASCII, into a glyph atlas.
- Converts the skin and the hit sounds.
- Broken songs (no chart, missing audio, undecodable) are skipped with a warning.

The output is deterministic for a given ffmpeg version. Audio is cached by source
path, size and modification time, so a re-run only encodes what changed.

## Audio formats

| Format | Size | Quality |
|---|---|---|
| `pcm` | 192 kB/s, 11.5 MB per minute | bit exact |
| `adpcm` | 48 kB/s, 2.9 MB per minute | about 38 dB signal-to-noise, slight hiss on quiet passages |
| `adpcm-mono` | 24 kB/s, 1.4 MB per minute | same, without stereo |

For 838 songs (about 42 hours) that is roughly 29 GB, 7.2 GB and 3.6 GB. A library
over 4 GB needs an exFAT card. ADPCM also loads four times faster and is what the
gateware streamer was designed around.

## Skins

`skins/taikojiro-original/` is the default skin (`skins/README.md`).

A skin folder is a Taikojiro skin: `img/*.png`, `img/imgdet.ini`, `snd/dong.wav`,
`snd/ka.wav`. Images are used at their own pixel size on the 400x360 screen; full
width images (512 px) are cropped. An extension-less dummy file disables an image, as
in Taikojiro. Which images are used is listed in `src/game/assets.def` and
`tools/packer/taikopack/skin.py`. Not used: mob, player characters, the soul's explosion,
2P images, and `dispconf.ini` (the layout is the game's own). Dancers are read as one
image `dancer.png` or as single pictures in a folder `dancer/`, as Taikojiro's `exp.txt`
describes them, with `DancerFrames` and `DancerLoopBeats` from `imgdet.ini`.

The skin is baked into the library, so changing it only needs a re-pack.

## Checking a song folder

```
python3 tools/packer/pack.py SONGS_DIR -o build/scratch/check.tpl --audio none
make build/tplcheck && build/tplcheck build/scratch/check.tpl
```

`tplcheck` loads every course of every song with the game's own loader (same parser,
same storage limits, same judge set-up) and lists what would not play. For scale: a
library of 684 charts (1,033 courses, 23.9 hours of audio) packed with ADPCM stereo is
one 4.16 GB file.
