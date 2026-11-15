# Context

Working state of the project: what is done, what was decided and why, what is known
about the platform. Update it when something changes.

## Status

| # | Milestone | State |
|---|---|---|
| 1 | Repo setup, license, layout, SDL build, CI | done |
| 2 | Packer v1, library format, desktop loader | done |
| 3 | TJA parser + golden tests vs YataiDON | done: 848 charts bit-identical |
| 4 | Judge / scoring / gauge / branches + replay tests | done: 6,432 replays identical |
| 5 | Desktop-playable game (+ web build) | done |
| 6 | Pocket bring-up | **runs on a Pocket**: boots, plays songs at 60 fps, saves scores and settings |
| 7 | Gateware: hit sounds, input timestamps, music streamer, sprite layer | works on hardware; simulated against software models (`make -C gateware/taiko sim`) |
| 8 | Calibration, latency, performance, docs, release zip | done; releases are on GitHub. The Dock is untested |

## Decisions

- **License** GPL-3.0 (brief). **Language** C11 for the game, Python for the packer.
- **Build** plain `make`; all output in `build/`.
- **Resolution** 400x360 RGB565, integer 4x on the Pocket's 1600x1440 panel. Skin
  sprites are used 1:1 (Taikojiro skins are drawn for 512x384). See `docs/video.md`.
- **Gateware fork** `gateware/` is openfpga-litex at commit `c21570d` plus `taiko/`,
  `pkg/` and changes to five files of `target/pocket/` and the project file. Its example
  programs, Rust crates and editor settings were removed (nothing here used them). To
  take over upstream changes, compare with that commit by hand. The LiteX SoC
  (`rtl/litex/litex.v`) is used as committed; `litex/` is the generator it came from.
- **Platform API** `src/platform/platform.h`. Async library reads, a music ring buffer
  the game fills, an output-sample counter that timestamps input and drives game time.
- **Audio engine** `src/platform/common/softmix.c` is the software model of the
  gateware (music streamer, 4 hit voices, mixer). SDL uses it directly.
- **Music formats** 48 kHz s16 PCM, or IMA ADPCM in WAV block layout (2041 samples per
  block), encoded by ffmpeg, decodable per block so seeking is trivial.
- **Parser** `src/core/tja.c` is a line-by-line port of YataiDON's. `compat=false`
  is exactly YataiDON (checked by `make golden`); the game uses `compat=true`, four
  extra rules for Taikojiro-style charts (no `COURSE:`, headers before `COURSE:`,
  `COURSEDESIGNER`, missing `#END`). See `docs/tja-notes.md`.
- **Float quirks matter for bit-exact parity**: YataiDON parses with `std::stof`
  (float) and a few products are done in float (`#DELAY`, `#JPOSSCROLL`,
  `compute_load_time`). `src/core/num.c` reimplements stof/stod/stoi.
- **Judge timing keeps YataiDON's 720p lane constants** (`TP_LANE_*` in `chart.h`),
  because the branch decision time depends on a note's load time. Rendering uses
  its own geometry.
- **Scroll speed** is YataiDON's `scroll_distance()` (its fix of 2026-11-06): one 4/4
  bar at scroll 1 is screen width + 82 px of a 1280 wide screen - judge position.
  948 px there, 345.6 px here (`TRAVEL` in `scene_game.c`; it was 320, the form
  before the fix).

## Game code map (`src/game`)

- `app.c` scene switching, input (edges + auto-repeat), boot scene, hit-sound loading.
- `gfx.c` renderer: background layer + dirty rectangles, RLE sprites, glyph text.
- `library.c` library boot region and offset reads. `song.c` chart load, audio
  stream into the platform's music ring, preview. `settings.c` settings + scores.
- `scene_select.c`, `scene_game.c`, `scene_result.c`, `scene_settings.c`,
  `scene_calibrate.c`, `scene_stats.c` (lifetime drum hits and time, in the save).
