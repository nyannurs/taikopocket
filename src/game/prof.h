/* Frame rate and a small profiler: where each frame's time goes and how many
 * pixels the renderer touched. Costs a clock read per section, so it is always on. */
#ifndef TP_PROF_H
#define TP_PROF_H

#include <stdbool.h>
#include <stdint.h>

enum {
    PROF_INPUT,  /* input collection and scene switching */
    PROF_LOGIC,  /* scene update: streaming, judge, timelines */
    PROF_REPAIR, /* gfx_begin: restoring what was drawn two frames ago */
    PROF_HUD,    /* gauge, score, counters, text */
    PROF_NOTES,  /* bar lines and notes */
    PROF_FX,     /* flashes, flame, burst, judgment text */
    PROF_WAIT,   /* outside app_frame: waiting for vertical sync, platform work */
    PROF_SECTIONS
};

enum { PROF_PX_COPY, PROF_PX_BLEND, PROF_PX_REPAIR, PROF_COUNTERS };

extern uint32_t prof_counter[PROF_COUNTERS]; /* gfx adds to these directly */

void prof_frame_start(void); /* first thing in a frame */
void prof_mark(int section); /* time from here on belongs to `section` */

/* everything except PROF_WAIT, for the frame before this one: how much time is to spare */
uint32_t prof_last_frame_us(void);

/* The frame rate box and the detail overlay are drawn into the background layer, and
 * only when their numbers change (once per window) or the layer was redrawn: text
 * drawn every frame cost the Pocket more than the game it was measuring.
 * Call once per frame after the scene's update. */
extern bool prof_detail; /* the overlay is wanted (toggled by the game scene) */
void prof_show(bool fps, bool top_left);

#endif
