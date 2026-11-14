/* Settings: scroll speed, volumes, the two timing offsets, calibration, button
 * mapping, auto-play, modifiers. Up/Down pick a line, Left/Right change it,
 * A runs an action, B or SELECT saves and leaves.
 *
 * Drawn like song select: the backdrop with the empty line panels is blended once
 * and kept, and only the lines that changed are copied back and written again. */
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "overlay.h"

enum { IT_SPEED, IT_MUSIC, IT_HITS, IT_AUDIO_OFFSET, IT_INPUT_OFFSET, IT_CALIBRATE, IT_BUTTONS, IT_CUSTOM,
       IT_DEBOUNCE, IT_AUTO, IT_INVISIBLE, IT_INVERSE, IT_RANDOM, IT_SORT, IT_FPS, IT_STATS, IT_COUNT };
#define ROW_H 16
#define LIST_Y 32

static int cursor;
static bool redraw;
static uint16_t base[GFX_W * GFX_H]; /* backdrop, title, line panels, help panel */
static bool base_ready;
#define PIECE_HELP (1u << IT_COUNT)
#define PIECE_ALL ((2u << IT_COUNT) - 1)
#define HELP_Y (GFX_H - 70)
static uint32_t stale;  /* lines (bit per item) and the help panel to draw again */
static int shown_cursor; /* line that has the cursor frame on screen */
static bool shown_capture;
static int capture = -1; /* custom mapping: which drum is being assigned, -1 = not capturing */
static uint32_t capture_mask;

static const uint32_t presets[2][DRUM_KEYS] = {
    /* A: thumbs. Don on the inner buttons, ka on the outer ones and the shoulders. */
    {PF_BTN_DOWN | PF_BTN_RIGHT, PF_BTN_Y | PF_BTN_B, PF_BTN_UP | PF_BTN_LEFT | PF_BTN_L1,
     PF_BTN_X | PF_BTN_A | PF_BTN_R1},
    /* B: don on every pad and face button, ka on the shoulders and triggers. */
    {PF_BTN_UP | PF_BTN_DOWN | PF_BTN_LEFT | PF_BTN_RIGHT, PF_BTN_A | PF_BTN_B | PF_BTN_X | PF_BTN_Y,
     PF_BTN_L1 | PF_BTN_L2, PF_BTN_R1 | PF_BTN_R2},
};
static const uint32_t keyboard_keys[DRUM_KEYS] = {PF_KEY_DON_L, PF_KEY_DON_R, PF_KEY_KA_L, PF_KEY_KA_R};
static const char *const drum_names[DRUM_KEYS] = {"DON left", "DON right", "KA left", "KA right"};

static int current_preset(void) {
    for (int p = 0; p < 2; p++) {
        bool same = true;
        for (int k = 0; k < DRUM_KEYS; k++) {
            same = same && (settings.map[k] & 0xFFFF) == presets[p][k];
        }
        if (same) {
            return p;
        }
    }
    return 2;
}

static void set_preset(int p) {
    for (int k = 0; k < DRUM_KEYS; k++) {
        settings.map[k] = presets[p][k] | keyboard_keys[k];
    }
}

static void mask_names(uint32_t mask, char *out, size_t cap) {
    static const char *const names[16] = {"Up", "Down", "Left", "Right", "A", "B", "X", "Y",
                                          "L1", "R1", "L2", "R2", "L3", "R3", "Select", "Start"};
    out[0] = 0;
    for (int b = 0; b < 16; b++) {
        if (mask >> b & 1) {
            size_t n = strlen(out);
            snprintf(out + n, cap - n, "%s%s", n ? " " : "", names[b]);
        }
    }
    if (!out[0]) {
        snprintf(out, cap, "-");
    }
}