- `prof.c`: frame rate counter and the profiler overlay (Select during a song).
- Game time = audio position: `song_now_ms()`; input edges: `song_event_ms(counter)`.
- Screens can be captured headless: `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy
  build/taikopocket LIB --frames N --shot out.bmp --script "20:A 21:-A" --set autoplay=1`.
  Delete `LIB.sav` first; it remembers the last song.
- Python: Homebrew's `python3` (3.15, pulled in by emscripten) has no Pillow; use
  `build/venv/bin/python` (`make venv`). The Makefile does this automatically.

## Tests

- `make test`: C unit tests (`tests/test_*.c`), packer tests, C-vs-Python library check.
- `tests/game/test_game.c` runs the real game on a simulated platform
  (`tests/host`): menus, full plays, a sample-accurate scripted player, pause, retry,
  settings, calibration. 0.1 s.
- `make golden`: parser and judge against YataiDON's own code (`tests/yatai/` links
  `third_party/YataiDON`). Uses every `.tja` under `tests/data/songs`,
  `third_party/YataiDON/Songs` and `taikojiro_reference/`.
- `tools/tjadump`, `tools/judgesim`, `tools/tplist` are the host tools behind those.

## Verified platform facts (brief section 2)

- Input: the APF pad controller in the core polls every 2^16 cycles of the 74.25 MHz
  clock, so `cont1_key` updates every 0.88 ms. Much better than once per frame.
- Audio: the 4096-sample FIFO is filled by the CPU; samples leave it at 48 kHz in the
  12.288 MHz audio clock domain. A mixer placed after the FIFO adds no buffering.
- Files: bridge reads are slot + offset + length. The stock openfpga-litex only sends
  the 32-bit read (`0x0180`); this core sends the 48-bit one (`0x0181`) when an offset
  needs it, so one library file can be larger than 4 GB (used: 4.16 GB). The format can
  also split a library into up to eight volumes, for FAT32 cards.
- Files: openfpga-litex **does** expose "open file by path into a slot" (`0x0192`).
  The brief says paths cannot be opened; the single-library design does not rely on it.
- Video: up to 800x720, pixel clock up to about 50 MHz.
- SD writes: the firmware creates and writes `taikopocket.sav` (slot 9) through the
  bridge; this works on hardware. A failure only turns saving off for the session.
  Growing an existing file has never been tried, so the save keeps one size.
- CPU caches: 4 kB instruction, 4 kB data, both direct-mapped. Measured on a
  Pocket: **0.1 us per copied pixel, 0.7 us per blended pixel**, so about
  100,000 pixels per frame. Rule: draw a thing when it changes, not every frame;
  no decorative effect over `pf_blend_budget()` blended pixels (`docs/performance.md`).
- There is no effects setting. Flashes, flames and bursts are always on; a decorative effect the CPU would have to
  blend beyond `pf_blend_budget()` is still left out (the Go-Go splash without the
  sprite layer).
- **The lane is drawn by the FPGA** (sprite layer, `docs/video.md`,
  `gateware/taiko/README.md`): the CPU cannot draw a screen of notes in a frame.
  The software path stays as the fallback for old bitstreams and oversized skins.
- Song select has two levels, folders and songs, and is paged (six rows), not scrolling: a scrolling list
  repaints the screen on every step, which the Pocket cannot do at 60 fps.

## YataiDON facts that differ from the brief

- Gauge code is `GaugeCore` in `judge.h`, not `global_data.h`.
- `JudgeCore` has no Go-Go score multiplier and no big-note bonus. Scoring is
  "shinuchi" (fixed base score, default) or "gen3" (`SCOREINIT`/`SCOREDIFF`, +10000 per
  100 combo).
- `#LEVELHOLD`, `SCOREMODE`, `SONGVOL`, `SEVOL` are not parsed by YataiDON.
- Only note characters `0-9`, `A`, `B` are accepted; others are blank with a warning.

## Pocket side

- `src/platform/pocket/`: `platform_pocket.c` (registers only), `taiko_regs.h` (all
  register addresses and the memory map), `libc_min.c` (freestanding: own string,
  snprintf, math, 64-bit helpers; no libc or libgcc is linked), `crt0.S`, `link.ld`.
