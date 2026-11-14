# Taiko gateware

Additions to the openfpga-litex SoC for Taiko Pocket (GPL-3.0). The SoC itself is
not regenerated: everything here hangs off ports the stock `litex.v` already has.

| File | What |
|---|---|
| `rtl/taiko_audio.sv` | audio clock domain: music position, four hit-sound voices, mixer, button edge events |
| `rtl/taiko_stream.sv` | reads the song from a ring in SDRAM, decodes PCM / IMA ADPCM, fills the music FIFO |
| `rtl/taiko_top.sv` | register block (Wishbone slave at `0x8000_0000`) and the glue between the two |
| `rtl/taiko_cdc.sv` | clock-domain crossings, small async FIFO, sample RAM |
| `tb/tb_taiko.cpp` | Verilator test bench |

Changes to the base core (`gateware/target/pocket`): `audio.sv` keeps the FIFO, the
48 kHz tick and the I2S serialiser, and takes its samples from `taiko_top`;
`core_top.sv` instantiates `taiko_top`, shares the bus-master port between the APF
bridge and the streamer, and outputs exactly 400 active pixels; `core_bridge_cmd.v`
sends the 48-bit data slot read (`0x0181`) when the offset needs it; the video PLL
output is 11.424 MHz (400x360 at 60 Hz) instead of 5.712 MHz.

## How it works

- **Music.** The game has the APF bridge write file data straight into a 32 MB ring
  in SDRAM and tells the streamer how many bytes are there. The streamer reads
  words over Wishbone, decodes, and pushes stereo samples into the 4096-deep FIFO.
  In the audio domain one sample is popped per 48 kHz tick and the **song position**
  is counted right there. If the FIFO is empty mid-song the position waits; after
  the last sample it keeps counting in silence. The CPU never touches a sample.
- **Hit sounds.** Don and ka samples (up to 16,384 each, any rate up to 48 kHz) sit
  in block RAM. A press edge on a mapped button starts one of four voices in the
  audio domain; the CPU can start them too. With all four busy the oldest restarts.
- **Mixer.** `music * gain * volume + hits * volume`, saturated, every sample.
- **Input.** Each press and release is put in a 64-entry FIFO with the value of the
  free-running output sample counter. The buttons of all four controller ports are
  OR-ed, so Dock controllers work like the Pocket's own.

`src/platform/common/softmix.c` is the software model of all this and what the
desktop build plays through; the test bench requires identical output.

## Sprite layer

`rtl/taiko_sprites.sv`. Why it exists: `docs/performance.md`. What it is:
`src/platform/platform.h`.

- Memories: sheet 73728 x 5 bit (36 M10K), palette 16 x 32 entries of {alpha 0..16,
  colour RGB565 times alpha}, 256 cells {double, clip, h, w, sheet address / 2}, two
  sprite lists of 128 {bank, cell, y, x}. A doubled cell shows every pixel 2 x 2: the
  Go-Go splash is 48 x 192 soft pixels, kept at 24 x 96.
- `TK_OVL_CTRL` reads the layer's version in its top byte (3; the first layer had a
  65536 pixel sheet, cell addresses in pixels and no doubling, and reads 0; version 2
  drew every pixel of every sprite). The firmware handles all of them and no layer at
  all. The CPU writes the memories through `TK_OVL_ADDR` / `TK_OVL_DATA`.
- One line of sprites is drawn into a line buffer two lines before it is displayed;
  two buffers alternate. A line is 2380 system clock cycles: 400 to clear the
  buffer, three per sprite in the list, one per pixel looked at. If a line runs out
  of time, the sprite being drawn is cut off and the ones behind it are left out.
