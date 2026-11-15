# Video and rendering

## Resolution: 400 x 360, RGB565

- The Pocket's panel is 1600 x 1440 (10:9). 400 x 360 is the same shape and scales by
  exactly 4, so every pixel is a sharp 4 x 4 block. openFPGA allows up to 800 x 720.
- The CPU is a soft RISC-V at about 50 MHz, which is roughly 830,000 cycles per
  frame at 60 fps. 400 x 360 is 144,000 pixels (288 kB); 800 x 720 would be four
  times that and could not even be cleared in one frame.
- Taikojiro skins are drawn for 512 x 384 with 48 px notes. At 400 x 360 they are
  used pixel for pixel: notes keep their size, full-width images are cropped (lane
  layers on the right so their labels stay, flashes on the left, backgrounds
  centred or bottom-right depending on where their label is).
- The lane is 56 px high, the judge circle sits at x = 80 and a 4/4 bar at scroll 1.0
  spans the 320 px to the right edge, the same proportion YataiDON uses (one bar per
  lane length). Sixteenth notes overlap by about a third, as in Taikojiro's default.

The layout constants are at the top of `src/game/scene_game.c`.

## How a frame is drawn

`src/game/gfx.c` has two targets:

- the **background layer**, a 400 x 360 image in memory that a scene composes once
  (background picture, lane, drum, title, all text of the menus);
- the **frame**, the platform's back buffer (double buffered, flipped at vsync).

Each frame, `gfx_begin()` copies back from the background layer only the rectangles
that were drawn on in that buffer two frames earlier, then the scene draws what
moves. Drawing calls record their own rectangles; nearby ones are merged.

Consequences:

- Menus cost nothing per frame. They are redrawn into the background layer only
  when the selection changes.
- Gameplay repairs the lane strip and a few HUD areas (about 30,000 pixels) and
  draws the notes on screen.
- Sprites are stored run-length encoded with opaque runs copied by `memcpy` and
  only edge pixels blended (see `docs/library-format.md`).
- Text is 4-bit coverage glyphs from the library's atlas, blended per pixel. It is
  only ever drawn into the background layer during gameplay, except the lyric line.

## What is drawn when

The rule on the Pocket: draw a thing when it changes, not every frame.

- **Sprite layer, every frame, no pixel work for the CPU:** notes, bar lines, note
  names, the burst and the inner hit effect, the judgment text, the kusudama counter.
- **Background layer, on change:** gauge and soul (a judged note), the frame rate box
  and the overlay (twice a second), the lines of song select and of the settings.
- **Frame, every frame:** drum flashes, score and combo numbers, the Go-Go flame, the
  roll and balloon counters, the cursor of song select.

## Sprite layer

`src/platform/platform.h` describes it (`pf_ovl_*`); `src/game/overlay.c` is the
game's use of it. At boot the pictures are copied from the library's sprites onto
the layer's sheet (65,536 pixels of 5 bits), without their empty margins, each group
with its own palette of 30 colours (more colours are merged, nearest first). The
Taikojiro skin uses 62,482 of 73,728 pixels in 61 cells.

- Sprites are listed front first; the game adds them back to front and
  `overlay_end()` turns the list round.
- The layer draws a line in a fixed time. It does not look at pixels that are drawn
  already, at the empty end of a row (the game marks the end of every row on the
  sheet) or at what is outside the screen or the clip window, so notes that lie on
  each other cost little: `gateware/taiko/README.md`. The first bitstream with the
  layer ("v2" in the header of the settings screen) looked at everything and ran out
  of time at about 35 notes on screen: the notes furthest right and the bar lines
  were then missing, and the last one drawn was cut off. That was reached by 18 of
  682 charts of a test library at scroll speed 1.0 and by 292 at 0.5. With the current
  layer, all 684 songs of it played by auto-play at speed 0.5 (the slowest setting) stay
  under the limit in the model; the busiest line needs 1608 of 1975 cycles. What is
  left is the empty left side of a row: where the narrow top rows of the notes do not
  touch, each note still costs about 25 cycles.
- The layer has no per-sprite alpha. A fade is full, half (a second palette bank
  with half the alpha), gone.
- A translucent sprite pixel is mixed with the frame buffer under it, not with a
  sprite under it: the burst's rays show the lane, not the note, through them. For
  that reason the faint Go-Go splash is the last sprite of the list, behind the notes;
  in front it would cut holes into them.