static void value_text(int item, char *out, size_t cap) {
    switch (item) {
    case IT_SPEED: snprintf(out, cap, "x%d.%d", settings.speed_x10 / 10, settings.speed_x10 % 10); break;
    case IT_MUSIC: snprintf(out, cap, "%d%%", settings.music_volume); break;
    case IT_HITS: snprintf(out, cap, "%d%%", settings.hit_volume); break;
    case IT_AUDIO_OFFSET: snprintf(out, cap, "%d ms", settings.audio_offset_ms); break;
    case IT_INPUT_OFFSET: snprintf(out, cap, "%d ms", settings.input_offset_ms); break;
    case IT_CALIBRATE: snprintf(out, cap, "press A"); break;
    case IT_BUTTONS: {
        static const char *const names[3] = {"Type A", "Type B", "Custom"};
        snprintf(out, cap, "%s", names[current_preset()]);
        break;
    }
    case IT_CUSTOM: snprintf(out, cap, "press A"); break;
    case IT_DEBOUNCE:
        if (settings.debounce_ms) {
            snprintf(out, cap, "%d ms", settings.debounce_ms);
        } else {
            snprintf(out, cap, "Off");
        }
        break;
    case IT_SORT: snprintf(out, cap, "%s", settings.song_order == SORT_DIFFICULTY ? "By difficulty" : "By title"); break;
    case IT_INVISIBLE: snprintf(out, cap, "%s", settings.mod_invisible ? "On" : "Off"); break;
    case IT_INVERSE: snprintf(out, cap, "%s", settings.mod_inverse ? "On" : "Off"); break;
    case IT_RANDOM: {
        static const char *const names[3] = {"Off", "Kimagure", "Detarame"};
        snprintf(out, cap, "%s", names[settings.mod_random < 3 ? settings.mod_random : 0]);
        break;
    }
    case IT_FPS: snprintf(out, cap, "%s", settings.show_fps ? "On" : "Off"); break;
    case IT_STATS: snprintf(out, cap, "press A"); break;
    case IT_AUTO: snprintf(out, cap, "%s", settings.auto_play ? "On" : "Off"); break;
    default: out[0] = 0; break;
    }
}

static const char *label_of(int item, bool help) {
    static const char *const labels[IT_COUNT] = {
        "Scroll speed", "Music volume", "Hit sound volume", "Audio offset", "Input offset",
        "Measure the input offset", "Buttons", "Set custom buttons", "Button debounce", "Auto-play",
        "Invisible notes (Doron)", "Reverse (Abekobe)", "Random", "Song order", "Frame rate counter", "Statistics"};
    static const char *const help_text[IT_COUNT] = {
        "How fast the notes move. The timing does not change.",
        "", "",
        "Raise it if notes reach the drum before you hear the beat (TV, Dock).",
        "Raise it if your hits are judged late. Calibration sets this.",
        "Tap along with a click to measure your input offset.",
        "A: don inside, ka outside. B: don everywhere, ka on the shoulders.",
        "Choose the buttons for each of the four drum zones.",
        "A release counts once the button has been up this long. Raise it if one press gives two hits.",
        "The game plays by itself. Scores are not saved.",
        "The notes are not drawn. Bar lines and note names are.",
        "Don and ka are swapped.",
        "Kimagure swaps don and ka for a fifth of the notes, Detarame for two fifths. Different every play.",
        "By difficulty uses the level of the course you played last.",
        "Frames per second in a corner of every screen.",
        "Total drum hits and time played."};
    return help ? help_text[item] : labels[item];
}

static void restore(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        gfx_bg_write(x, y + j, w, 1, base + (y + j) * GFX_W + x);
    }
}

static void compose_base(void) {
    gfx_target(GFX_BG);
    gfx_clip_reset();
    if (gfx_sprite_frames(SPR_SONGSELECT_BG)) {
        gfx_sprite(SPR_SONGSELECT_BG, 0, 0, 0);
    } else {
        gfx_fill(0, 0, GFX_W, GFX_H, RGB(16, 18, 28));
    }
    gfx_text(FONT_L, 12, 6, "Settings", RGB(255, 255, 255), ALIGN_LEFT, 0);
    for (int i = 0; i < IT_COUNT; i++) {
        gfx_blend(8, LIST_Y + i * ROW_H, GFX_W - 16, ROW_H - 1, RGB(0, 0, 0), 16);
    }
    gfx_blend(0, HELP_Y, GFX_W, GFX_H - HELP_Y, RGB(0, 0, 0), 22);
    gfx_target(GFX_FRAME);
    gfx_bg_read(0, 0, GFX_W, GFX_H, base);
    base_ready = true;
}

