#include "prof.h"

#include <stdio.h>
#include <string.h>

#include "gfx.h"

#define WINDOW 30 /* frames per average */

uint32_t prof_counter[PROF_COUNTERS];

static uint64_t mark_us, window_start_us;
static int section = PROF_WAIT;
static uint32_t acc[PROF_SECTIONS], shown[PROF_SECTIONS];
static uint32_t acc_count[PROF_COUNTERS], shown_count[PROF_COUNTERS];
static uint32_t frame_us, frame_max, shown_frame, shown_max, last_frame_us;
static int frames, fps_x10;
static int window_id;

static void account(uint64_t t) {
    uint32_t d = (uint32_t)(t - mark_us);
    acc[section] += d;
    if (section != PROF_WAIT) {
        frame_us += d;
    }
    mark_us = t;
}

void prof_mark(int s) {
    account(pf_time_us());
    section = s;
}

void prof_frame_start(void) {
    uint64_t t = pf_time_us();
    if (!window_start_us) {
        window_start_us = mark_us = t;
    }
    account(t); /* the wait since the last frame ended */
    section = PROF_INPUT;
    if (frame_us > frame_max) {
        frame_max = frame_us;
    }
    last_frame_us = frame_us;
    frame_us = 0;
    for (int i = 0; i < PROF_COUNTERS; i++) {
        acc_count[i] += prof_counter[i];
        prof_counter[i] = 0;
    }
    if (++frames >= WINDOW) {
        uint32_t span = (uint32_t)(t - window_start_us);
        fps_x10 = span ? (int)((uint64_t)frames * 10000000u / span) : 0;
        shown_frame = 0;
        for (int i = 0; i < PROF_SECTIONS; i++) {
            shown[i] = acc[i] / (uint32_t)frames;
            if (i != PROF_WAIT) {
                shown_frame += shown[i];
            }
            acc[i] = 0;
        }
        for (int i = 0; i < PROF_COUNTERS; i++) {
            shown_count[i] = acc_count[i] / (uint32_t)frames;
            acc_count[i] = 0;
        }
        shown_max = frame_max;
        frame_max = 0;
        frames = 0;
        window_start_us = t;
        window_id++;
    }
}

uint32_t prof_last_frame_us(void) { return last_frame_us; }

/* ---- display ---- */

/* a box drawn over the background layer, with what was under it kept for taking it away */
typedef struct {
    int x, y, w, h;
    bool visible;
    uint32_t generation; /* background layer it was last drawn on */
    int window;          /* numbers it shows */
    uint16_t *under;
} box;

#define FPS_W 30
#define FPS_H 13
#define DETAIL_W 268
#define DETAIL_H 50
static uint16_t fps_under[FPS_W * FPS_H], detail_under[DETAIL_W * DETAIL_H];
static box fps_box = {0, 0, FPS_W, FPS_H, false, 0, 0, fps_under};
static box detail_box = {0, 0, DETAIL_W, DETAIL_H, false, 0, 0, detail_under};
bool prof_detail;

/* tenths of a millisecond as "12.3" */
static int ms_text(char *out, size_t cap, uint32_t us) {
    return snprintf(out, cap, "%u.%u", (unsigned)(us / 1000), (unsigned)(us % 1000 / 100));
}

static void draw_fps(const box *b) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d.%d", fps_x10 / 10, fps_x10 % 10);
    gfx_fill(b->x, b->y, b->w, b->h, RGB(0, 0, 0));
    gfx_text(FONT_BUILTIN, b->x + b->w - 2, b->y + 1, buf, fps_x10 >= 595 ? RGB(0, 255, 0) : RGB(255, 200, 0),
             ALIGN_RIGHT, 0);
}

static void draw_detail(const box *b) {
    static const char *const names[PROF_SECTIONS] = {"in", "logic", "repair", "hud", "notes", "fx", "wait"};
    char line[96], num[16];
    int h = gfx_font_height(FONT_BUILTIN) + 1, x = b->x, y = b->y;
    gfx_fill(x, y, b->w, b->h, RGB(0, 0, 0));

    int n = snprintf(line, sizeof line, "frame ");
    n += ms_text(line + n, sizeof line - (size_t)n, shown_frame);
    n += snprintf(line + n, sizeof line - (size_t)n, " ms  max ");
    n += ms_text(line + n, sizeof line - (size_t)n, shown_max);
    gfx_text(FONT_BUILTIN, x + 2, y + 1, line, RGB(0, 255, 0), ALIGN_LEFT, 0);

    for (int row = 0; row < 2; row++) { /* in, logic, repair / hud, notes, fx, wait */
        int first = row ? 3 : 0, last = row ? PROF_SECTIONS : 3;
        n = 0;
        line[0] = 0;
        for (int i = first; i < last; i++) {
            ms_text(num, sizeof num, shown[i]);
            n += snprintf(line + n, sizeof line - (size_t)n, "%s%s %s", i > first ? "  " : "", names[i], num);
        }
        gfx_text(FONT_BUILTIN, x + 2, y + 1 + (row + 1) * h, line, RGB(0, 255, 0), ALIGN_LEFT, 0);
    }

    snprintf(line, sizeof line, "px copy %u  blend %u  repair %u", (unsigned)shown_count[PROF_PX_COPY],
             (unsigned)shown_count[PROF_PX_BLEND], (unsigned)shown_count[PROF_PX_REPAIR]);
    gfx_text(FONT_BUILTIN, x + 2, y + 1 + 3 * h, line, RGB(0, 255, 0), ALIGN_LEFT, 0);
}

/* shows, refreshes, moves or removes one box */
static void update_box(box *b, bool want, int x, int y, void (*draw)(const box *)) {
    uint32_t generation = gfx_bg_generation();
    bool fresh = generation != b->generation; /* the scene redrew the layer: the box is gone from it */
    if (b->visible && !fresh && (!want || x != b->x || y != b->y)) {
        gfx_bg_write(b->x, b->y, b->w, b->h, b->under);
        gfx_bg_changed_rect(b->x, b->y, b->w, b->h);
        b->visible = false;
    }
    if (fresh) {
        b->visible = false;
    }
    b->generation = generation;
    if (!want || (b->visible && b->window == window_id)) {
        return;
    }
    if (!b->visible) {
        b->x = x;
        b->y = y;
        gfx_bg_read(x, y, b->w, b->h, b->under);
    }
    gfx_target(GFX_BG);
    gfx_clip(b->x, b->y, b->w, b->h);
    draw(b);
    gfx_clip_reset();
    gfx_target(GFX_FRAME);
    gfx_bg_changed_rect(b->x, b->y, b->w, b->h);
    b->visible = true;
    b->window = window_id;
}

void prof_show(bool fps, bool top_left) {
    update_box(&fps_box, fps, top_left ? 0 : GFX_W - FPS_W, top_left ? 0 : GFX_H - FPS_H, draw_fps);
    update_box(&detail_box, prof_detail, 0, GFX_H - DETAIL_H, draw_detail);
}