- The Go-Go splash (48 x 192, eight frames) is kept at half resolution and shown with
  doubled pixels; at full resolution it is taller than a cell and larger than the
  sheet. It is smoke, so it does not show.
- The pause menu is drawn by the CPU, and the layer covers everything the CPU draws.
  The menu is therefore below the lane and the note names, and the lane stays as it
  was when the game was paused: a still picture of the notes, which is also the way
  to look at something that is hard to see in motion.
- A sprite cannot be cut to a width, so a drumroll's bar is whole 48 px tiles and
  then pieces of 24, 12, 6 and 3 px. The one or two pixels those leave are covered by
  one more 3 px piece that ends exactly where the tail's cap begins.
- If the platform has no layer (a bitstream from before it) or a skin's pictures do
  not fit the sheet, `overlay_on()` is false and the game draws the same things into
  the frame, as described under "Piled-up notes". `--set overlay=0` on the desktop.

## Lane flashes

The coloured flash on a drum hit and the yellow one on a judged hit lie under the
notes, on the left part of the lane. Instead of blending them into every frame, the
game keeps prepared pictures of that part of the lane with a flash on it (three
colours and the hit flash, two brightness steps each: 20 pictures per lane) and
tells the renderer to repair the lane from one of them: `gfx_bg_strip()`.

A picture costs the Pocket about 8 ms, so none is ever made because a button was
pressed; a flash whose picture is not ready is shown without it. They are made in
the background, half a picture per frame and only in a quiet moment: the game's own
work in the last frame took under 5 ms, no button was pressed, and no note is within
0.6 s of its hit, before or after. (A frame with a hit in it is the longest kind; a
half picture on top of it ran past the frame and showed as notes that twitched.) First
for the lane as it is, then for the lane on the other side of Go-Go Time, whose
plain picture is painted and kept the same way. That work is normally done within
the lead-in of a song. When Go-Go Time starts, the lane is replaced by one copy of
its stored picture and its flashes are already there; nothing is prepared for the
next four frames.

## Piled-up notes

Notes are drawn back to front. When the next note to be drawn (the one that ends up
on top) overlaps the current one, the current one is drawn without the columns the
other one's opaque part will cover, row by row (`gfx_sprite_spans()`,
`gfx_sprite_behind()`). Note names closer than 8 px to the last one drawn are skipped
there, since a pile of names would be blended pixel by pixel.

On the sprite layer every note has its name. Names of notes that lie close together
overlap, the earlier note's on top, as the notes themselves do; nothing is left out, so
a name does not come and go when one note overtakes another. The names are the first
sprites of the frame's list: if a list is ever too long, they are what is dropped.

## Song select

Two levels: the folders (the library's categories), and one page of a folder's songs
at a time (six rows). A opens a folder, B closes it. The backdrop with the empty
row panels and the footer panel is blended once and kept; a row, the header or the
footer is repainted by copying its part back and drawing its text, at most two rows
per frame. Header and footer follow the cursor at once, one of them per frame. A
folder's songs are listed by title or by difficulty (settings: "Song order";
difficulty is the level of the course played last).
The settings screen is drawn the same way, line by line.

The window with the best play of the song under the cursor (after 45 frames of rest,
over the row below it, or above it on the last row) is drawn into the background
layer when it changes. Its fade from one course to the next is the same window drawn
in four steps of darker colours, three frames apart: a fill and some forty glyphs, no
blending. When the cursor moves, the row under it is drawn again.

## Frame rate counter and profiler

`src/game/prof.c`. The counter (settings: "Frame rate counter", on by default) sits
top left during a song and bottom right elsewhere. Select during a song opens the
overlay; see `docs/performance.md`.

## Timing

Note positions are computed from the song's chart time, which is the audio sample
position reported by the platform (`song_now_ms()`), not from a frame counter. A
late frame draws the notes further along; it cannot put them out of step with
the music. Effects (flashes, judgment text) are timed in chart time too, so they
freeze while paused.

A note's position is a whole number of pixels: its place on an imagined tape and the
tape's position are rounded down separately and subtracted (`note_x()`). Notes that
move at the same speed then step by the same number of pixels in every frame, and a
drumroll's bar keeps its length. Rounding each note's distance to the drum by itself
made neighbours move against each other by a pixel.

## Not drawn

- Vertical note movement from complex `#SCROLL` values. The notes still load and
  are judged at YataiDON's times; they move horizontally only.
- `#JPOSSCROLL` (judge circle moves). Parsed, not shown.
- Dancers, mob, player characters, 2P images.

## Measurements

See `docs/performance.md`.
