/* Calibration: measures how late the player's presses arrive relative to what
 * they hear, and stores it as the input offset.
 *
 * A click track is written straight into the music ring as PCM, so each click
 * sits at an exactly known sample position; taps carry the hardware timestamp
 * of the button edge. The difference needs no frame timing at all. */
#include <stdio.h>
#include <string.h>

#include "game.h"

#define BEAT_SAMPLES 24000 /* 120 BPM */
#define BEATS 40
#define WARMUP 4 /* taps on the first beats are not counted */
#define MAX_TAPS 64

static int taps[MAX_TAPS]; /* tap minus the nearest click, in samples */
static int tap_count;
static int result_ms;
static bool have_result;
static int64_t last_pos;

static int measured_ms(void) {
    if (tap_count == 0) {
        return 0;
    }
    /* median first, then the mean of the taps within 80 ms of it */
    int sorted[MAX_TAPS];
    memcpy(sorted, taps, (size_t)tap_count * sizeof *taps);
    for (int i = 1; i < tap_count; i++) {
        int v = sorted[i], j = i;
        for (; j > 0 && sorted[j - 1] > v; j--) {
            sorted[j] = sorted[j - 1];
        }
        sorted[j] = v;
    }
    int median = sorted[tap_count / 2];
    long sum = 0;
    int n = 0;
    for (int i = 0; i < tap_count; i++) {
        if (taps[i] - median < 3840 && median - taps[i] < 3840) {
            sum += taps[i];
            n++;
        }
    }
    return (int)(sum / (n ? n : 1) * 1000 / PF_SAMPLE_RATE);
}

static void start_clicks(void) {
    uint32_t capacity, n = 0, rate = PF_SAMPLE_RATE;
    int16_t *ring = pf_music_ring(&capacity);
    const int16_t *click = assets_sound(SND_KA, &n, &rate);
    uint32_t total = BEATS * BEAT_SAMPLES;
    if (total * 2 > capacity) {
        total = capacity / 2;
    }
    memset(ring, 0, total * 2);
    for (uint32_t beat = 0; click && beat * BEAT_SAMPLES < total; beat++) {
        uint32_t len = n < BEAT_SAMPLES ? n : BEAT_SAMPLES;
        memcpy(ring + beat * BEAT_SAMPLES, click, len * 2);
    }
    pf_music_fmt fmt = {PF_CODEC_PCM16, 1, 2, 1};
    pf_music_begin(&fmt, capacity / 2 * 2, 0, -BEAT_SAMPLES, 256);
    pf_music_feed(total * 2);
    pf_music_finish();
    pf_music_run(true);
}

static void calibrate_enter(void) {
    preview_stop();
    pf_hit_auto(false);
    pf_volume(256, 0);
    tap_count = 0;
    have_result = false;
    /* a preview read that is still on its way would land in the ring, on top of the clicks */
    for (uint64_t t0 = pf_time_us(); !stream_idle() && pf_time_us() - t0 < 500000;) {
    }
    start_clicks();

    gfx_target(GFX_BG);
    gfx_clip_reset();
    gfx_fill(0, 0, GFX_W, GFX_H, RGB(14, 16, 26));
    gfx_text(FONT_L, GFX_W / 2, 20, "Input offset", RGB(255, 255, 255), ALIGN_CENTER, 0);
    gfx_text(FONT_S, GFX_W / 2, 56, "Tap a drum button on every click.", RGB(210, 210, 220), ALIGN_CENTER, 0);
    gfx_text(FONT_S, GFX_W / 2, 74, "Keep going until the number settles.", RGB(210, 210, 220), ALIGN_CENTER, 0);
    gfx_text(FONT_S, GFX_W / 2, GFX_H - 44, "START: use this value", RGB(170, 170, 180), ALIGN_CENTER, 0);
    gfx_text(FONT_S, GFX_W / 2, GFX_H - 26, "SELECT: cancel", RGB(170, 170, 180), ALIGN_CENTER, 0);
    gfx_target(GFX_FRAME);
    gfx_bg_changed();
}

static void leave(void) {
    pf_music_stop();
    settings_apply_audio();
    app_goto(&scene_settings);
}

static void calibrate_update(void) {
    int64_t counter, pos;
    pf_audio_clock(&counter, &pos);
    last_pos = pos;
    uint32_t drums = settings.map[0] | settings.map[1] | settings.map[2] | settings.map[3];
    for (int i = 0; i < in.event_count; i++) {
        const pf_input_event *e = &in.events[i];
        if (!e->pressed || !(e->button & drums)) {
            continue;
        }
        int64_t tap = pos + (e->counter - counter);
        int64_t beat = (tap + BEAT_SAMPLES / 2) / BEAT_SAMPLES;
        if (tap < 0 || beat < WARMUP || beat >= BEATS) {
            continue;
        }
        if (tap_count == MAX_TAPS) {
            memmove(taps, taps + 1, (MAX_TAPS - 1) * sizeof *taps);
            tap_count--;
        }
        taps[tap_count++] = (int)(tap - beat * BEAT_SAMPLES);
        result_ms = measured_ms();
        have_result = tap_count >= 4;
    }
    if (pos >= (int64_t)BEATS * BEAT_SAMPLES) {
        start_clicks(); /* the track ran out: play it again, keeping the taps */
    }
    if (in.pressed & PF_BTN_START) {
        if (have_result) {
            settings.input_offset_ms = (int16_t)result_ms;
            settings_save();
        }
        leave();
    } else if (in.pressed & PF_BTN_SELECT) {
        leave();
    }
}

static void calibrate_draw(void) {
    /* a dot that lights up on every click, drawn from the audio position */
    int64_t phase = last_pos >= 0 ? last_pos % BEAT_SAMPLES : BEAT_SAMPLES;
    bool lit = phase < BEAT_SAMPLES / 6;
    gfx_fill(GFX_W / 2 - 20, 110, 40, 40, lit ? RGB(255, 96, 48) : RGB(60, 60, 70));
    char buf[48];
    if (have_result) {
        snprintf(buf, sizeof buf, "%+d ms", result_ms);
        gfx_text(FONT_L, GFX_W / 2, 180, buf, RGB(255, 220, 80), ALIGN_CENTER, 0);
    } else {
        gfx_text(FONT_L, GFX_W / 2, 180, "...", RGB(120, 120, 130), ALIGN_CENTER, 0);
    }
    snprintf(buf, sizeof buf, "%d taps    current setting %d ms", tap_count, settings.input_offset_ms);
    gfx_text(FONT_S, GFX_W / 2, 214, buf, RGB(170, 170, 180), ALIGN_CENTER, 0);
}

const scene scene_calibrate = {calibrate_enter, calibrate_update, calibrate_draw};