static void draw_line(int i) {
    char buf[96];
    int y = LIST_Y + i * ROW_H;
    bool on = i == cursor;
    restore(0, y, GFX_W, ROW_H);
    if (on) {
        gfx_fill(8, y, GFX_W - 16, ROW_H - 1, RGB(40, 40, 56));
        gfx_frame_rect(8, y, GFX_W - 16, ROW_H - 1, RGB(255, 220, 80));
    }
    gfx_text(FONT_S, 16, y + 2, label_of(i, false), on ? RGB(255, 255, 255) : RGB(190, 190, 190), ALIGN_LEFT, 0);
    value_text(i, buf, sizeof buf);
    gfx_text(FONT_S, GFX_W - 16, y + 2, buf, RGB(255, 220, 120), ALIGN_RIGHT, 0);
    gfx_bg_changed_rect(0, y, GFX_W, ROW_H);
}

static void draw_help(void) {
    char buf[96];
    restore(0, HELP_Y, GFX_W, GFX_H - HELP_Y);
    if (cursor == IT_BUTTONS || cursor == IT_CUSTOM) {
        for (int k = 0; k < DRUM_KEYS; k++) {
            char names[64];
            mask_names(settings.map[k] & 0xFFFF, names, sizeof names);
            snprintf(buf, sizeof buf, "%s: %s", drum_names[k], names);
            gfx_text(FONT_S, 10 + (k % 2) * 195, HELP_Y + 4 + (k / 2) * 15, buf, RGB(150, 170, 220), ALIGN_LEFT, 185);
        }
        ui_text_wrap(FONT_S, 10, HELP_Y + 36, GFX_W - 80, 15, label_of(cursor, true), RGB(200, 200, 210), ALIGN_LEFT);
    } else {
        ui_text_wrap(FONT_S, 10, HELP_Y + 6, GFX_W - 20, 15, label_of(cursor, true), RGB(200, 200, 210), ALIGN_LEFT);
    }
    gfx_text(FONT_S, ui_corner_x(), GFX_H - 18, "B: back", RGB(150, 150, 160), ALIGN_RIGHT, 0);
    gfx_bg_changed_rect(0, HELP_Y, GFX_W, GFX_H - HELP_Y);
    gfx_bg_touched(); /* the frame rate box sits in this corner */
}

/* the panel for choosing custom buttons, over the lines */
static void draw_capture(void) {
    char buf[96];
    ui_panel(30, 90, GFX_W - 60, 150, RGB(0, 0, 0), 28);
    snprintf(buf, sizeof buf, "Buttons for %s", drum_names[capture]);
    gfx_text(FONT_L, GFX_W / 2, 104, buf, RGB(255, 220, 80), ALIGN_CENTER, 0);
    gfx_text(FONT_S, GFX_W / 2, 134, "Press each button you want, then START.", RGB(220, 220, 220), ALIGN_CENTER, 0);
    mask_names(capture_mask, buf, sizeof buf);
    gfx_text(FONT_L, GFX_W / 2, 164, buf, RGB(255, 255, 255), ALIGN_CENTER, GFX_W - 80);
    gfx_text(FONT_S, GFX_W / 2, 206, "SELECT: cancel", RGB(150, 150, 160), ALIGN_CENTER, 0);
    gfx_bg_changed_rect(30, 90, GFX_W - 60, 150);
}

