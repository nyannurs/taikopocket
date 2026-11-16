/* Software model of the gateware's sprite layer (gateware/taiko/rtl/taiko_sprites.sv).
 * The desktop build shows the game's sprites through it and the gateware test bench
 * compares the hardware with it pixel for pixel. See src/platform/platform.h for what
 * the layer is; this file only fixes its arithmetic. */
#ifndef TP_SOFTOVL_H
#define TP_SOFTOVL_H

#include <stdbool.h>
#include <stdint.h>

#define OVL_W 400
#define OVL_H 360

void ovl_reset(void);
/* Sheet pixels are palette indices: 0 is transparent, OVL_ROW_END ends the row of a
 * cell (nothing right of it is drawn, and the pixels after it in that row must be 0). */
#define OVL_ROW_END 31
void ovl_sheet(uint32_t addr, const uint8_t *pixels, uint32_t count);
void ovl_palette(int bank, int index, uint32_t entry); /* (alpha 0..16) << 16 | colour, RGB565 times alpha */
void ovl_cell(int cell, uint32_t addr, int w, int h, bool clip, bool twice); /* twice: every pixel shown 2 x 2 */
void ovl_clip(int x0, int x1);
void ovl_sprites(const uint32_t *list, int count); /* entry 0 is on top */

/* Cycles the hardware has to draw the sprites of one line: a line is 2380 cycles of the
 * system clock, 400 of them clear the line buffer and a few pass at either end. A sprite
 * of the list costs three, a pixel that is looked at one. */
#define OVL_LINE_CYCLES 1975
void ovl_budget(int cycles); /* another limit; 0: none */
int ovl_peak(void);          /* most cycles a line would have needed since the last call */

/* One line of the layer: per pixel a palette entry, 0 where no sprite is. */
void ovl_line(int y, uint32_t out[OVL_W]);
/* The layer's pixel over a picture pixel, both as the video output has them (RGB888). */
uint32_t ovl_mix888(uint32_t entry, uint32_t rgb);
/* The layer over a whole RGB565 frame, in place. */
void ovl_compose(uint16_t *frame);
/* The fade stage after it: every channel times level / 16 (16: unchanged, 0: black). */
void ovl_fade(uint16_t *frame, int level);

/* For tests: the model's whole state to a file and back, and a look at its parts. */
bool ovl_state_save(const char *path);
bool ovl_state_load(const char *path);
int ovl_state_cell(int cell, uint32_t *addr, int *w, int *h, bool *clip, bool *twice); /* returns the sprite count */
const uint8_t *ovl_state_sheet(void);     /* 73728 pixels of it are the hardware's */
const uint32_t *ovl_state_palette(void);  /* 16 x 32 */
const uint32_t *ovl_state_sprites(void);
void ovl_state_clip(int *x0, int *x1);

#endif
