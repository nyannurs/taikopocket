/* Application shell: input collection, scene switching, and the boot scene
 * that loads the library's boot region with a progress bar. */
#include <string.h>

#include "game.h"
#include "overlay.h"

#define REPEAT_DELAY 22 /* frames before a held direction repeats */
#define REPEAT_RATE 4

input_state in;
uint32_t app_frame_count;
static const scene *current, *pending;
static int repeat_timer;

void app_goto(const scene *next) { pending = next; }

static void collect_input(void) {
    uint32_t before = in.held;
    in.pressed = 0;
    in.event_count = 0;
    pf_input_event e;
    while (pf_input_next(&e)) {
        if (e.pressed) {
            in.pressed |= e.button;
        }
        if (in.event_count < INPUT_EVENTS) {
            in.events[in.event_count++] = e;
        }
    }
    in.held = pf_buttons();
    in.repeat = in.pressed;
    uint32_t dirs = in.held & (PF_BTN_UP | PF_BTN_DOWN | PF_BTN_LEFT | PF_BTN_RIGHT);
    if (dirs && dirs == (before & (PF_BTN_UP | PF_BTN_DOWN | PF_BTN_LEFT | PF_BTN_RIGHT)) && !in.pressed) {
        if (++repeat_timer >= REPEAT_DELAY && (repeat_timer - REPEAT_DELAY) % REPEAT_RATE == 0) {
            in.repeat |= dirs;
        }
    } else {
        repeat_timer = 0;
    }
}

void ui_panel(int x, int y, int w, int h, uint16_t color, int alpha) {
    gfx_blend(x, y, w, h, color, alpha);
    gfx_frame_rect(x, y, w, h, RGB(160, 160, 160));
}

int ui_text_wrap(int font, int x, int y, int w, int line_h, const char *text, uint16_t color, int align) {
    while (*text) {
        int len = 0, last_space = -1;
        while (text[len] && gfx_text_width(font, text, len + 1) <= w) {
            if (text[len] == ' ') {
                last_space = len;
            }
            len++;
        }
        if (text[len] && last_space > 0) {
            len = last_space; /* break at the last space that fits */
        }
        if (len == 0) {
            len = 1;
        }
        gfx_text_n(font, x, y, text, len, color, align, 0);
        text += len;
        while (*text == ' ') {
            text++;
        }
        y += line_h;
    }
    return y;
}

int ui_corner_x(void) {
    return settings.show_fps ? GFX_W - 10 - gfx_text_width(FONT_BUILTIN, "00.0", -1) - 4 : GFX_W - 10;
}

void ui_sound_move(void) { pf_hit_play(PF_HIT_KA); }
void ui_sound_ok(void) { pf_hit_play(PF_HIT_DON); }

/* Hit sounds live in a small fixed memory on the Pocket (0.34 s at 48 kHz). A sound
 * that is longer is cut there if what is lost is inaudible (under half a percent of
 * its energy: a rim click); otherwise it is stored at half the rate, which keeps
 * twice the length (a drum's tail) for half the bandwidth. What still does not fit
 * is faded out. */
/* A sound of the skin into the platform's hit sound memory. A slot has room for a
 * fraction of a second at the full rate. A sound that is longer is kept at half, a
 * third or a quarter of the rate, whichever is the first at which no more than half a
 * percent of its energy lies beyond the room; what is cut off is faded out. */
static void load_hit_sound(int slot, int sound) {
    static int16_t buf[PF_HIT_MAX_SAMPLES];
    uint32_t n, rate, room = pf_hit_room(slot);
    const int16_t *pcm = assets_sound(sound, &n, &rate);
    if (!pcm || !room) {
        return;
    }
    room = room > PF_HIT_MAX_SAMPLES ? PF_HIT_MAX_SAMPLES : room;
    uint32_t step = 1;
    if (n > room) {
        uint64_t total = 0;
        for (uint32_t i = 0; i < n; i++) {
            total += (uint64_t)((int32_t)pcm[i] * pcm[i]);
        }
        for (; step < 4; step++) {
            uint64_t tail = 0;
            for (uint32_t i = room * step; i < n; i++) {
                tail += (uint64_t)((int32_t)pcm[i] * pcm[i]);
            }
            if (tail * 200 <= total) {
                break;
            }
        }
    }
    uint32_t out = n / step;
    if (out > room) {
        out = room;
    }
    for (uint32_t i = 0; i < out; i++) {
        int v = 0;
        for (uint32_t k = 0; k < step; k++) {
            v += pcm[i * step + k];
        }
        buf[i] = (int16_t)(v / (int)step);
    }
    if (out < n / step) {
        const uint32_t fade = out < 2048 ? out : 2048;
        for (uint32_t i = 0; i < fade; i++) {
            buf[out - 1 - i] = (int16_t)(buf[out - 1 - i] * (int)i / (int)fade);
        }
    }
    pf_hit_load(slot, buf, out, rate / step);
}