/* draws what changed: a few lines per frame, so opening the screen does not stall */
static void draw_screen(void) {
    if (redraw) { /* something changed: the line that had the cursor, the one that has it now, the help */
        stale |= 1u << shown_cursor | 1u << cursor | PIECE_HELP;
        if (capture >= 0 || shown_capture) {
            stale = PIECE_ALL; /* the panel covers lines: put them back under it first */
        }
        redraw = false;
    }
    if (!stale) {
        return;
    }
    gfx_target(GFX_BG);
    gfx_clip_reset();
    int lines = 0;
    for (int i = 0; i < IT_COUNT && lines < 5; i++) {
        if (stale >> i & 1) {
            draw_line(i);
            stale &= ~(1u << i);
            lines++;
        }
    }
    if (lines < 5 && (stale & PIECE_HELP)) {
        draw_help();
        stale &= ~PIECE_HELP;
    }
    if (!stale) {
        shown_cursor = cursor;
        shown_capture = capture >= 0;
        if (capture >= 0) {
            draw_capture();
        }
    }
    gfx_target(GFX_FRAME);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void change(int dir) {
    switch (cursor) {
    case IT_SPEED: settings.speed_x10 = (uint8_t)clampi(settings.speed_x10 + dir, 5, 40); break;
    case IT_MUSIC: settings.music_volume = (uint8_t)clampi(settings.music_volume + dir * 5, 0, 100); break;
    case IT_HITS:
        settings.hit_volume = (uint8_t)clampi(settings.hit_volume + dir * 5, 0, 100);
        settings_apply_audio();
        ui_sound_ok();
        break;
    case IT_AUDIO_OFFSET: settings.audio_offset_ms = (int16_t)clampi(settings.audio_offset_ms + dir, -300, 500); break;
    case IT_INPUT_OFFSET: settings.input_offset_ms = (int16_t)clampi(settings.input_offset_ms + dir, -300, 500); break;
    case IT_BUTTONS: set_preset(clampi(current_preset() + dir, 0, 1)); break;
    case IT_DEBOUNCE: settings.debounce_ms = (uint8_t)clampi(settings.debounce_ms + dir * 2, 0, 40); break;
    case IT_SORT: settings.song_order = settings.song_order == SORT_DIFFICULTY ? SORT_TITLE : SORT_DIFFICULTY; break;
    case IT_INVISIBLE: settings.mod_invisible = !settings.mod_invisible; break;
    case IT_INVERSE: settings.mod_inverse = !settings.mod_inverse; break;
    case IT_RANDOM: settings.mod_random = (uint8_t)((settings.mod_random + 3 + dir) % 3); break;
    case IT_FPS: settings.show_fps = !settings.show_fps; break;
    case IT_AUTO: settings.auto_play = !settings.auto_play; break;
    default: return;
    }
    settings_apply_audio();
}

static void settings_enter(void) {
    capture = -1;
    shown_capture = false;
    shown_cursor = cursor;
    pf_hit_auto(false);
    if (!base_ready) {
        compose_base();
    }
    gfx_target(GFX_BG);
    gfx_clip_reset();
    gfx_bg_write(0, 0, GFX_W, GFX_H, base);
    /* only what is not as it should be: scores that cannot be saved, a bitstream older than the game */
    const char *status = !pf_save_supported()                          ? "Scores cannot be saved"
                         : !overlay_on()                               ? "No sprite layer: old bitstream"
                         : pf_ovl_version() < PF_OVL_VERSION           ? "Old bitstream"
                                                                       : "";
    gfx_text(FONT_S, GFX_W - 12, 10, status, RGB(255, 200, 80), ALIGN_RIGHT, 0);
    gfx_target(GFX_FRAME);
    gfx_bg_changed();
    stale = PIECE_ALL;
    redraw = false;
}

static void settings_update(void) {
    if (capture >= 0) {
        uint32_t add = in.pressed & 0x3FFF; /* every pad button except Select and Start */
        if (add) {
            capture_mask |= add;
            redraw = true;
        }
        if (in.pressed & PF_BTN_SELECT) {
            capture = -1;
            redraw = true;
        } else if (in.pressed & PF_BTN_START) {
            static uint32_t pending[DRUM_KEYS];
            pending[capture] = capture_mask;
            capture_mask = 0;
            if (++capture >= DRUM_KEYS) {
                for (int k = 0; k < DRUM_KEYS; k++) {
                    settings.map[k] = pending[k] | keyboard_keys[k];
                }
                settings_apply_audio();
                capture = -1;
            }
            redraw = true;
        }
    } else {
        if (in.repeat & PF_BTN_DOWN) {
            cursor = (cursor + 1) % IT_COUNT;
            ui_sound_move();
            redraw = true;
        } else if (in.repeat & PF_BTN_UP) {
            cursor = (cursor + IT_COUNT - 1) % IT_COUNT;
            ui_sound_move();
            redraw = true;
        } else if (in.repeat & (PF_BTN_LEFT | PF_BTN_RIGHT)) {
            change((in.repeat & PF_BTN_RIGHT) ? 1 : -1);
            redraw = true;
        } else if (in.pressed & PF_BTN_A) {
            if (cursor == IT_CALIBRATE) {
                app_goto(&scene_calibrate);
            } else if (cursor == IT_STATS) {
                app_goto(&scene_stats);
            } else if (cursor == IT_CUSTOM) {
                capture = 0;
                capture_mask = 0;
            } else {
                change(1);
            }
            redraw = true;
        } else if (in.pressed & (PF_BTN_B | PF_BTN_SELECT)) {
            settings_save();
            app_goto(&scene_select);
        }
    }
    draw_screen();
}

static void settings_draw(void) {}

const scene scene_settings = {settings_enter, settings_update, settings_draw};
