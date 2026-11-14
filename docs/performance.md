# Measurements

## Measured

| What | Result | Where |
|---|---|---|
| Parser vs YataiDON | 848 charts, 1072 courses, 757,715 notes and bar lines identical | `make golden` |
| Judge vs YataiDON | 6,432 simulated plays, 3,863,809 judgments, 1,608 branch decisions identical | `make golden` |
| Gateware vs software audio engine | 183 checks, all output samples and sprite pixels identical | `make -C gateware/taiko sim` |
| Hit sound: button edge to sample at the I2S serialiser | under one sample period (19 us for an edge 20 cycles into a period) | test bench |
| Parse time, largest real chart (1,880 lines, 5,864 notes and bar lines) | about 0.4 ms on an Apple M-series core | desktop log |
| Frame time (update + draw), desktop | 50 us average | desktop log |
| Game clock vs audio clock, browser build | within 5 ms over 40 s | page probe |
| ADPCM against PCM | 38 dB signal-to-noise | ffmpeg decode comparison |
| Firmware size | 59 kB code, 19 MB static memory | `make pocket` |
| FPGA use (Quartus 21.1, 5CEBA4) | 11,203 of 18,480 ALMs (61 %), 231 of 308 M10K blocks (75 %), 21 of 66 DSP blocks | Actions run, fit summary |
| Fmax, slow 0 C corner | system clock 69.9 MHz (needs 57.12), audio 68.2 MHz (needs 12.288), video 80.1 MHz (needs 11.424), bridge 117 MHz (needs 74.25); worst setup slack +0.106 ns, no failing paths | Actions run, timing report |
| Cloud build | 18 minutes, of which Quartus 14 | Actions run |
| Packing 16 songs (37 minutes) | 1.1 s, 102 MB (ADPCM stereo) | packer log |

## Frame rate on the Pocket

Measured on a Pocket with the overlay (Select during a song), 2026-11-14, while
mashing buttons, firmware of the second round:

| Song | fps | frame | repair | hud | notes | fx | pixels: copied / blended / repaired |
|---|---|---|---|---|---|---|---|
| Dstorv (Oni 13) | 30 | 20.6 ms | 5.0 ms | 9.0 ms | 3.5 ms | 2.8 ms | 23,166 / 3,256 / 53,238 |
| elegante (Oni 8) | 40 | 16.0 ms | 4.2 ms | 9.0 ms | 1.3 ms | 1.3 ms | 18,084 / 1,740 / 44,046 |

What these say about the machine (57 MHz soft CPU, 4 kB direct-mapped caches, SDRAM
behind a write-through cache):

- **Copying a pixel costs about 0.1 us, blending one about 0.7 us.** A frame is
  16.7 ms, so it can touch roughly 100,000 pixels, or far fewer if they are blended.
  It does not matter whether a pixel ends up visible.
- Code that runs per character or per row is expensive too: the overlay's own four
  lines of text and the gauge, both redrawn every frame, were most of the 9 ms "hud".

What was changed, in order of effect:

- `memcpy` in `libc_min.c` copies words at every alignment (it used a byte loop for
  three sprite rows out of four).
- Nothing is redrawn that has not changed. Gauge and soul, the frame rate box and the
  overlay are drawn on the background layer when their content changes. Song select
  is drawn in pieces from a kept backdrop; moving the cursor redraws only the cursor.
- Lane flashes are prepared lane pictures (`gfx_bg_strip()`), made one per frame in
  spare time, never on a press. 12,000 blended pixels per frame became none.
- Notes that pile up are drawn without the part the note on top hides
  (`gfx_sprite_behind()`).
- The Go-Go splash (six soft images, 40,000 blended pixels, about 28 ms per frame) is
  left out when the CPU would have to draw it (`pf_blend_budget()`); on the sprite
  layer it costs nothing.
- 64-bit divisions are a software loop on this CPU: `snprintf` and the clock no
  longer use them for numbers that fit 32 bits.

Third round, same day, Dstorv on auto-play with those changes in:

| fps | frame | repair | hud | notes | fx | pixels: copied / blended / repaired |
|---|---|---|---|---|---|---|
| 33 to 58 | 14.0 to 17.5 ms, worst 21 to 25 | 3.7 to 3.9 ms | 1.8 to 2.4 ms | 5.1 to 8.0 ms | 2.0 to 2.7 ms | about 10,500 / 3,400 / 38,000 |

The notes took 5 to 8 ms for some 10,000 pixels, six times the 0.1 us per pixel of
long copies: a note is 48 short rows, and every row costs a cache refill of the
frame buffer lines it touches plus the per-row code. Drawing sprites with the CPU
cannot be made cheap enough on this SoC. So:

## The sprite layer

Everything that moves in the lane is no longer drawn by the CPU. Notes, bar lines,
note names, the hit burst, the inner hit effect, the judgment text and the kusudama
counter are sprites of a hardware layer (`gateware/taiko/rtl/taiko_sprites.sv`) that
the FPGA mixes into the picture on its way to the screen. The CPU writes a list of
at most 128 positions per frame.

Synthesis with the layer, version 3 (Quartus 21.1): 12,128 of 18,480 ALMs (66 %), 284 of
308 M10K blocks (92 %), 25 DSP blocks; worst setup slack +0.531 ns, worst hold slack
+0.068 ns. Met. (Version 2: 12,056 ALMs, +0.085 ns and +0.017 ns; the slack moves
with the placement, not with the 72 ALMs.) All its memories are block RAM.

Measured on the Pocket with the layer (a dense level 10 chart, auto-play, overlay on): **60.0 fps, frame 4.5 ms, worst 11.8 ms**; repair 0.6 ms, hud 2.4 ms,
notes 0.6 ms; pixels 2,122 copied, 422 blended, 7,345 repaired. Before the layer the
same kind of section took 14 to 17.5 ms.

Still drawn by the CPU and measured in the same session: the pause menu, redrawn every
frame, 12.8 ms (now drawn once, on the background layer); the Go-Go splash,
a visible dip (now on the sprite layer); song select while a
direction is held, 8 ms on average with frames of 28 ms (now one row, the header or
the footer per frame).

Pixel work left for the CPU in the same dense sections (desktop count, auto-play):
about 6,000 repaired, 2,000 copied, 350 blended per frame, down from 38,000 / 10,500 /
3,400.

The frame rate counter (settings, on by default) and the overlay are in
`src/game/prof.c`. The result line in the log also names the heaviest frame of a
song in pixel units (blended pixels counted seven times).

## Not measured yet (needs the Pocket)

- Parse time and frame time on the 57 MHz CPU. Estimate for the parser: the desktop
  figure times roughly 300 to 600 for a soft RISC-V with SDRAM, so 0.1 to 0.25 s
  for the largest chart; the brief asks for well under a second.
- Frame time per section with notes on screen (overlay numbers, see above).
- Bridge read speed (library boot of 2.4 MB, song load, streaming headroom).
- End-to-end latency through speaker, headphones and the Dock.