/* ---------- boot scene ---------- */

static bool boot_done;

static void boot_enter(void) {
    gfx_target(GFX_BG);
    gfx_fill(0, 0, GFX_W, GFX_H, RGB(12, 12, 20));
    gfx_text(FONT_BUILTIN, GFX_W / 2, 140, "TAIKO POCKET", RGB(255, 255, 255), ALIGN_CENTER, 0);
    gfx_target(GFX_FRAME);
    gfx_bg_changed();
    boot_done = false;
    lib_boot_begin();
}

static void boot_update(void) {
    if (boot_done) {
        return;
    }
    lib_state st = lib_boot_poll();
    if (st == LIB_READY) {
        boot_done = true;
        gfx_init();
        overlay_init();
        settings_load();
        if (pf_option("autoplay")) {
            settings.auto_play = pf_option("autoplay")[0] == '1';
        }
        if (pf_option("sort")) {
            settings.song_order = (uint8_t)(pf_option("sort")[0] - '0');
        }
        if (pf_option("speed")) {
            int tenths = 0; /* 5 is x0.5 */
            for (const char *d = pf_option("speed"); *d >= '0' && *d <= '9'; d++) {
                tenths = tenths * 10 + (*d - '0');
            }
            settings.speed_x10 = (uint8_t)tenths;
        }
        load_hit_sound(PF_HIT_DON, SND_DON);
        load_hit_sound(PF_HIT_KA, SND_KA);
        load_hit_sound(PF_HIT_BALLOON, SND_BALLOON);
        load_hit_sound(PF_HIT_KUSUDAMA, SND_POTATO);
        settings_apply_audio();
        pf_hit_auto(false);
        pf_log("library: %u songs in %u categories", (unsigned)lib_song_count(), (unsigned)lib_category_count());
        app_goto(&scene_select);
    }
}

static void boot_draw(void) {
    if (lib_boot_poll() == LIB_ERROR) {
        ui_text_wrap(FONT_BUILTIN, GFX_W / 2, 190, GFX_W - 40, 16, lib_error(), RGB(255, 200, 80), ALIGN_CENTER);
        return;
    }
    int w = 240, x = (GFX_W - w) / 2;
    gfx_frame_rect(x, 180, w, 10, RGB(200, 200, 200));
    gfx_fill(x + 2, 182, (int)((w - 4) * lib_boot_progress() / 256), 6, RGB(240, 96, 48));
    gfx_text(FONT_BUILTIN, GFX_W / 2, 200, "Loading library", RGB(180, 180, 180), ALIGN_CENTER, 0);
}

const scene scene_boot = {boot_enter, boot_update, boot_draw};

/* ---------- platform entry points ---------- */

void app_init(void) {
    settings_defaults();
    app_goto(&scene_boot);
}

void app_frame(void) {
    prof_frame_start();
    collect_input();
    if (pending) {
        current = pending;
        pending = NULL;
        gfx_clip_reset();
        gfx_bg_strip(0, 0, 0, 0, NULL);
        overlay_clear(); /* a scene that uses the sprite layer fills it every frame */
        current->enter();
    }
    prof_mark(PROF_LOGIC);
    current->update();
    /* the frame rate, out of the way of each screen: top left over the song, bottom right in the menus */
    prof_show(settings.show_fps && current != &scene_boot, current == &scene_game);
    prof_mark(PROF_REPAIR);
    gfx_begin();
    prof_mark(PROF_HUD);
    current->draw();
    stats_tick(current == &scene_game && game_is_playing());
    app_frame_count++;
    prof_mark(PROF_WAIT);
}
