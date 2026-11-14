# Taiko Pocket

A Taiko drum rhythm game for the **Analogue Pocket**. It plays song charts in the TJA
format, the one used by Taikojiro and other PC simulators: you bring the songs, the
Pocket plays them.

It is a new game written for the Pocket, not an emulator. Drum sounds come straight
from the hardware the instant you press a button, and the notes run at a steady 60
frames per second.

## What you need

- An Analogue Pocket (firmware 1.1 or newer) and its SD card.
- Your own songs: folders with a `.tja` chart and its music file.
- A computer to pack the songs once: macOS or Linux (on Windows, use WSL).

## 1. Install the game

1. Download `TaikoPocket-<version>.zip` from the [releases](../../releases) and unzip it.
2. Copy the three folders `Cores`, `Platforms` and `Assets` to the SD card. Let them
   merge with what is there.

To update later, do the same again. Your songs and scores stay.

## 2. Pack your songs

The Pocket cannot read a folder of songs, so they are packed into one file first.
You need Python 3.9 or newer and [ffmpeg](https://ffmpeg.org/).

```sh
git clone https://github.com/nyannurs/taikopocket && cd taikopocket
tools/fetch_third_party.sh font   # downloads the font for song titles
make venv                         # sets up Python in build/venv
build/venv/bin/python tools/packer/pack.py MY_SONGS -o library.tpl
```

`MY_SONGS` is your song folder. Its top-level folders become the folders you see in
the game. Packing takes seconds to minutes and gives about 3 MB per minute of music.

Copy `library.tpl` to the SD card, into `/Assets/taikopocket/common/`. If the packer
wrote more than one file (`library.tp1`, ...), copy them all.

A card formatted as FAT32 cannot hold a file over 4 GB: add `--volume-size 4000m` for
a big library. More options: [docs/packer.md](docs/packer.md).

## 3. Play

On the Pocket: openFPGA, then Taiko Pocket.

| Button | What it does |
|---|---|
| D-pad down or right, Y, B | Don (red notes) |
| D-pad up or left, X, A, L, R | Ka (blue notes) |
| Start | pause: resume, retry or leave the song |
| Select | settings (in song select) |

In song select, **A** opens a folder and picks a song, **B** goes back, **L** and **R**
jump a page, **X** picks a random song. Rest on a song you have played and a small
window shows your best result.

The settings have the scroll speed, volumes, timing offsets with a calibration, button
layouts, and three modifiers that can be combined: invisible notes (Doron), reversed
notes (Abekobe) and random notes (Kimagure or Detarame).

## The look

The game comes with a default skin (pictures and drum sounds) in
[`skins/`](skins/README.md). The packer uses it unless you give another one with
`--skin`; any skin made for Taikojiro works.

## Try it on a computer

The same game runs in a window, which is how it is developed. You need a C compiler,
`make` and SDL2 (macOS: `brew install sdl2 ffmpeg`).

```sh
make
build/venv/bin/python tools/packer/pack.py MY_SONGS -o build/library.tpl
build/taikopocket build/library.tpl
```

Keys: `F` `J` don, `D` `K` ka, arrows to move, `X` confirm, `Z` back, `Enter` pause,
`Tab` settings. There is a browser version too: [docs/web.md](docs/web.md).

## For developers

| Topic | Where |
|---|---|
| How it works, measurements | [docs/video.md](docs/video.md), [docs/performance.md](docs/performance.md), [gateware/taiko/README.md](gateware/taiko/README.md) |
| Building the core, the Pocket's limits | [docs/pocket.md](docs/pocket.md), [docs/platform.md](docs/platform.md) |
| Charts: what is supported, how hits are judged | [docs/tja-notes.md](docs/tja-notes.md), [docs/judge.md](docs/judge.md) |
| The library file | [docs/library-format.md](docs/library-format.md) |
| State of the project | [CONTEXT.md](CONTEXT.md) |

`make test` runs the tests. The game's code is in `src/`, the hardware in `gateware/`,
the packer in `tools/packer/`.

## Credits and licence

- The rules of play (chart reading, timing, scoring) follow
  [YataiDON](https://github.com/Yonokid/YataiDON) and are tested against it.
- The hardware base is [openfpga-litex](https://github.com/agg23/openfpga-litex) (MIT).
- The default skin's pictures and drum sounds are by toach, the author of Taikojiro
  ([skins/README.md](skins/README.md)).

Taiko Pocket is licensed under GPL-3.0 (`LICENSE`). Songs and skins are not part of
that licence. Tested on one Pocket; the Dock is untested.
