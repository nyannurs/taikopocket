/* Result screen: the totals of the last play. A / START: back to song select,
 * X: play the same chart again. */
#include <stdio.h>

#include "game.h"

#define BOARD_W 192
#define BOARD_H 176
#define BOARD_X ((GFX_W - BOARD_W) / 2)
#define BOARD_Y 104

static bool loading;

static void row(const char *label, int value, int y, uint16_t color) {
    char buf[16];
    snprintf(buf, sizeof buf, "%d", value);
    if (!gfx_sprite_frames(SPR_RESULT_BOARD)) {
        gfx_text(FONT_S, BOARD_X + 20, y, label, RGB(200, 200, 200), ALIGN_LEFT, 0);
    }
    gfx_text(FONT_S, BOARD_X + 170, y, buf, color, ALIGN_RIGHT, 0);
}

static void draw_screen(void) {
    const result_t *r = &last_result;
    gfx_target(GFX_BG);
    gfx_clip_reset();
    int bg = r->cleared ? SPR_RESULT_BG_CLEAR : SPR_RESULT_BG_FAIL;
    if (gfx_sprite_frames(bg)) {
        gfx_sprite(bg, 0, 0, 0);
    } else {
        gfx_fill(0, 0, GFX_W, GFX_H, r->cleared ? RGB(40, 36, 16) : RGB(16, 16, 32));
    }
    gfx_text_outline(FONT_L, GFX_W / 2, 30, lib_string(song.info->title), -1, RGB(255, 255, 255), RGB(0, 0, 0),
                     ALIGN_CENTER, GFX_W - 24);
    gfx_text_outline(FONT_S, GFX_W / 2, 54, lib_string(song.info->subtitle), -1, RGB(190, 190, 200), RGB(0, 0, 0),
                     ALIGN_CENTER, GFX_W - 24);

    if (gfx_sprite_frames(SPR_RESULT_BOARD)) {
        gfx_sprite(SPR_RESULT_BOARD, 0, BOARD_X, BOARD_Y);
    } else {
        ui_panel(BOARD_X, BOARD_Y, BOARD_W, BOARD_H, RGB(0, 0, 0), 24);
        gfx_text(FONT_S, BOARD_X + 8, BOARD_Y + 4, "Score", RGB(200, 200, 200), ALIGN_LEFT, 0);
    }
    /* positions follow the Taikojiro result board: score box, gauge, then five rows */
    gfx_number(SPR_NUM_M, (int)r->score, BOARD_X + 180, BOARD_Y + 8, ALIGN_RIGHT, 0);
    int msg = r->cleared ? 0 : 1;
    if (gfx_sprite_frames(SPR_RESULT_MSG)) {
        gfx_sprite(SPR_RESULT_MSG, msg, BOARD_X + (BOARD_W - gfx_sprite_w(SPR_RESULT_MSG)) / 2, BOARD_Y + 32);
    } else {
        gfx_text(FONT_L, BOARD_X + BOARD_W / 2, BOARD_Y + 32, r->cleared ? "CLEARED" : "FAILED",
                 r->cleared ? RGB(255, 200, 60) : RGB(100, 140, 255), ALIGN_CENTER, 0);
    }
    row("Max combo", r->max_combo, BOARD_Y + 62, RGB(255, 255, 255));
    row("Good", r->good, BOARD_Y + 84, RGB(255, 220, 60));
    row("OK", r->ok, BOARD_Y + 102, RGB(255, 255, 255));
    row("Bad", r->bad, BOARD_Y + 120, RGB(120, 150, 255));
    row("Drumroll", r->rolls, BOARD_Y + 138, RGB(255, 255, 255));
    int total = r->good + r->ok + r->bad;
    row("Rate", total ? (r->good * 100 + r->ok * 50) / total : 0, BOARD_Y + 160, RGB(255, 255, 255));

    int my = BOARD_Y + BOARD_H + 6;
    char buf[48];
    snprintf(buf, sizeof buf, "Soul gauge %d%%", r->gauge_percent);
    gfx_text_outline(FONT_S, GFX_W / 2, my, buf, -1, r->rainbow ? RGB(255, 220, 60) : RGB(220, 220, 220), RGB(0, 0, 0),
                     ALIGN_CENTER, 0);
    if (r->full_combo) {
        if (gfx_sprite_frames(SPR_MARK_FULLCOMBO)) {
            gfx_sprite(SPR_MARK_FULLCOMBO, 0, BOARD_X + BOARD_W + 6, BOARD_Y + 58);
        }
        gfx_text_outline(FONT_S, BOARD_X + BOARD_W + 6, BOARD_Y + 80, r->all_good ? "All GOOD" : "Full combo", -1,
                         RGB(255, 220, 60), RGB(0, 0, 0), ALIGN_LEFT, 0);
    }
    if (r->new_record) {
        if (gfx_sprite_frames(SPR_MARK_NEWRECORD)) {
            gfx_sprite(SPR_MARK_NEWRECORD, 0, BOARD_X + BOARD_W - 70, BOARD_Y - 20);
        } else {
            gfx_text(FONT_S, BOARD_X + BOARD_W, BOARD_Y - 16, "New record", RGB(255, 120, 120), ALIGN_RIGHT, 0);
        }
    }
    if (r->auto_play) {
        gfx_text_outline(FONT_S, 8, 8, "AUTO PLAY (not saved)", -1, RGB(255, 96, 96), RGB(0, 0, 0), ALIGN_LEFT, 0);
    }
    gfx_text_outline(FONT_S, GFX_W / 2, GFX_H - 24, loading ? "Loading..." : "A: song select    X: play again", -1,
                     RGB(200, 200, 210), RGB(0, 0, 0), ALIGN_CENTER, 0);
    gfx_target(GFX_FRAME);
    gfx_bg_changed();
}

static void result_enter(void) {
    loading = false;
    draw_screen();
}

static void result_update(void) {
    if (loading) {
        song_state st = song_poll();
        if (st == SONG_READY) {
            app_goto(&scene_game);
        } else if (st == SONG_ERROR) {
            app_goto(&scene_select);
        }
        return;
    }
    stream_poll(); /* let the music play out */
    if (in.pressed & PF_BTN_X) {
        song_load(song.index, song.course);
        loading = true;
        draw_screen();
    } else if (in.pressed & (PF_BTN_A | PF_BTN_START | PF_BTN_B | PF_KEY_DON_L | PF_KEY_DON_R)) {
        ui_sound_ok();
        app_goto(&scene_select);
    }
}

static void result_draw(void) {}

const scene scene_result = {result_enter, result_update, result_draw};
