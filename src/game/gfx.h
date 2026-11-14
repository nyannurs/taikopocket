/* Software renderer for the 400x360 RGB565 screen.
 *
 * Two targets: the background layer (GFX_BG), composed once by a scene and kept
 * in memory, and the frame (GFX_FRAME), the platform's back buffer. Every frame,
 * gfx_begin() repairs the parts of the back buffer that were drawn on two frames
 * ago from the background layer, then the scene draws only what moves. That is
 * what keeps the Pocket's 50 MHz CPU at 60 fps: a frame costs a few small
 * copies and the sprites that are actually on screen. */
#ifndef TP_GFX_H
#define TP_GFX_H

#include <stdbool.h>
#include <stdint.h>

#include "../platform/platform.h"

#define GFX_W PF_SCREEN_W
#define GFX_H PF_SCREEN_H
#define RGB(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define RGB24(c) RGB(((c) >> 16) & 255, ((c) >> 8) & 255, (c) & 255)

enum { GFX_FRAME = 0, GFX_BG = 1 };
enum { FONT_L = 0, FONT_S = 1, FONT_BUILTIN = 2 };
enum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

void gfx_init(void);    /* after the library is loaded: sprite and font tables */
void gfx_begin(void);   /* start of a frame */
void gfx_target(int t); /* GFX_FRAME or GFX_BG */
void gfx_bg_changed(void); /* the background layer was redrawn: repaint both buffers */
void gfx_bg_touched(void); /* part of the layer was redrawn (and reported with gfx_bg_changed_rect): boxes kept on it must be put back */
uint32_t gfx_bg_generation(void); /* counts gfx_bg_changed() calls: tells a fresh background layer from a kept one */
void gfx_bg_changed_rect(int x, int y, int w, int h); /* only this part of it was */
/* Shows `pixels` (w * h, row after row) in place of that rectangle of the background
 * layer until it is changed or cleared with NULL. For parts of the background that
 * switch between a few prepared states, such as the lane with a flash on it: the
 * switch costs one copy instead of blending every frame. The pixels must stay valid. */
void gfx_bg_strip(int x, int y, int w, int h, const uint16_t *pixels);
void gfx_bg_read(int x, int y, int w, int h, uint16_t *out);      /* copy out of the background layer */
void gfx_bg_write(int x, int y, int w, int h, const uint16_t *in); /* and back in; no repaint */
void gfx_clip(int x, int y, int w, int h);
void gfx_clip_reset(void);

void gfx_fill(int x, int y, int w, int h, uint16_t color);
void gfx_blend(int x, int y, int w, int h, uint16_t color, int alpha); /* alpha 0..32 */
void gfx_frame_rect(int x, int y, int w, int h, uint16_t color);

/* Sprites (ids: SPR_* in assets.h). A missing sprite draws nothing. */
int gfx_sprite_frames(int id);
int gfx_sprite_w(int id);
int gfx_sprite_h(int id);
void gfx_sprite(int id, int frame, int x, int y);
void gfx_sprite_alpha(int id, int frame, int x, int y, int alpha); /* alpha 0..32 */
void gfx_sprite_c(int id, int frame, int cx, int cy);              /* centred */

/* A frame as plain pixels, w * h of each: the colour, and how opaque it is (0..32).
 * False if there is no such frame. For building the sprite layer's sheet. */
bool gfx_sprite_decode(int id, int frame, uint16_t *colour, uint8_t *alpha);

/* Columns of the frame that have any pixel: [*x0, *x1). False for an empty frame. */
bool gfx_sprite_extent(int id, int frame, int *x0, int *x1);

/* For notes that pile up. gfx_sprite_spans() gives, per row of a frame, its longest
 * fully opaque stretch [a[row], b[row]) (equal when the row has none); returns the
 * number of rows written. gfx_sprite_behind() draws a frame without the pixels that a
 * sprite drawn later at `cover_x`, on the same rows, will hide anyway: `a`/`b` are
 * that sprite's spans. On the Pocket a pixel costs the same whether it ends up
 * visible or not, so a stack of a hundred notes must not be drawn a hundred times. */
int gfx_sprite_spans(int id, int frame, uint8_t *a, uint8_t *b, int cap);
void gfx_sprite_behind(int id, int frame, int x, int y, int cover_x, const uint8_t *a, const uint8_t *b, int rows);

/* Stretched vertically by `extra` pixels, growing upwards from its bottom edge. */
void gfx_sprite_stretch(int id, int frame, int x, int y, int extra, int alpha);

/* Number with a digit sprite (frames 0..9). Returns the width drawn. */
int gfx_number(int id, int value, int x, int y, int align, int spacing);
int gfx_number_fx(int id, int value, int x, int y, int align, int spacing, int stretch, int alpha);

/* UTF-8 text. `max_w` > 0 cuts the text with an ellipsis. Returns the width. */
int gfx_text(int font, int x, int y, const char *s, uint16_t color, int align, int max_w);
int gfx_text_n(int font, int x, int y, const char *s, int bytes, uint16_t color, int align, int max_w);
int gfx_text_width(int font, const char *s, int bytes);
int gfx_font_height(int font);
/* text with a 1 px dark outline, for text over busy backgrounds */
int gfx_text_outline(int font, int x, int y, const char *s, int bytes, uint16_t color, uint16_t outline,
                     int align, int max_w);

#endif
