/* Statistics: lifetime drum hits and time played. Reached from the settings;
 * B, A or SELECT goes back. The two clocks are drawn again once a second. */
#include <stdio.h>

#include "game.h"

static uint32_t shown_s = 0xFFFFFFFFu;

static void duration(char *out, size_t cap, uint32_t s) {
    snprintf(out, cap, "%u h %02u min %02u s", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}

#define ROW_W (GFX_W - 16)
#define ROW_H 25
static uint16_t clock_under[2][ROW_W * ROW_H]; /* the two rows that change, as they are without their text */

static void row_text(int y, const char *label, const char *value) {
    gfx_text(FONT_S, 16, y + 6, label, RGB(200, 200, 200), ALIGN_LEFT, 0);
    gfx_text(FONT_L, GFX_W - 16, y + 3, value, RGB(255, 220, 120), ALIGN_RIGHT, 0);
}

static void row(int y, const char *label, const char *value) {
    gfx_blend(8, y, ROW_W, ROW_H, RGB(0, 0, 0), 16);
    row_text(y, label, value);
}

#define CLOCKS_Y (44 + 4 * 28)

/* the two times, on their rows as they were before any text; every second */
static void draw_clocks(void) {
    char buf[48];
    gfx_target(GFX_BG);
    gfx_clip_reset();
    for (int k = 0; k < 2; k++) {
        int y = CLOCKS_Y + k * 28;
        gfx_bg_write(8, y, ROW_W, ROW_H, clock_under[k]);
        duration(buf, sizeof buf, k ? stats.active_s : stats.play_s);
        row_text(y, k ? "Time running" : "Time in songs", buf);
        gfx_bg_changed_rect(8, y, ROW_W, ROW_H);
    }
    gfx_target(GFX_FRAME);
}

static void draw_screen(void) {
    char buf[48];
    gfx_target(GFX_BG);
    gfx_clip_reset();
    if (gfx_sprite_frames(SPR_SONGSELECT_BG)) {
        gfx_sprite(SPR_SONGSELECT_BG, 0, 0, 0);
    } else {
        gfx_fill(0, 0, GFX_W, GFX_H, RGB(16, 18, 28));
    }
    gfx_text(FONT_L, 12, 8, "Statistics", RGB(255, 255, 255), ALIGN_LEFT, 0);

    int y = 44;
    snprintf(buf, sizeof buf, "%u", (unsigned)(stats.don_hits + stats.ka_hits));
    row(y, "Drum hits", buf);
    snprintf(buf, sizeof buf, "%u", (unsigned)stats.don_hits);
    row(y += 28, "Don", buf);
    snprintf(buf, sizeof buf, "%u", (unsigned)stats.ka_hits);
    row(y += 28, "Ka", buf);
    snprintf(buf, sizeof buf, "%u", (unsigned)stats.songs_finished);
    row(y += 28, "Songs finished", buf);
    for (int k = 0; k < 2; k++) { /* the clocks' rows: the panel now, kept, the text from draw_clocks() */
        gfx_blend(8, CLOCKS_Y + k * 28, ROW_W, ROW_H, RGB(0, 0, 0), 16);
        gfx_bg_read(8, CLOCKS_Y + k * 28, ROW_W, ROW_H, clock_under[k]);
    }

    ui_text_wrap(FONT_S, 10, GFX_H - 62, GFX_W - 20, 16,
                 pf_save_supported() ? "Hits made by auto-play are not counted. Saved with the scores."
                                     : "Hits made by auto-play are not counted. This device cannot save, so "
                                       "the counters start again every time.",
                 RGB(200, 200, 210), ALIGN_LEFT);
    gfx_text(FONT_S, ui_corner_x(), GFX_H - 18, "B: back", RGB(150, 150, 160), ALIGN_RIGHT, 0);
    gfx_target(GFX_FRAME);
    gfx_bg_changed();
}

static void stats_enter(void) {
    draw_screen();
    shown_s = 0xFFFFFFFFu;
}

static void stats_update(void) {
    if (in.pressed & (PF_BTN_A | PF_BTN_B | PF_BTN_SELECT)) {
        app_goto(&scene_settings);
        return;
    }
    if (stats.active_s != shown_s) {
        shown_s = stats.active_s;
        draw_clocks();
    }
}

static void stats_draw(void) {}

const scene scene_stats = {stats_enter, stats_update, stats_draw};
