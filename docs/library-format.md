# Library file format (version 1)

One file (`.tpl`) holds everything the core needs: song list, charts, audio, glyph
atlas, hit sounds and skin. Written by `tools/packer`, read by `src/game/library.c`.
All integers are little endian. Changing anything here means bumping `version`.

## Overall layout

```
0            header (512 bytes)
             boot sections, each aligned to 16 bytes:
               categories, songs (TOC), strings, fonts, sounds, sprites, skin settings
boot_bytes   end of the boot region
             chart blobs (UTF-8 TJA text), each aligned to 4096 bytes
             audio blobs, each aligned to 4096 bytes
```

The core reads `[0, boot_bytes)` once at boot. A song's chart and audio are read on
demand with the offsets in its TOC entry, which is what lets one data slot serve a
whole library.

### Volumes

Offsets are 64-bit positions in one address space. With `volume_bytes = 0` that space
is a single file. Otherwise it is cut into files of at most `volume_bytes`:
position `p` is byte `p % volume_bytes` of volume `p / volume_bytes`, named
`name.tpl`, `name.tp1`, `name.tp2`, ... (at most 8, the core's data slots; `volume_bytes`
is a multiple of 4096 and larger than the boot region). The boot region is always in
volume 0 and no blob crosses a volume boundary. This exists for FAT32 cards and for
cores that can only do 32-bit bridge reads.

## Header

| Offset | Type | Field |
|---|---|---|
| 0 | char[8] | magic `TAIKOLIB` |
| 8 | u32 | version (1) |
| 12 | u32 | header size (512) |
| 16 | u64 | total size of the address space |
| 24 | u64 | volume_bytes (0 = one file) |
| 32 | u32 | boot_bytes |
| 36 | u32 | song count |
| 40 | u32 | category count |
| 44 | u32 | flags (0) |
| 48 | u32 | build id: CRC-32 of the boot region with this field zero |
| 64 | 8 x 16 bytes | section table: `{u32 offset, u32 bytes, u32 count, u32 0}` |

Sections, by index: 0 categories, 1 songs, 2 strings, 3 fonts, 4 sounds, 5 sprites,
6 skin settings, 7 unused.

## Categories (24 bytes each)

`{u32 name, u32 first_song, u32 song_count, u32 color, u32 text_color, u32 0}`.
Colours are `0xRRGGBB`. Songs are grouped by category, so a category is a range of
the song table.

## Songs (64 bytes each)

| Offset | Type | Field |
|---|---|---|
| 0 | u32 | title (offset into strings) |
| 4 | u32 | subtitle |
| 8 | u16 | category index |
| 10 | u8 | audio format: 0 none, 1 PCM s16, 2 IMA ADPCM |
| 11 | u8 | audio channels |
| 12 | u8[5] | level of Easy, Normal, Hard, Oni, Edit; 255 = no such course |
| 17 | u8 | branch mask (bit n: course n has branches) |
| 18 | u8 | flags (bit 0: `SUBTITLE:++`, show in full) |
| 20 | u32 | chart size in bytes |
| 24 | u64 | chart offset |
| 32 | u64 | audio offset |
| 40 | u32 | audio size in bytes |
| 44 | u32 | audio length in samples (48 kHz) |
| 48 | f32 | `DEMOSTART` in seconds |
| 52 | u32 | CRC-32 of the chart text (identifies the chart in save data) |
| 56 | u16 | audio block size in bytes (PCM: bytes per frame) |
| 58 | u16 | samples per audio block (PCM: 1) |

## Strings

UTF-8, NUL-terminated, referenced by byte offset. Offset 0 is the empty string.

## Fonts

```
u32 font_count                       (2: large, small)
font   {u16 px, u16 line_height, i16 ascent, i16 descent,
        u32 glyph_count, u32 glyphs_offset, u32 bitmaps_offset, u32 0}
glyph  {u32 codepoint, u32 bitmap_offset, u8 w, u8 h, i8 xoff, i8 yoff, u8 advance, u8[3] 0}
```

Glyphs are sorted by codepoint. Offsets are relative to the section (`bitmap_offset`
to the font's `bitmaps_offset`). Bitmaps are 4-bit coverage, rows padded to bytes,
high nibble first. `yoff` is measured down from the top of the line.

## Sounds

`u32 count`, then `{u32 rate, u32 samples, u32 data_offset, u32 0}` per sound, then
mono s16 data. Ids are the `SOUND()` entries of `src/game/assets.def`.

## Sprites

`u32 count`, then `{u16 w, u16 h, u16 frames, u16 0, u32 frame_table}` per sprite
(ids: `SPRITE()` in `assets.def`; `frames = 0` means the skin has no such image).
`frame_table` is `frames` u32 offsets to frame data. A frame is `h` u32 row offsets
(bytes from the start of the frame) followed by the rows. A row is a stream of u16
commands `(kind << 14) | count`:

| kind | meaning |
|---|---|
| 0 | skip `count` transparent pixels |
| 1 | `count` opaque RGB565 pixels follow |
| 2 | `count` blended pixels follow as pairs (RGB565, alpha 1..31) |
| 3 | end of row |

Opaque runs are copied, only edge pixels are blended, which keeps the software
blitter fast.

## Skin settings

`u32 count`, then `count` i32 values, ids from `CFG()` in `assets.def`.

## Audio blobs

- **PCM**: interleaved s16.
- **IMA ADPCM**: WAV block layout without the WAV header. Each block is, per channel,
  `{s16 first_sample, u8 step_index, u8 0}`, then groups of 4 bytes per channel
  (8 samples each, low nibble first). 2041 samples per block: 1024 bytes mono,
  2048 bytes stereo. Blocks decode independently, so seeking is a matter of
  jumping to `sample / 2041`.
