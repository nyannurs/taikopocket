/* The game's use of the platform's sprite layer (pf_ovl_*): everything that moves in
 * the lane is a sprite there instead of pixels the CPU draws every frame. At boot the
 * pictures are copied out of the library's sprites into the layer's sheet, each group
 * with its own 31-colour palette. If the platform has no sprite layer, or a skin's
 * pictures do not fit the sheet, overlay_on() is false and the game draws them itself. */
#ifndef TP_OVERLAY_H
#define TP_OVERLAY_H

#include <stdbool.h>

void overlay_init(void); /* after gfx_init() */
bool overlay_on(void);

/* Cell for a frame of a library sprite, or -1 if it is not on the sheet. */
int overlay_cell(int sprite, int frame);
/* 1 x 48 bar line cells: plain, and the yellow one that starts a branch section. */
extern int overlay_bar, overlay_bar_branch;
/* Pieces of the drumroll bars (small, big) narrower than a tile: 24, 12, 6 and 3 px. */
#define OVERLAY_ROLL_PIECES 4
extern int overlay_roll_piece[2][OVERLAY_ROLL_PIECES];

/* A frame's sprites, added back to front like drawing. `dim` shows it at half strength
 * (the layer has no per-sprite alpha; fades are two steps). */
void overlay_begin(void);
void overlay_add(int cell, int x, int y, bool dim);
void overlay_end(void);
void overlay_clear(void); /* no sprites, for scenes that have none */

#endif