- Version 2 looked at every pixel of every sprite on the line: room for about 35
  notes. Notes are only that many when they lie on top of each other, so version 3
  does not look at what cannot show:
  - sheet value 31 ends the row of a cell (the firmware puts it after the last pixel
    of every row; 0 must follow it);
  - `run` is a stretch of the line that is drawn without a gap, the one the last
    drawn pixels belong to. A sprite that reaches it continues at its end, one cycle
    later. Not for doubled cells;
  - a row starts and ends where it can be seen: the screen, or the clip window.

  A note behind another costs about ten cycles instead of fifty. The test bench draws
  128 sprites 2 px apart in 1337 cycles (6724 pixel by pixel) and compares every pixel
  with the model.
- Front to back: a pixel that has been drawn on a line is not drawn again, so the
  first sprite of the list is on top and nothing is read back from the buffer.
- On the way out, each pixel is `sprite + picture * (16 - alpha) / 16`.
- The list in use changes in the vertical blank: the firmware fills the other list
  during the frame and names it in `TK_OVL_CTRL` when it switches frame buffers.
- The test bench runs the video timing the firmware programs, shows random
  sprites, a lane full of notes and a list switch, and compares every output pixel
  with `src/platform/common/softovl.c`, which is also what the desktop build shows.

## Debounce and video enable

- **Debounce** (`taiko_audio.sv`, register `TK_DEBOUNCE` in samples): a press is
  taken the moment it is seen; a release only counts once the button has stayed up
  that long. Chatter and short drop-outs while a button is held cannot make a
  second press, and no press is ever delayed. Hit sounds, the event FIFO and the
  key word all see the debounced buttons.
- **Video enable** (`TK_CTRL` bit 4): until the firmware sets it, `core_top.sv` sends
  black frames of its own with the game's timing (400 x 360 in 476 x 400). Without
  frames the Pocket's scaler shows what is left in its buffer: stripes after power-on,
  the last picture of the previous session otherwise. Blanking the picture alone
  (the first attempt) did not help, because the SoC sends no frames at all until the
  firmware has programmed its timing generator. The game's picture takes over at the
  start of one of its frames.

## Latency of a hit sound

| Stage | Time |
|---|---|
| APF pad polling (`io_pad_controller.v`: every 2^16 cycles of 74.25 MHz, plus the 12-word reply) | up to about 1.2 ms, 0.9 ms of it jitter |
| synchroniser into the audio clock (3 flops, 2 equal samples) | 4 cycles of 12.288 MHz, 0.3 us |
| edge to the sample latched for the serialiser (measured in simulation) | under 1 sample: 20 us at most when the edge is before cycle 200 of 256, else 1 sample more |
| I2S serialiser picks the sample up at the next frame | up to 1 sample, 21 us |
| DAC, amplifier, speaker / HDMI in the Dock | not measured (needs hardware) |

So the gateware adds at most about 3 samples (60 us); the controller polling
dominates. Timestamps have the same 0.9 ms polling granularity.

## Resources

Block RAM: the hit sample memory is 32,768 x 16 bits = 64 M10K blocks (21% of the
device's 308). The stock core reports 31% with its logic analyser, which is now
off. To halve it, set `HIT_ADDR` to 13 (8,192 samples per sound) in `taiko_top`,
`PF_HIT_MAX_SAMPLES` in `src/platform/platform.h` and the slot shift (`slot << 14`) in
`pf_hit_load()` of `platform_pocket.c`.

## Simulation

```sh
make -C gateware/taiko sim     # needs verilator; output in build/gateware
```

Checks (183): hit latency, polyphony, restart, saturation and volume against
softmix; button events with exact timestamps, overflow flag; five streaming cases
(ADPCM stereo/mono, PCM stereo/mono, lead-in, mid-block start, ring wrap, ring
running dry) compared sample by sample and position by position; the CPU-fed path
of the stock core; the frame counter.

Not simulated: the Altera PLLs and `dcfifo`, the SDRAM controller, timing closure.

## Registers

Word index at `0x8000_0000 + 4 * index`; names as in
`src/platform/pocket/taiko_regs.h`, which documents each field.
