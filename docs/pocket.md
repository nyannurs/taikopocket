# Running on the Analogue Pocket

**State: runs on a Pocket.** It boots, shows song select and plays songs with music
and hit sounds at 60 fps (`docs/performance.md`); scores and settings are saved on the
SD card. The Dock is untested.

## Building

| Part | How | Output |
|---|---|---|
| Firmware | `make pocket` (RISC-V GCC; `brew install riscv64-elf-gcc` or `apt install gcc-riscv64-unknown-elf`) | `build/pocket/taikopocket.bin` |
| Bitstream | Quartus Prime Lite 21.1: `cd gateware/projects && quartus_sh --flow compile openFPGA-RISC-V_pocket` (Windows or Linux; no macOS version) | `gateware/projects/output_files/*.rbf` |
| Release | `python3 tools/make_release.py --rbf FILE.rbf` | `build/release/` and `build/release.zip` |

The GitHub workflow `.github/workflows/pocket.yml` does all three in the cloud and
uploads `TaikoPocket-release`.

## Installing

1. Copy the contents of the release folder to the root of the SD card
   (`Cores/`, `Platforms/`, `Assets/`).
2. Pack your songs (`docs/packer.md`) and copy `library.tpl` to
   `/Assets/taikopocket/common/`. A library over 4 GB needs an exFAT card; with
   `--volume-size 4000m` it is split into `library.tpl`, `library.tp1`, ... instead.
3. Start "Taiko Pocket" from the openFPGA menu.

APF limits that bite: the bitstream filename in `core.json` may be at most 15
characters (a longer one gives "can't find bitstream"), data slot names 15, data
slot filenames 31. `make_release.py` checks the first.

## What the first boots showed

| Seen on hardware | Cause | Fix |
|---|---|---|
| "can't find bitstream" | bitstream filename over APF's 15 characters | `taiko.rbf_r` |
| Black and white lines before the loading bar on a cold boot, the last picture of the previous session on a warm one | the core sends no frames until the firmware has set up the video timing, and the scaler shows what is left in its buffer. Blanking the picture (first attempt) changed nothing. | the core sends black frames of its own from power-on (`core_top.sv`); to be confirmed |
| "Loading library" cut off at the bottom | the built-in font's cell had no room for descenders | font regenerated |
| 5 fps after Retry, pause menu closing by itself | the scene was set up again on every frame after a retry | explicit retry flag, covered by `tests/game` |
| Slow with notes on screen, on presses, at Go-Go, in song select | measured: 0.1 us per copied pixel, 0.7 us per blended one | see `docs/performance.md`; to be re-measured |
| Buttons sometimes register twice | contact bounce | debounce in the gateware, adjustable in the settings |
| Still 33 to 58 fps in dense sections after the software changes | drawing notes row by row costs the CPU 5 to 8 ms | the lane is drawn by the FPGA (sprite layer); needs the bitstream that has it |

## Memory map (64 MB SDRAM)

| Address | Use |
|---|---|
| `0x4000_0000` | program and its static memory (22 MB of 30), stack at the top |
| `0x41E0_0000`, `0x41E8_0000` | two 400x360 RGB565 frame buffers |
| `0x4200_0000` | music ring, 32 MB (11 minutes of ADPCM stereo; longer songs keep streaming) |

## First boot checklist

Things that could not be checked without hardware, in the order they would show up:

1. **Does Quartus meet timing?** Yes: 69.9 MHz for the 57.12 MHz system clock.
2. **Picture.** 400x360 at 60 Hz with an 11.424 MHz pixel clock, set by the firmware
   through the video timing registers. No picture: check `video.json` and the
   timing values in `platform_pocket.c`.
3. **Boot.** The loading bar should appear, then song select. "No library file":
   the data slot or file name is wrong.
4. **Reads past 4 GB** use bridge command `0x0181`, which this core sends but nobody
   has run. If songs far into a large library fail to load, repack with
   `--volume-size 4000m`.
5. **Tearing** at the top of the screen: the frame buffer base is switched at
   vertical sync; if the DMA has already fetched ahead, move the switch earlier.
6. **Frame time.** Press Select during a song for the overlay; put the numbers in
   `docs/performance.md`.
7. **Saves.** Data slot 9, `/Assets/taikopocket/common/taikopocket.sav` (settings,
   scores, statistics; 49 kB). The firmware creates the file with bridge command
   `0x0192` (create + resize) and writes it with `0x0184` after a song, when leaving
   the settings and when leaving a song. If creating or writing fails, saving is
   off until the next start and the settings screen says so. Unconfirmed on
   hardware: openfpga-litex's notes (2023) call SD writes unreliable. If the file
   cannot be created, copy any file of at least 49,248 bytes there under that name.
8. **Latency.** Run the calibration, and compare with a recording of the speaker.