- `gateware/taiko/`: RTL + Verilator test bench; README there has the design, the
  latency budget and what was changed in the base core. The LiteX SoC (`litex.v`)
  is used as committed, not regenerated.
- Core definition: `gateware/pkg/pocket/` (core `nyannurs.TaikoPocket`, platform
  `taikopocket`, data slots 0 program, 1-8 library volumes).
- `tools/make_release.py` builds the SD card folder; `.github/workflows/pocket.yml`
  builds firmware + bitstream (Quartus container) + release in GitHub Actions.
- Effects (judgment text, burst, counters) follow YataiDON's skin animation values;
  a copy of its `animation.json` and scripts is in `third_party/yatai_skin/` for
  reference only. The graphics are the default skin's (`skins/`).

## Open

- **Dock** untested.
- **Trails between close notes on the Pocket's own screen.** While notes move, a
  bright bar can show in a gap of a few pixels between two notes. It is in no single
  frame: it is gone when the game is paused, the desktop model and the gateware
  simulation of the same frames (with the default skin) do not have it, and a frame
  is always drawn from one sprite list. Reading: the LCD's pixels do not go from white
  to dark within one frame. Not confirmed; a Dock and a TV would tell.
- **Browser build**: music and notes were once seen out of step under CPU load. The
  game clock does not drift; the suspect is the ScriptProcessorNode. An AudioWorklet
  backend would remove the doubt.
- **Per-song offset**: charts whose `OFFSET` does not suit their audio can only be
  fixed in the TJA and packed again. A setting in the game would be kinder.
- `docs/performance.md` has the measurements; what is left for the CPU per frame is
  mostly the HUD.

### Gateware version 4 (2026-11-14): simulated, not yet confirmed on a Pocket

Built: timing met (setup +0.546 ns, hold +0.076 ns), 300 of 308 M10K blocks.

What a code review found in the hardware, and two wishes, in one bitstream
(`gateware/taiko/README.md`, "Versions"):
- A don and a ka pressed in the same instant both sound (requests are counted).
- Four hit sounds: the balloon's and the kusudama's are played when one bursts.
- The frame buffer is switched where the SoC's reader starts over (`present()`), not
  at the vertical sync, where the reader had already fetched 82 lines: the top of the
  picture was one frame behind the rest. The sprite list is taken over at the start
  of the same frame, and the firmware waits for that before it writes the other list.
- The end of the song needs the streamer to have delivered everything.
- Volumes, button masks and debounce cross into the audio clock as whole words.
- `.data` is restored on every start (`crt0.S`, `link.ld`); button edges from before
  a reset are dropped.

Since 0.2.0, in the game: charts with one route per branch point (only `#M`, then only
`#N`) play through, because a section belongs to its own `#BRANCHSTART`
(`docs/judge.md`, extensions); song rows show two arrows for a course with branches.

### Known and not done

- Unused in the gateware: the CPU-fed audio path, the mono PCM path, three RTL files
  of the base (`sram.sv`, `data_loader.sv`, `wishbone.sv`), the LiteX generator under
  `litex/`. Left as they are.
- Two buttons of the same kind pressed in the same instant are one stroke on the
  Pocket and two on the desktop.
- The Go-Go flame is drawn by the CPU every frame of Go-Go Time (3 to 4 ms). It was
  tried on the sprite layer at half resolution and taken back: a hit burst in front of
  it hid it, because a sprite pixel is mixed with the picture under it, not with
  another sprite. The layer would have to mix sprite over sprite first.
- The lyric line is drawn by the CPU every frame (several ms on charts that have one).
- Loading has no timeout: a read that never ends leaves "Loading..." on screen.
- A Go-Go splash over 127 x 127 pixels is silently left out.
- Auto-play stops hitting drumrolls for a while after a tempo drop (as YataiDON does).
- A branch decision re-sorts every waiting note, and the chart loader makes several
  passes over all notes; both are slow only for very large charts.
- Without an audio device the desktop build's clock does not move.
- The skin has pictures the game does not use yet (dancers, second player).
- Only the firmware (`make pocket`) and the library need rebuilding for game or skin
  changes; the bitstream only for gateware changes.
