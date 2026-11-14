/* Gameplay. Everything is positioned from the song's chart time, which comes
 * from the audio position (song_now_ms), never from frame counts: a dropped
 * frame moves the notes further, it does not put them out of step. */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "game.h"
#include "overlay.h"

/* ---- layout (400x360) ---- */
#define LANE_Y 148
#define LANE_H 56
#define LANE_CY (LANE_Y + LANE_H / 2)
#define LANE_X 56       /* notes are clipped here, at the drum's right edge */
#define JUDGE_X 80      /* centre of the judge circle */
#define TRAVEL 320.0f   /* pixels a note moves in one 4/4 bar at scroll 1 */
#define NOTE_HALF 24
#define DRUM_X 4
#define DRUM_Y (LANE_CY - 24)
#define GAUGE_X 104
#define GAUGE_Y 88
#define GAUGE_UNITS 16
#define SENOTES_Y (LANE_Y + LANE_H + 2)
#define TITLE_Y (LANE_Y + LANE_H + 24)
#define LYRIC_Y 300

#define VIS_MAX (16384 + 32768)
#define ACT_MAX 384
#define VT_MAX 4096
#define GONE_MAX 65536

result_t last_result;

/* ---- notes waiting to come on screen, and the ones on it ---- */
static const tp_note *vis[VIS_MAX];
static double vis_key[VIS_MAX]; /* time the note comes on screen */
static const tp_note *vis_end[VIS_MAX]; /* end of the list the note belongs to, to find a roll's tail */
static int vis_head, vis_tail;

typedef struct {
    const tp_note *n, *tail;
    double unload;
} active;
static active act[ACT_MAX];
static int act_n;
static uint8_t gone[GONE_MAX]; /* by note index: hit, so no longer drawn */

/* ---- visual timeline: bpm, go-go, lyrics ---- */
static const tp_event *vt[VT_MAX];
static int vt_head, vt_tail;

static double now;
static double cur_bpm;
static bool gogo;
static double gogo_since;
static const char *lyric;
static int lyric_len;
static int branch_shown; /* tp_branch drawn in the lane */
static bool bg_dirty;
static bool paused;
static bool retrying; /* the song is being loaded again from the pause menu */
static int pause_cursor;
static int pause_drawn = -1; /* cursor position the pause menu shows; -1: it is not on screen */
static int auto_side;

/* ---- effects, all timed in chart ms ---- */
/* The motion follows YataiDON's default skin (Graphics/game/animation.json and
 * Scripts/game of PyTaikoGreen); distances are scaled to this screen. */
static struct {
    double judge_t, explode_t, flash_t, hit_flash_t, roll_t, balloon_t, balloon_end_t, branch_t, popup_t;
    double combo_t, score_t;
    double drum_t[4];
    int judge_kind, explode_kind, flash_kind, roll_count, balloon_left, branch_dir, popup_value;
    int last_combo, last_score;
    bool explode_big, kusudama;
} fx;

static uint64_t frame_us_sum;
static uint32_t frame_us_max, frame_count;
static uint32_t peak_notes, peak_px; /* most notes on screen, most pixels drawn and repaired in a frame */
static double peak_ms;              /* when that frame was */

/* ---- lane flashes ----
 * The flashes lie under the notes, on the lane's background, so the part of the lane
 * they cover is prepared once per colour and brightness step and then shown with
 * gfx_bg_strip(). Blending them into every frame cost the Pocket a third of its frame.
 *
 * Preparing a picture takes the Pocket about 8 ms, so it is never done because a button
 * was pressed: a flash whose picture is not ready yet is shown without it. The pictures
 * are made in the background, half a picture per frame and only after a frame that left
 * the time: first for the lane as it is, then for the lane as it will be when Go-Go
 * Time starts or ends. By the time the lane changes, its flashes are ready, and the
 * change itself is one copy of the lane's stored picture. */
#define LANE_W (GFX_W - LANE_X)
#define FLASH_LEVELS 2                    /* brightness steps of a fading flash */
#define FLASH_KEYS (1 + 3 * FLASH_LEVELS) /* none, or one of three colours at a level */
#define FLASH_HITS (1 + FLASH_LEVELS)     /* none, or the hit flash at a level */
#define FLASH_SPARE_US 5000               /* work on a picture only after a frame whose own work was shorter */
#define FLASH_CALM_MS 600                 /* and with no hit this close, before or after */
#define FLASH_PHASES 2                    /* a picture is made in this many frames, top half first */
#define FLASH_QUIET 4                     /* frames without preparing after the lane was changed */
#define FLASH_MAX_W 224
#define LANE_STATES 6                     /* Go-Go or not, times the branch layer: each has its own lane background */
static uint16_t lane_full[LANE_STATES][LANE_W * LANE_H]; /* the lane of each state, without a flash */
static bool lane_full_ready[LANE_STATES];
static uint16_t lane_strips[LANE_STATES][FLASH_KEYS][FLASH_HITS][FLASH_MAX_W * LANE_H];
static uint8_t lane_done[LANE_STATES][FLASH_KEYS][FLASH_HITS]; /* phases made; FLASH_PHASES: ready */
static int lane_state; /* the lane background that is on the layer now */
static int lane_quiet; /* frames since it was put there */
static int flash_w;    /* how far the flash images reach into the lane: the strip's width */
static int flash_want; /* key * FLASH_HITS + hit that was needed and not ready; 0: none */
static int envelope(double since, double in_ms, double out_start, double out_end);

static bool flash_ready(int state, int key, int hit) { return lane_done[state][key][hit] == FLASH_PHASES; }

static void lane_flash_reset(void) {
    static const int ids[4] = {SPR_FLASH_RED, SPR_FLASH_BLUE, SPR_FLASH_YELLOW, SPR_FLASH_HIT};
    /* The prepared pictures show the skin's lane, with the branch layer if the chart
     * branches. They stay good for the next song and for a retry unless that changes;
     * making them again takes some eighty quiet frames, with flashes missing until then. */
    static int made_for = -1;
    int branching = song.tja.course[song.course].is_branching ? 1 : 0;
    if (made_for != branching) {
        made_for = branching;
        memset(lane_done, 0, sizeof lane_done);
        memset(lane_full_ready, 0, sizeof lane_full_ready);
    }
    flash_want = 0;
    gfx_bg_strip(0, 0, 0, 0, NULL);
    flash_w = 0;
    for (int i = 0; i < 4; i++) {
        int x0, x1;
        if (gfx_sprite_extent(ids[i], 0, &x0, &x1) && x1 > flash_w) {
            flash_w = x1;
        }
    }
    if (flash_w > FLASH_MAX_W) {
        flash_w = FLASH_MAX_W;
    }
}

/* ---- piled-up notes ----
 * Per row of each plain note image, its opaque stretch: what a note drawn on top hides
 * of the one under it. Slow-scroll and stutter sections put a hundred notes on a few
 * pixels; only the visible slivers are drawn. */
#define SPAN_ROWS 64
static uint8_t span_a[5][SPAN_ROWS], span_b[5][SPAN_ROWS]; /* frames 1..4: don, ka, DON, KA */
static int span_rows[5];

/* ---- gauge and soul ----
 * They change on a judged note, not every frame, so they live on the background layer
 * and are redrawn there when their picture changes. */
#define GAUGE_W (GAUGE_UNITS * 16)
#define GAUGE_MAX_H 32
#define SOUL_MAX 64
static uint16_t gauge_under[GAUGE_W * GAUGE_MAX_H], soul_under[SOUL_MAX * SOUL_MAX];
static int gauge_h, soul_w, soul_h;
static int gauge_shown, soul_shown; /* what is drawn; -1: nothing yet */
static void hud_reset(void);
static void hud_update(void);

/* ---------- note visibility ---------- */

static float px_per_ms(const tp_note *n) { return (float)(n->bpm / 240000.0 * n->scroll_x) * TRAVEL; }

static void visible_span(const tp_note *n, double *load, double *unload) {
    const tp_chart *c = &song.chart;
    float right = TRAVEL + NOTE_HALF + 8, left = JUDGE_X + NOTE_HALF + 8;
    if (c->scroll_type != TP_NMSCROLL) {
        double per_beat = n->scroll_x * TRAVEL / 4;
        if (per_beat == 0) {
            *load = *unload = n->hit_ms;
            return;
        }
        double beat = tp_tempo_beat_at(c, n->hit_ms);
        double lo = beat - right / per_beat, hi = beat + left / per_beat;
        double first, last;
        tp_tempo_ms_within(c, fmin(lo, hi), fmax(lo, hi), &first, &last);
        *load = fmin(first, n->hit_ms);
        *unload = fmax(last, n->hit_ms);
    } else {
        float speed = fabsf(px_per_ms(n));
        if (speed == 0) {
            *load = *unload = n->hit_ms;
            return;
        }
        float span = (right > left ? right : left) / speed;
        *load = n->hit_ms - span;
        *unload = n->hit_ms + span;
    }
    if ((n->flags & TP_NOTE_SUDDEN) && n->sudden_appear_ms != INFINITY) {
        *load = n->hit_ms - n->sudden_appear_ms;
    }
}

static uint32_t song_serial, beat_cache_song; /* counts the charts set up; see note_x() */

/* Where a note is, in whole pixels. The note's own place on the "tape" and the tape's
 * position are rounded separately and then subtracted, not their difference: notes that
 * move at the same speed then all step by the same whole number of pixels each frame.
 * Rounding each note's distance on its own made neighbours shift against each other by
 * a pixel as they scrolled, and a drumroll's bar change its length. */
static int note_x(const tp_note *n, double t) {
    if ((n->flags & TP_NOTE_SUDDEN) && n->sudden_moving_ms != INFINITY && t < n->hit_ms - n->sudden_moving_ms) {
        t = n->hit_ms - n->sudden_moving_ms; /* waits in place until it starts to move */
    }
    if (song.chart.scroll_type != TP_NMSCROLL) {
        /* Both beats come from a search of the tempo map. The note's own never changes
         * and the time's is the same for nearly every note of a frame: remembered. */
        static struct {
            const tp_note *n;
            double beat;
        } own[64];
        static double at_t = NAN, at_beat;
        unsigned slot = (unsigned)(((uintptr_t)n / sizeof *n) & 63);
        if (own[slot].n != n || beat_cache_song != song_serial) {
            if (beat_cache_song != song_serial) { /* another chart: its notes may sit at the same addresses */
                memset(own, 0, sizeof own);
                at_t = NAN;
                beat_cache_song = song_serial;
            }
            own[slot].n = n;
            own[slot].beat = tp_tempo_beat_at(&song.chart, n->hit_ms);
        }
        if (t != at_t) {
            at_t = t;
            at_beat = tp_tempo_beat_at(&song.chart, t);
        }
        double per_beat = n->scroll_x * TRAVEL / 4;
        return JUDGE_X + (int)(floor(own[slot].beat * per_beat) - floor(at_beat * per_beat));
    }
    double per_ms = px_per_ms(n);
    return JUDGE_X + (int)(floor(n->hit_ms * per_ms) - floor(t * per_ms));
}

/* shell sort of the waiting notes by the time they come on screen */
static void sort_waiting(void) {
    static const int gaps[] = {8861, 3938, 1750, 701, 301, 132, 57, 23, 10, 4, 1};
    int n = vis_tail - vis_head;
    const tp_note **v = vis + vis_head, **e = vis_end + vis_head;
    double *k = vis_key + vis_head;
    for (unsigned g = 0; g < sizeof gaps / sizeof *gaps; g++) {
        int gap = gaps[g];
        for (int i = gap; i < n; i++) {
            const tp_note *tv = v[i], *te = e[i];
            double tk = k[i];
            int j = i;
            for (; j >= gap && k[j - gap] > tk; j -= gap) {
                v[j] = v[j - gap];
                e[j] = e[j - gap];
                k[j] = k[j - gap];
            }
            v[j] = tv;
            e[j] = te;
            k[j] = tk;
        }
    }
}

static void add_notes(const tp_note *notes, int count) {
    if (vis_head > 0) {
        int n = vis_tail - vis_head;
        memmove(vis, vis + vis_head, (size_t)n * sizeof *vis);
        memmove(vis_end, vis_end + vis_head, (size_t)n * sizeof *vis_end);
        memmove(vis_key, vis_key + vis_head, (size_t)n * sizeof *vis_key);
        vis_head = 0;
        vis_tail = n;
    }
    for (int i = 0; i < count && vis_tail < VIS_MAX; i++) {
        const tp_note *n = &notes[i];
        if (n->type == TP_TAIL || (song.has_audio_end && tp_note_unplayable(n, song.chart.scroll_type, &song.audio_end_ms))) {
            continue; /* tails are drawn with their heads */
        }
        if (n->type == TP_BARLINE && !(n->flags & TP_NOTE_DISPLAY)) {
            continue;
        }
        double load, unload;
        visible_span(n, &load, &unload);
        vis[vis_tail] = n;
        vis_end[vis_tail] = notes + count;
        vis_key[vis_tail] = load;
        vis_tail++;
    }
    sort_waiting();
}

static void add_events(const tp_event *ev, int count) {
    if (vt_head > 0) {
        memmove(vt, vt + vt_head, (size_t)(vt_tail - vt_head) * sizeof *vt);
        vt_tail -= vt_head;
        vt_head = 0;
    }
    for (int i = 0; i < count && vt_tail < VT_MAX; i++) {
        if (ev[i].kind == TP_EV_BPM || ev[i].kind == TP_EV_GOGO || ev[i].kind == TP_EV_LYRIC) {
            vt[vt_tail++] = &ev[i];
        }
    }
    for (int i = vt_head + 1; i < vt_tail; i++) { /* stable, by start time */
        const tp_event *e = vt[i];
        int j = i;
        for (; j > vt_head && vt[j - 1]->start_ms > e->start_ms; j--) {
            vt[j] = vt[j - 1];
        }
        vt[j] = e;
    }
}

static void activate(const tp_note *n, const tp_note *list_end) {
    if (act_n >= ACT_MAX) {
        return;
    }
    active a = {n, NULL, 0};
    double load;
    visible_span(n, &load, &a.unload);
    if (n->type == TP_ROLL_HEAD || n->type == TP_ROLL_HEAD_L || n->type == TP_BALLOON_HEAD || n->type == TP_KUSUDAMA) {
        for (const tp_note *t = n + 1; t < list_end; t++) {
            if (t->type == TP_TAIL) {
                double tl, tu;
                visible_span(t, &tl, &tu);
                a.tail = t;
                a.unload = fmax(a.unload, tu);
                break;
            }
            if (t->type != TP_BARLINE) {
                break;
            }
        }
    }
    int i = act_n++;
    while (i > 0 && act[i - 1].n->index > n->index) {
        act[i] = act[i - 1];
        i--;
    }
    act[i] = a;
}

/* ---------- background layer ---------- */

/* Paints the lane of a state (Go-Go or not, branch layer) onto the background layer.
 * The target must be the background layer. */
static void paint_lane(bool go, int branch) {
    int lane_bg = go ? SPR_LANE_BG_GOGO : SPR_LANE_BG;
    gfx_clip(LANE_X, LANE_Y, LANE_W, LANE_H);
    gfx_fill(LANE_X, LANE_Y, LANE_W, LANE_H, go ? RGB(72, 0, 88) : RGB(0, 0, 0));
    gfx_sprite(lane_bg, 0, 0, LANE_Y + (LANE_H - gfx_sprite_h(lane_bg)) / 2);
    if (song.tja.course[song.course].is_branching) {
        int layer = branch == TP_BRANCH_MASTER ? SPR_LAYER_MASTER
                    : branch == TP_BRANCH_EXPERT ? SPR_LAYER_EXPERT : SPR_LAYER_NORMAL;
        gfx_sprite(layer, 0, 0, LANE_Y + (LANE_H - gfx_sprite_h(layer)) / 2);
    }
    gfx_sprite_c(SPR_NOTES, 0, JUDGE_X, LANE_CY);
    gfx_clip_reset();
}

static int lane_state_of(bool go, int branch) {
    return (go ? 3 : 0) + (song.tja.course[song.course].is_branching && branch >= 0 && branch < 3 ? branch : 0);
}

/* The lane strip of the background layer. It changes with Go-Go Time and the branch,
 * so it is replaced on its own: repainting the whole layer would cost several frames
 * on the Pocket. A lane that was shown or prepared before is one copy. */
static void draw_lane_background(void) {
    gfx_bg_strip(0, 0, 0, 0, NULL);
    flash_want = 0;
    lane_state = lane_state_of(gogo, branch_shown);
    if (lane_full_ready[lane_state]) {
        gfx_bg_write(LANE_X, LANE_Y, LANE_W, LANE_H, lane_full[lane_state]);
    } else {
        gfx_target(GFX_BG);
        paint_lane(gogo, branch_shown);
        gfx_target(GFX_FRAME);
        gfx_bg_read(LANE_X, LANE_Y, LANE_W, LANE_H, lane_full[lane_state]);
        lane_full_ready[lane_state] = true;
    }
    gfx_bg_changed_rect(LANE_X, LANE_Y, LANE_W, LANE_H);
    bg_dirty = false;
    lane_quiet = 0;
    if (pf_option("flashlog")) { /* test hook: how much of this lane's flashes was ready in time */
        int ready = 0;
        for (int i = 1; i < FLASH_KEYS * FLASH_HITS; i++) {
            ready += flash_ready(lane_state, i / FLASH_HITS, i % FLASH_HITS);
        }
        pf_log("lane state %d at %d ms: %d of %d flash pictures ready", lane_state, (int)now, ready,
               FLASH_KEYS * FLASH_HITS - 1);
    }
}

static void draw_background(void) {
    static const char *const names[LIB_COURSES] = {"Easy", "Normal", "Hard", "Oni", "Edit"};
    gfx_target(GFX_BG);
    gfx_clip_reset();
    if (gfx_sprite_frames(SPR_BG)) {
        gfx_sprite(SPR_BG, 0, 0, 0);
    } else {
        gfx_fill(0, 0, GFX_W, GFX_H, RGB(16, 16, 24));
    }
    gfx_fill(0, LANE_Y - 1, GFX_W, 1, RGB(90, 90, 90));
    gfx_fill(0, LANE_Y + LANE_H, GFX_W, 1, RGB(90, 90, 90));
    gfx_sprite(SPR_HR_SENOTES, 0, 0, SENOTES_Y - 2);
    /* drum */
    gfx_fill(0, LANE_Y, LANE_X, LANE_H, RGB(24, 24, 28));
    gfx_fill(LANE_X - 1, LANE_Y, 1, LANE_H, RGB(90, 90, 90));
    gfx_sprite(SPR_MTAIKO, 0, DRUM_X, DRUM_Y);
    /* title band */
    gfx_sprite(SPR_HR_SONGTITLE, 0, 0, TITLE_Y);
    gfx_text_outline(FONT_L, GFX_W - 8, TITLE_Y + 5, lib_string(song.info->title), -1, RGB(255, 255, 255),
                     RGB(0, 0, 0), ALIGN_RIGHT, GFX_W - 110);
    char buf[32];
    snprintf(buf, sizeof buf, "%s \xE2\x98\x85%d", names[song.course < LIB_COURSES ? song.course : 3],
             song.info->level[song.course]);
    gfx_text_outline(FONT_S, 8, TITLE_Y + 8, buf, -1, RGB(255, 220, 120), RGB(0, 0, 0), ALIGN_LEFT, 0);
    if (gfx_sprite_frames(SPR_COURSE_SYMBOL) >= 8) {
        gfx_sprite(SPR_COURSE_SYMBOL, (song.course > 3 ? 3 : song.course) * 2, 20, LANE_Y - 72);
    }
    {
        char tags[64];
        snprintf(tags, sizeof tags, "%s%s", settings.auto_play ? "AUTO PLAY  " : "", modifier_tags());
        if (tags[0]) {
            gfx_text_outline(FONT_S, 40, 4, tags, -1, RGB(255, 96, 96), RGB(0, 0, 0), ALIGN_LEFT, 0);
        }
    }
    gfx_target(GFX_FRAME);
    gfx_bg_changed();
    hud_reset();
    draw_lane_background();
}

/* ---------- scene ---------- */

static void game_enter(void) {
    song_serial++;
    memset(gone, 0, sizeof gone);
    memset(&fx, 0, sizeof fx);
    fx.judge_t = fx.explode_t = fx.flash_t = fx.hit_flash_t = fx.roll_t = fx.balloon_t = fx.branch_t = fx.popup_t = -1e9;
    fx.balloon_end_t = fx.combo_t = fx.score_t = -1e9;
    for (int i = 0; i < 4; i++) {
        fx.drum_t[i] = -1e9;
    }
    lane_flash_reset();
    pf_ovl_clip(LANE_X, GFX_W);
    for (int f = 1; f <= 4; f++) {
        span_rows[f] = gfx_sprite_spans(SPR_NOTES, f, span_a[f], span_b[f], SPAN_ROWS);
    }
    vis_head = vis_tail = act_n = 0;
    vt_head = vt_tail = 0;
    add_notes(song.chart.master, song.chart.master_count);
    add_events(song.chart.events, song.chart.master_event_count);
    cur_bpm = song.tja.bpm;
    gogo = false;
    lyric = NULL;
    lyric_len = 0;
    branch_shown = TP_BRANCH_NORMAL;
    paused = false;
    pause_drawn = -1;
    retrying = false;
    peak_notes = peak_px = 0;
    frame_us_sum = 0;
    frame_us_max = 0;
    frame_count = 0;
    pf_hit_auto(!settings.auto_play);
    draw_background();
    song_play();
    now = song_now_ms();
}

static void finish(void) {
    const tp_judge *j = &song.judge;
    result_t *r = &last_result;
    memset(r, 0, sizeof *r);
    r->score = (uint32_t)j->score;
    r->good = j->good_count;
    r->ok = j->ok_count;
    r->bad = j->bad_count;
    r->max_combo = j->max_combo;
    r->rolls = j->total_drumroll;
    r->gauge_percent = tp_gauge_percent(&j->gauge);
    r->cleared = tp_gauge_clear(&j->gauge);
    r->rainbow = tp_gauge_rainbow(&j->gauge);
    r->full_combo = j->bad_count == 0 && j->good_count + j->ok_count > 0;
    r->all_good = r->full_combo && j->ok_count == 0;
    r->auto_play = settings.auto_play;
    r->avg_frame_us = frame_count ? (uint32_t)(frame_us_sum / frame_count) : 0;
    r->max_frame_us = frame_us_max;
    if (!settings.auto_play) {
        stats.songs_finished++;
        int crown = r->all_good ? CROWN_ALL_GOOD : r->full_combo ? CROWN_FULL_COMBO : r->cleared ? CROWN_CLEAR
                                                                                               : CROWN_PLAYED;
        r->new_record = score_submit(song.info->chart_crc, song.course, r, crown);
    }
    pf_log("result: score %u good %d ok %d bad %d combo %d rolls %d gauge %d%% | frame avg %u us max %u us | "
           "peak %u notes on screen; heaviest frame %u pixel units at %d ms",
           (unsigned)r->score, r->good, r->ok, r->bad, r->max_combo, r->rolls, r->gauge_percent,
           (unsigned)r->avg_frame_us, (unsigned)r->max_frame_us, (unsigned)peak_notes, (unsigned)peak_px, (int)peak_ms);
    pf_hit_auto(false);
    app_goto(&scene_result);
}

static void handle_judge_events(void) {
    tp_judge_event e;
    while (tp_judge_next_event(&song.judge, &e)) {
        switch (e.kind) {
        case TP_JE_JUDGMENT:
            fx.judge_kind = e.judgment;
            fx.judge_t = now;
            if (e.judgment != TP_BAD) {
                fx.explode_kind = e.judgment;
                fx.explode_big = e.note->type == TP_DON_L || e.note->type == TP_KAT_L;
                fx.explode_t = now;
                fx.hit_flash_t = now;
            }
            break;
        case TP_JE_NOTE_CORRECT:
            if ((uint32_t)e.note->index < GONE_MAX) {
                gone[e.note->index] = 1;
            }
            break;
        case TP_JE_SCORE:
            fx.popup_value = e.value;
            fx.popup_t = now;
            break;
        case TP_JE_DRUMROLL_HIT:
            fx.roll_count = e.value;
            fx.roll_t = now;
            fx.flash_kind = 2;
            fx.flash_t = now;
            break;
        case TP_JE_BALLOON_HIT:
            fx.balloon_left = e.note->count - e.value;
            fx.kusudama = e.note->type == TP_KUSUDAMA;
            fx.balloon_t = now;
            if (e.flag) { /* it burst */
                fx.explode_kind = TP_GOOD;
                fx.explode_big = true;
                fx.explode_t = now;
                fx.balloon_end_t = now;
                pf_hit_play(fx.kusudama ? PF_HIT_KUSUDAMA : PF_HIT_BALLOON);
            }
            break;
        case TP_JE_BALLOON_START:
            fx.balloon_left = e.note->count;
            fx.kusudama = e.note->type == TP_KUSUDAMA;
            fx.balloon_t = now;
            break;
        case TP_JE_BRANCH:
            if (e.section >= 0) {
                const tp_section *sec = &song.chart.sections[e.section];
                add_notes(song.chart.branch + sec->first_note, sec->note_count);
                add_events(song.chart.events + sec->first_event, sec->event_count);
            }
            if (e.value != branch_shown) {
                fx.branch_dir = e.value > branch_shown ? 0 : 1;
                fx.branch_t = now;
                branch_shown = e.value;
                bg_dirty = true;
            }
            break;
        case TP_JE_AUTO_HIT: {
            pf_hit_play(e.drum == TP_DRUM_DON ? PF_HIT_DON : PF_HIT_KA);
            int base = e.drum == TP_DRUM_DON ? DRUM_DON_L : DRUM_KA_L;
            if (e.flag) {
                fx.drum_t[base] = fx.drum_t[base + 1] = now;
            } else {
                auto_side ^= 1;
                fx.drum_t[base + auto_side] = now;
            }
            fx.flash_kind = e.drum == TP_DRUM_DON ? 0 : 1;
            fx.flash_t = now;
            break;
        }
        default: break;
        }
    }
}

/* ---- pause menu ----
 * On the background layer, drawn when it opens and when the cursor moves, with what
 * was under it kept. While it is open the lane's sprites are not shown: they would lie
 * on top of it (the sprite layer covers everything the CPU draws). */
#define PAUSE_X 100
#define PAUSE_Y 232 /* below the lane and the note names: the notes stay in view while paused */
#define PAUSE_W 200
#define PAUSE_H 110
static uint16_t pause_under[PAUSE_W * PAUSE_H];

static void pause_show(void) {
    static const char *const items[3] = {"Resume", "Retry", "Back to song select"};
    if (pause_drawn == pause_cursor) {
        return;
    }
    if (pause_drawn < 0) {
        gfx_bg_strip(0, 0, 0, 0, NULL); /* a lane flash is shown in place of the layer there */
        gfx_bg_read(PAUSE_X, PAUSE_Y, PAUSE_W, PAUSE_H, pause_under);
    }
    gfx_target(GFX_BG);
    gfx_clip_reset();
    gfx_fill(PAUSE_X, PAUSE_Y, PAUSE_W, PAUSE_H, RGB(10, 10, 14));
    gfx_frame_rect(PAUSE_X, PAUSE_Y, PAUSE_W, PAUSE_H, RGB(160, 160, 160));
    gfx_text(FONT_L, GFX_W / 2, PAUSE_Y + 8, "Paused", RGB(255, 220, 80), ALIGN_CENTER, 0);
    for (int i = 0; i < 3; i++) {
        if (i == pause_cursor) {
            gfx_fill(PAUSE_X + 8, PAUSE_Y + 34 + i * 24, PAUSE_W - 16, 22, RGB(64, 64, 72));
        }
        gfx_text(FONT_S, GFX_W / 2, PAUSE_Y + 38 + i * 24, items[i],
                 i == pause_cursor ? RGB(255, 255, 255) : RGB(170, 170, 170), ALIGN_CENTER, 0);
    }
    gfx_target(GFX_FRAME);
    gfx_bg_changed_rect(PAUSE_X, PAUSE_Y, PAUSE_W, PAUSE_H);
    pause_drawn = pause_cursor;
}

static void pause_hide(void) {
    if (pause_drawn >= 0) {
        gfx_bg_write(PAUSE_X, PAUSE_Y, PAUSE_W, PAUSE_H, pause_under);
        gfx_bg_changed_rect(PAUSE_X, PAUSE_Y, PAUSE_W, PAUSE_H);
        pause_drawn = -1;
    }
}

static void pause_menu(void) {
    if (in.repeat & PF_BTN_DOWN) {
        pause_cursor = (pause_cursor + 1) % 3;
        ui_sound_move();
    } else if (in.repeat & PF_BTN_UP) {
        pause_cursor = (pause_cursor + 2) % 3;
        ui_sound_move();
    } else if (in.pressed & PF_BTN_START) {
        pause_cursor = 0;
        in.pressed |= PF_BTN_A;
    }
    if (in.pressed & PF_BTN_A) {
        if (pause_cursor == 0) {
            paused = false;
            song_pause(false);
            pf_hit_auto(!settings.auto_play);
        } else if (pause_cursor == 1) {
            song_load(song.index, song.course);
            paused = false;
            retrying = true;
        } else {
            song_unload();
            settings_save(); /* keeps the statistics of an abandoned play */
            app_goto(&scene_select);
        }
    }
}

bool game_is_playing(void) { return !paused && !retrying; }

static int flash_level(double since) {
    int alpha = envelope(since, 0, 83, 233); /* held for 83 ms, then fading over 150 ms */
    return alpha >= 20 ? 2 : alpha >= 6 ? 1 : 0;
}

/* the part of a state's plain lane that flashes cover, onto the background layer */
static void lane_put_plain(int state) {
    for (int y = 0; y < LANE_H; y++) {
        gfx_bg_write(LANE_X, LANE_Y + y, flash_w, 1, lane_full[state] + y * LANE_W);
    }
}

/* The plain lane of a state that is not on screen: painted on the background layer,
 * kept, and the lane that is on screen put back. */
static void lane_full_prepare(int state) {
    if (state == lane_state) {
        gfx_bg_read(LANE_X, LANE_Y, LANE_W, LANE_H, lane_full[state]);
    } else {
        gfx_target(GFX_BG);
        paint_lane(state >= 3, state % 3);
        gfx_target(GFX_FRAME);
        gfx_bg_read(LANE_X, LANE_Y, LANE_W, LANE_H, lane_full[state]);
        gfx_bg_write(LANE_X, LANE_Y, LANE_W, LANE_H, lane_full[lane_state]);
    }
    lane_full_ready[state] = true;
}

/* One phase of the picture lane_strips[state][key][hit]: its starting picture goes onto
 * the background layer, half of the flash is drawn on it, the result is kept and the
 * layer put back as it was. A picture with both flashes starts from the one with the
 * colour flash alone, so no call blends more than half an image. Needs the plain lanes
 * of `state` and of the lane on screen. */
static void lane_flash_step(int state, int key, int hit) {
    static const int flash_sprite[3] = {SPR_FLASH_RED, SPR_FLASH_BLUE, SPR_FLASH_YELLOW};
    int phase = lane_done[state][key][hit];
    uint16_t *picture = lane_strips[state][key][hit];
    if (phase > 0) {
        gfx_bg_write(LANE_X, LANE_Y, flash_w, LANE_H, picture);
    } else if (key && hit) {
        gfx_bg_write(LANE_X, LANE_Y, flash_w, LANE_H, lane_strips[state][key][0]);
    } else if (state != lane_state) {
        lane_put_plain(state);
    }
    gfx_target(GFX_BG);
    gfx_clip(LANE_X, LANE_Y + phase * LANE_H / FLASH_PHASES, flash_w, LANE_H / FLASH_PHASES);
    if (hit) {
        gfx_sprite_alpha(SPR_FLASH_HIT, 0, LANE_X, LANE_Y + 4, hit * 32 / FLASH_LEVELS);
    } else {
        gfx_sprite_alpha(flash_sprite[(key - 1) / FLASH_LEVELS], 0, LANE_X, LANE_Y + 4,
                         ((key - 1) % FLASH_LEVELS + 1) * 32 / FLASH_LEVELS);
    }
    gfx_clip_reset();
    gfx_target(GFX_FRAME);
    gfx_bg_read(LANE_X, LANE_Y, flash_w, LANE_H, picture);
    lane_put_plain(lane_state);
    lane_done[state][key][hit] = (uint8_t)(phase + 1);
}

/* One piece of the background work, if any is left: the plain lane of a state, or a
 * phase of one of its flash pictures. Returns whether it did something. */
static bool lane_prepare_state(int state) {
    if (!lane_full_ready[state]) {
        lane_full_prepare(state);
        return true;
    }
    for (int pass = 0; pass < 2; pass++) { /* single flashes, then the pairs */
        for (int i = 1; i < FLASH_KEYS * FLASH_HITS; i++) {
            int key = i / FLASH_HITS, hit = i % FLASH_HITS;
            if ((key && hit) == (pass == 1) && !flash_ready(state, key, hit)) {
                if (key && hit && !flash_ready(state, key, 0)) {
                    hit = 0; /* its starting point first */
                }
                lane_flash_step(state, key, hit);
                return true;
            }
        }
    }
    return false;
}

static void lane_background_work(void);

/* Picks the lane picture for this frame, and does a piece of the background work if
 * this is a quiet moment. */
static void lane_flash_select(void) {
    int colour = 0, hit = 0;
    if (flash_w > 0) {
        colour = flash_level(fx.flash_t);
        hit = flash_level(fx.hit_flash_t);
    }
    int key = colour ? 1 + fx.flash_kind * FLASH_LEVELS + (colour - 1) : 0;
    const uint16_t *pixels = NULL;
    if (key || hit) {
        if (flash_ready(lane_state, key, hit)) {
            pixels = lane_strips[lane_state][key][hit];
        } else {
            flash_want = key * FLASH_HITS + hit;
            pixels = key && flash_ready(lane_state, key, 0)   ? lane_strips[lane_state][key][0]
                     : hit && flash_ready(lane_state, 0, hit) ? lane_strips[lane_state][0][hit]
                                                              : NULL;
        }
    }
    gfx_bg_strip(LANE_X, LANE_Y, flash_w, LANE_H, pixels);

    static uint32_t work_us; /* what the background work cost in the last frame */
    uint32_t own_us = prof_last_frame_us() > work_us ? prof_last_frame_us() - work_us : 0;
    work_us = 0;
    if (lane_quiet < FLASH_QUIET) {
        lane_quiet++;
        return; /* the lane has just been replaced: that frame and the next have enough to do */
    }
    /* Background work only in a quiet moment: the game's own last frame was short and no
     * note is about to be hit or has just been. A frame with a hit in it is the longest
     * kind, and one that runs over shows as a stutter of the notes. */
    if (flash_w <= 0 || own_us >= FLASH_SPARE_US || in.event_count ||
        now - fx.judge_t < FLASH_CALM_MS) {
        return;
    }
    for (int i = 0; i < act_n; i++) {
        const tp_note *n = act[i].n;
        if (n->type != TP_BARLINE && n->hit_ms - now < FLASH_CALM_MS && n->hit_ms - now > -FLASH_CALM_MS) {
            return;
        }
    }
    uint64_t work_start = pf_time_us();
    lane_background_work();
    work_us = (uint32_t)(pf_time_us() - work_start);
}

/* one piece of what is left to prepare */
static void lane_background_work(void) {
    if (!lane_full_ready[lane_state]) {
        lane_full_prepare(lane_state);
        return;
    }
    /* the picture a press had to do without comes first */
    int wk = flash_want / FLASH_HITS, wh = flash_want % FLASH_HITS;
    if (flash_want && !flash_ready(lane_state, wk, wh)) {
        lane_flash_step(lane_state, wk, wk && wh && !flash_ready(lane_state, wk, 0) ? 0 : wh);
        return;
    }
    flash_want = 0;
    /* then everything for the lane as it is, then for the lane on the other side of Go-Go Time */
    if (!lane_prepare_state(lane_state)) {
        lane_prepare_state((lane_state + 3) % LANE_STATES);
    }
}

static void game_update(void) {
    uint64_t t0 = pf_time_us();
    song_state st = song_poll();
    if (st == SONG_LOADING) { /* a retry is loading */
        return;
    }
    if (st == SONG_ERROR) {
        app_goto(&scene_select);
        return;
    }
    if (retrying) { /* the reload has finished: set the scene up again, once */
        game_enter();
    }
    if (paused) {
        pause_menu();
        if (paused) {
            pause_show();
        } else if (!retrying) {
            pause_hide(); /* resumed; on a retry the whole background is drawn again */
        }
        return;
    }
    if (in.pressed & PF_BTN_START) {
        paused = true;
        prof_detail = false; /* its box lies where the menu goes */
        pause_cursor = 0;
        song_pause(true);
        pf_hit_auto(false);
        return;
    }
    if (in.pressed & PF_BTN_SELECT) {
        prof_detail = !prof_detail;
    }
    now = song_now_ms();
    tp_judge *j = &song.judge;

    /* hits, each judged at the time of its own button edge */
    if (!settings.auto_play) {
        for (int i = 0; i < in.event_count; i++) {
            const pf_input_event *e = &in.events[i];
            if (!e->pressed) {
                continue;
            }
            for (int k = 0; k < DRUM_KEYS; k++) {
                if (e->button & settings.map[k]) {
                    double t = song_event_ms(e->counter);
                    tp_drum drum = k <= DRUM_DON_R ? TP_DRUM_DON : TP_DRUM_KAT;
                    if (drum == TP_DRUM_DON) {
                        stats.don_hits++;
                    } else {
                        stats.ka_hits++;
                    }
                    tp_judge_begin_frame(j, t);
                    tp_judge_hit(j, t, drum);
                    fx.drum_t[k] = now;
                    fx.flash_kind = drum == TP_DRUM_DON ? 0 : 1;
                    fx.flash_t = now;
                    break;
                }
            }
        }
    }
    tp_judge_begin_frame(j, now);
    if (settings.auto_play) {
        tp_judge_autoplay(j, now, cur_bpm);
    }
    tp_judge_end_frame(j, now);
    handle_judge_events();
    if (j->combo != fx.last_combo) {
        fx.last_combo = j->combo;
        fx.combo_t = now;
    }
    if (j->score != fx.last_score) {
        fx.last_score = j->score;
        fx.score_t = now;
    }

    /* visual timeline */
    while (vt_head < vt_tail && now >= vt[vt_head]->start_ms) {
        const tp_event *e = vt[vt_head++];
        if (e->kind == TP_EV_BPM) {
            cur_bpm = e->bpm;
        } else if (e->kind == TP_EV_GOGO) {
            if (gogo != (bool)e->gogo) {
                gogo = e->gogo;
                gogo_since = now;
                bg_dirty = true;
            }
        } else if (e->kind == TP_EV_LYRIC) {
            lyric = e->text;
            lyric_len = (int)e->text_len;
        }
    }
    /* notes coming on and going off screen */
    while (vis_head < vis_tail && now >= vis_key[vis_head]) {
        activate(vis[vis_head], vis_end[vis_head]);
        vis_head++;
    }
    int kept = 0;
    for (int i = 0; i < act_n; i++) {
        const tp_note *n = act[i].n;
        bool hit = (uint32_t)n->index < GONE_MAX && gone[n->index];
        if (now < act[i].unload && !hit) {
            act[kept++] = act[i];
        }
    }
    act_n = kept;

    if (bg_dirty) {
        draw_lane_background();
    }
    hud_update();
    lane_flash_select();
    bool over = tp_judge_finished(j) && now > j->chart_end_ms + 2000;
    /* A chart can leave something in the judge for ever: a drumroll without an end, a
     * branch section that is never taken up. The song is over all the same once its last
     * note is long past and the music has played out. */
    if (!over && now > j->chart_end_ms + 5000 && (!song.has_audio_end || song_audio_done())) {
        over = true;
    }
    if (over) {
        finish();
    }
    uint32_t us = (uint32_t)(pf_time_us() - t0);
    frame_us_sum += us; /* draw time is added in game_draw */
}

/* ---------- drawing ---------- */

/* alpha 0..32: fades in over `in_ms`, holds, fades out between `out_start` and `out_end` (ms since `since`) */
static int envelope(double since, double in_ms, double out_start, double out_end) {
    double t = now - since;
    if (t < 0 || t >= out_end) {
        return 0;
    }
    if (t < in_ms) {
        return (int)(32 * t / in_ms);
    }
    if (t > out_start) {
        return (int)(32 * (out_end - t) / (out_end - out_start));
    }
    return 32;
}

/* YataiDON's TextStretchAnimation: digits grow for 50 ms, then settle in steps over 116 ms.
 * Its values (2..12 px at 720p) are a third of that here. */
static int stretch_px(double since) {
    double t = now - since;
    float v;
    if (t < 0 || t > 166) {
        return 0;
    }
    if (t <= 50) {
        v = 2 + 5 * ((int)t / 25.0f);
    } else {
        v = 12 - 2 * ((int)((t - 50) / 16.57) + 1);
    }
    return (int)(v / 3 + (v >= 0 ? 0.5f : -0.5f));
}

static int gauge_state(const tp_gauge *g) { return tp_gauge_rainbow(g) ? 3 : tp_gauge_clear(g) ? 2 : 1; }

/* the gauge's units `from` to `to`, filled to `fill` pixels, in animation frame `anim` */
static void draw_gauge(int fill, int state, int anim, int from, int to) {
    const tp_gauge *g = &song.judge.gauge;
    int clear_units = (g->clear_points * GAUGE_UNITS + 5000) / g->max_points;
    int total = gfx_sprite_frames(SPR_GAUGE);
    if (total >= 24) {
        int cols = total / 4;
        for (int i = from; i <= to; i++) {
            int type = i == 0 ? 0 : i < clear_units - 1 ? 1 : i == clear_units - 1 ? 2 : i == clear_units ? 3
                       : i < GAUGE_UNITS - 1 ? 4 : 5;
            int x = GAUGE_X + i * 16;
            int lit = fill - i * 16;
            if (lit < 16) {
                gfx_sprite(SPR_GAUGE, type, x, GAUGE_Y);
            }
            if (lit > 0) {
                gfx_clip(x, GAUGE_Y, lit < 16 ? lit : 16, gauge_h);
                gfx_sprite(SPR_GAUGE, state * cols + anim * 6 + type, x, GAUGE_Y);
                gfx_clip(GAUGE_X, GAUGE_Y, GAUGE_W, gauge_h); /* the caller's: what is saved under the gauge */
            }
        }
    } else {
        gfx_fill(GAUGE_X, GAUGE_Y + 3, GAUGE_W, 10, RGB(40, 40, 56));
        gfx_fill(GAUGE_X, GAUGE_Y + 3, fill, 10, state == 3 ? RGB(255, 200, 40) : state == 2 ? RGB(255, 120, 40)
                                                                                         : RGB(60, 160, 240));
        gfx_fill(GAUGE_X + clear_units * 16, GAUGE_Y, 1, 16, RGB(255, 255, 255));
    }
}

/* after the background layer was composed: remember what lies under gauge and soul */
static void hud_reset(void) {
    gauge_h = gfx_sprite_frames(SPR_GAUGE) >= 24 ? gfx_sprite_h(SPR_GAUGE) : 16;
    gauge_h = gauge_h > GAUGE_MAX_H ? GAUGE_MAX_H : gauge_h;
    soul_w = gfx_sprite_frames(SPR_SOUL) ? gfx_sprite_w(SPR_SOUL) : 0;
    soul_h = gfx_sprite_h(SPR_SOUL);
    soul_w = soul_w > SOUL_MAX ? SOUL_MAX : soul_w;
    soul_w = soul_w > GFX_W - (GAUGE_X + GAUGE_W + 4) ? GFX_W - (GAUGE_X + GAUGE_W + 4) : soul_w; /* stays on the screen */
    soul_h = soul_h > SOUL_MAX ? SOUL_MAX : soul_h;
    gfx_bg_read(GAUGE_X, GAUGE_Y, GAUGE_W, gauge_h, gauge_under);
    if (soul_w) {
        gfx_bg_read(GAUGE_X + GAUGE_W + 4, GAUGE_Y - 8, soul_w, soul_h, soul_under);
    }
    gauge_shown = soul_shown = -1;
}

/* redraws gauge and soul on the background layer if their picture changed */
static void hud_update(void) {
    const tp_gauge *g = &song.judge.gauge;
    int fill = (int)(g->points * GAUGE_W / g->max_points);
    int state = gauge_state(g);
    int anim = 0;
    if (state == 3 && gfx_sprite_frames(SPR_GAUGE) >= 48) {
        anim = (int)(now / assets_cfg(CFG_GAUGE_INTERVAL_MS, 100)) % (gfx_sprite_frames(SPR_GAUGE) / 24);
        anim = anim < 0 ? 0 : anim;
    }
    int picture = fill | state << 12 | anim << 16;
    if (picture != gauge_shown) {
        /* A hit moves the fill by a pixel or two: only the units between the old and the
         * new fill change. All sixteen when the colour or the animation frame does, and
         * with the plain bars of a skin without gauge pictures. */
        int from = 0, to = GAUGE_UNITS - 1;
        if (gauge_shown >= 0 && (gauge_shown >> 12) == (picture >> 12) && gfx_sprite_frames(SPR_GAUGE) >= 24) {
            int old = gauge_shown & 0xFFF;
            from = (old < fill ? old : fill) / 16;
            to = (old < fill ? fill : old) / 16;
            to = to > GAUGE_UNITS - 1 ? GAUGE_UNITS - 1 : to;
        }
        int x = GAUGE_X + from * 16, w = (to - from + 1) * 16;
        gauge_shown = picture;
        for (int r = 0; r < gauge_h; r++) {
            gfx_bg_write(x, GAUGE_Y + r, w, 1, gauge_under + r * GAUGE_W + from * 16);
        }
        gfx_target(GFX_BG);
        gfx_clip(GAUGE_X, GAUGE_Y, GAUGE_W, gauge_h);
        draw_gauge(fill, state, anim, from, to);
        gfx_clip_reset();
        gfx_target(GFX_FRAME);
        gfx_bg_changed_rect(x, GAUGE_Y, w, gauge_h);
    }
    if (soul_w) {
        int soul_frames = gfx_sprite_frames(SPR_SOUL), frame = 0;
        if (state == 3 && soul_frames > 1) {
            frame = 1 + (int)(now / assets_cfg(CFG_SOUL_INTERVAL_MS, 50)) % (soul_frames - 1);
            frame = frame < 1 ? 1 : frame;
        }
        if (frame != soul_shown) {
            int x = GAUGE_X + GAUGE_W + 4, y = GAUGE_Y - 8;
            soul_shown = frame;
            gfx_bg_write(x, y, soul_w, soul_h, soul_under);
            gfx_target(GFX_BG);
            gfx_clip(x, y, soul_w, soul_h);
            gfx_sprite(SPR_SOUL, frame, x, y);
            gfx_clip_reset();
            gfx_target(GFX_FRAME);
            gfx_bg_changed_rect(x, y, soul_w, soul_h);
        }
    }
}

/* A sprite of the lane: on the sprite layer if that has the picture, drawn into the frame
 * otherwise. The layer has no alpha of its own: a fade is full, then half, then gone. */
static void lane_sprite(int id, int frame, int x, int y, int alpha) {
    if (alpha <= 0) {
        return;
    }
    int cell = overlay_cell(id, frame);
    if (cell >= 0) {
        overlay_add(cell, x, y, alpha < 20);
    } else {
        gfx_sprite_alpha(id, frame, x, y, alpha);
    }
}

/* a number centred on cx, as sprites of the layer when its digits are there */
static void lane_number(int id, int value, int cx, int y, int spacing, int stretch, int alpha) {
    if (overlay_cell(id, 0) < 0) {
        gfx_number_fx(id, value, cx, y, ALIGN_CENTER, spacing, stretch, alpha);
        return;
    }
    int digits[10], n = 0;
    value = value < 0 ? 0 : value;
    do {
        digits[n++] = value % 10;
        value /= 10;
    } while (value && n < 10);
    int w = gfx_sprite_w(id) + spacing;
    int x = cx - (n * w - spacing) / 2;
    for (int i = n - 1; i >= 0; i--, x += w) {
        overlay_add(overlay_cell(id, digits[i]), x, y, alpha < 20);
    }
}

static void draw_notes(void) {
    static const uint8_t moji_frame[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 255, 11};
    bool layer = overlay_on();
    gfx_clip(LANE_X, LANE_Y, GFX_W - LANE_X, LANE_H);
    uint16_t bar_color = (uint16_t)assets_cfg(CFG_BARLINE_COLOR, 0xFFFF);
    for (int i = 0; i < act_n; i++) {
        const tp_note *n = act[i].n;
        if (n->type == TP_BARLINE) {
            int x = note_x(n, now);
            bool branch = (n->flags & TP_NOTE_BRANCH_START) != 0;
            if (layer) {
                overlay_add(branch ? overlay_bar_branch : overlay_bar, x, LANE_Y + 4, false);
            } else {
                gfx_fill(x, LANE_Y + 4, 1, LANE_H - 8, branch ? RGB(255, 220, 0) : bar_color);
            }
        }
    }
    /* inner hit effect, under the notes: fades out over 283 ms */
    {
        lane_sprite(SPR_EXPLOSION_LOWER, 0, JUDGE_X - gfx_sprite_w(SPR_EXPLOSION_LOWER) / 2,
                    LANE_CY - gfx_sprite_h(SPR_EXPLOSION_LOWER) / 2, envelope(fx.explode_t, 0, 0, 283.3));
    }
    /* the plain note that will be drawn next, on top of the one being drawn: its position
     * and kind are known one step ahead, so the hidden part of each note can be left out */
    int over_x = 0, over_type = 0;
    for (int i = act_n - 1; i >= 0; i--) { /* later notes first, so earlier ones end up on top */
        const tp_note *n = act[i].n;
        if (n->type == TP_BARLINE || !(n->flags & TP_NOTE_DISPLAY)) {
            continue;
        }
        int x = note_x(n, now);
        switch (n->type) {
        case TP_DON:
        case TP_KAT:
        case TP_DON_L:
        case TP_KAT_L: {
            /* find the next plain note in drawing order (an earlier one in the chart);
             * only the CPU's own drawing needs it */
            over_type = 0;
            for (int k = i - 1; k >= 0 && !layer; k--) {
                const tp_note *m = act[k].n;
                if (m->type >= TP_DON && m->type <= TP_KAT_L && (m->flags & TP_NOTE_DISPLAY)) {
                    over_type = m->type;
                    over_x = note_x(m, now);
                    break;
                }
                if (m->type != TP_BARLINE) {
                    break; /* a roll or balloon comes next: draw this note whole */
                }
            }
            int d = over_x - x;
            if (layer) {
                lane_sprite(SPR_NOTES, n->type, x - NOTE_HALF, LANE_CY - 24, 32);
            } else if (over_type && d > -48 && d < 48 && span_rows[over_type]) {
                gfx_sprite_behind(SPR_NOTES, n->type, x - NOTE_HALF, LANE_CY - 24, over_x - NOTE_HALF,
                                  span_a[over_type], span_b[over_type], span_rows[over_type]);
            } else {
                gfx_sprite(SPR_NOTES, n->type, x - NOTE_HALF, LANE_CY - 24);
            }
            break;
        }
        case TP_ROLL_HEAD:
        case TP_ROLL_HEAD_L: {
            int base = n->type == TP_ROLL_HEAD ? 5 : 8;
            if (act[i].tail) {
                int xt = note_x(act[i].tail, now);
                int a = x < xt ? x : xt, b = x < xt ? xt : x;
                if (layer) {
                    /* the bar from a to b: whole 48 px tiles, then narrower pieces for the rest
                     * (a sprite cannot be cut to a width); what is left over is under the tail */
                    static const int piece_w[OVERLAY_ROLL_PIECES] = {24, 12, 6, 3};
                    int bx = a < LANE_X - 48 ? a + (LANE_X - 48 - a) / 48 * 48 : a;
                    for (; b - bx >= 48 && bx < GFX_W; bx += 48) {
                        lane_sprite(SPR_NOTES, base + 1, bx, LANE_CY - 24, 32);
                    }
                    for (int k = 0; k < OVERLAY_ROLL_PIECES && bx < GFX_W; k++) {
                        if (b - bx >= piece_w[k]) {
                            overlay_add(overlay_roll_piece[base == 8][k], bx, LANE_CY - 24, false);
                            bx += piece_w[k];
                        }
                    }
                    /* a pixel or two are left: the bar must reach the tail's cap, which starts
                     * exactly at b, so the smallest piece goes there once more, flush with the end */
                    int last = piece_w[OVERLAY_ROLL_PIECES - 1];
                    if (b > bx && b - last >= a && b - last < GFX_W) {
                        overlay_add(overlay_roll_piece[base == 8][OVERLAY_ROLL_PIECES - 1], b - last, LANE_CY - 24, false);
                    }
                } else {
                    gfx_clip(a < LANE_X ? LANE_X : a, LANE_Y, b - (a < LANE_X ? LANE_X : a), LANE_H);
                    for (int bx = a - (a % 48 + 48) % 48; bx < b; bx += 48) {
                        gfx_sprite(SPR_NOTES, base + 1, bx, LANE_CY - 24);
                    }
                    gfx_clip(LANE_X, LANE_Y, GFX_W - LANE_X, LANE_H);
                }
                lane_sprite(SPR_NOTES, base + 2, xt - NOTE_HALF, LANE_CY - 24, 32);
            }
            lane_sprite(SPR_NOTES, base, x - NOTE_HALF, LANE_CY - 24, 32);
            break;
        }
        case TP_BALLOON_HEAD:
        case TP_KUSUDAMA: {
            int base = n->type == TP_BALLOON_HEAD ? 11 : 13;
            if (now >= n->hit_ms && act[i].tail && now < act[i].tail->hit_ms) {
                x = JUDGE_X; /* waits at the drum while it is being hit */
            }
            lane_sprite(SPR_NOTES, base + 1, x + NOTE_HALF, LANE_CY - 24, 32);
            lane_sprite(SPR_NOTES, base, x - NOTE_HALF, LANE_CY - 24, 32);
            break;
        }
        default: break;
        }
    }
    gfx_clip_reset();
    /* note names under the lane */
    if (gfx_sprite_frames(SPR_SENOTES) >= 12) {
        gfx_clip(LANE_X, SENOTES_Y, GFX_W - LANE_X, 16);
        int label_x = -10000;
        for (int i = act_n - 1; i >= 0; i--) {
            const tp_note *n = act[i].n;
            /* also of notes that are not drawn ("doron"), as in YataiDON */
            if (n->type == TP_BARLINE || n->moji >= 12 || moji_frame[n->moji] == 255) {
                continue;
            }
            int x = note_x(n, now);
            if (x - label_x > -8 && x - label_x < 8) {
                continue; /* piled-up notes: one name is enough, the rest could not be read anyway */
            }
            label_x = x;
            lane_sprite(SPR_SENOTES, moji_frame[n->moji], x - 24, SENOTES_Y, 32);
        }
        gfx_clip_reset();
    }
}

/* Go-Go splash, six across the bottom, for 400 ms. On the sprite layer it costs nothing;
 * drawn by the CPU it is left out where the device cannot blend that much. */
static void draw_splash(void) {
    if (!gogo || now - gogo_since >= 400 || !gfx_sprite_frames(SPR_GOGO_SPLASH)) {
        return;
    }
    if (overlay_cell(SPR_GOGO_SPLASH, 0) < 0 &&
        6u * (uint32_t)(gfx_sprite_w(SPR_GOGO_SPLASH) * gfx_sprite_h(SPR_GOGO_SPLASH)) > pf_blend_budget()) {
        return;
    }
    int f = (int)((now - gogo_since) / assets_cfg(CFG_GOGOSPLASH_INTERVAL_MS, 50));
    if (f >= 0 && f < gfx_sprite_frames(SPR_GOGO_SPLASH)) {
        for (int i = 0; i < 6; i++) {
            lane_sprite(SPR_GOGO_SPLASH, f, 12 + i * 66, GFX_H - gfx_sprite_h(SPR_GOGO_SPLASH), 32);
        }
    }
}

/* Layers, bottom to top, as in YataiDON's Player::draw: gauge and score, flame and lane
 * flashes, inner hit effect, notes, drum flashes and combo, popups, then the hit burst,
 * the judgment text and the roll/balloon counters on top of everything. */
static void game_draw(void) {
    uint64_t t0 = pf_time_us();
    const tp_judge *j = &song.judge;

    overlay_begin();
    if (retrying) { /* the song is being loaded again: nothing in the lane */
        overlay_clear();
        gfx_number_fx(SPR_NUM_M, j->score, GFX_W - 8, GAUGE_Y + 20, ALIGN_RIGHT, 0, 0, 32);
        return;
    }
    /* While paused the lane is drawn as it stands (chart time does not move): the menu is
     * below it. Only the Go-Go splash would lie on the menu. */
    if (!paused && overlay_cell(SPR_GOGO_SPLASH, 0) >= 0) {
        /* On the sprite layer the splash goes behind everything else: a faint sprite pixel
         * is mixed with the picture under it, not with a sprite, so in front of a note it
         * would replace the note's pixels. */
        draw_splash();
    }
    /* The Go-Go flame, all through Go-Go Time. Its ring is in the middle of its frame:
     * centred on the judge circle. On the sprite layer it comes here, behind bar lines and
     * notes; the CPU would blend its 2,900 soft pixels in every frame. */
    int fire_frame = -1;
    if (gogo && gfx_sprite_frames(SPR_FIRE)) {
        fire_frame = (int)((now - gogo_since) / assets_cfg(CFG_FIRE_INTERVAL_MS, 50)) % gfx_sprite_frames(SPR_FIRE);
        fire_frame = fire_frame < 0 ? 0 : fire_frame;
        int cell = overlay_cell(SPR_FIRE, fire_frame);
        if (cell >= 0) {
            overlay_add(cell, JUDGE_X - gfx_sprite_w(SPR_FIRE) / 2, LANE_CY - gfx_sprite_h(SPR_FIRE) / 2, false);
            fire_frame = -1;
        }
    }
    gfx_number_fx(SPR_NUM_M, j->score, GFX_W - 8, GAUGE_Y + 20, ALIGN_RIGHT, 0, stretch_px(fx.score_t), 32);

    prof_mark(PROF_FX);
    if (fire_frame >= 0) { /* no layer, or no room on its sheet */
        gfx_sprite_c(SPR_FIRE, fire_frame, JUDGE_X, LANE_CY);
    }
    /* the lane flashes are part of the lane picture: lane_flash_select() */

    prof_mark(PROF_NOTES);
    draw_notes();
    prof_mark(PROF_HUD);

    /* drum: flashes held for 67 ms, fading over 100 ms; combo from 3, stretching on every hit */
    for (int k = 0; k < 4; k++) {
        int id = k <= DRUM_DON_R ? SPR_MTAIKO_RED : SPR_MTAIKO_BLUE;
        int right = k & 1;
        gfx_sprite_alpha(id, right, DRUM_X + right * gfx_sprite_w(id), DRUM_Y, envelope(fx.drum_t[k], 0, 67, 167));
    }
    if (j->combo >= 3) {
        int id = j->combo >= 100 ? SPR_NUM_COMBO_L : SPR_NUM_COMBO;
        gfx_number_fx(id, j->combo, DRUM_X + 24, DRUM_Y + 24 - gfx_sprite_h(id) / 2, ALIGN_CENTER, -2,
                      stretch_px(fx.combo_t), 32);
    }

    /* points just earned: fade in 100 ms, out between 367 and 433 ms */
    if (fx.popup_value > 0 && gfx_sprite_frames(SPR_NUM_S)) {
        gfx_number_fx(SPR_NUM_S, fx.popup_value, GFX_W - 8, GAUGE_Y + 42, ALIGN_RIGHT, 0, 0,
                      envelope(fx.popup_t, 100, 366.7, 433.7));
    }
    gfx_sprite_alpha(SPR_BRANCH_BALLOON, fx.branch_dir, 190, LANE_Y - 42, envelope(fx.branch_t, 117, 917, 1034));
    if (!paused && overlay_cell(SPR_GOGO_SPLASH, 0) < 0) {
        draw_splash(); /* drawn into the frame, over the notes */
    }

    /* hit burst: full for 167 ms, gone at 200 ms; above everything, the drum included */
    prof_mark(PROF_FX);
    {
        int frames = assets_cfg(CFG_EXPLOSION_FRAMES, 0);
        int frame = fx.explode_kind == TP_GOOD ? 0 : 1;
        int alpha = envelope(fx.explode_t, 0, 166.7, 200);
        if (frames >= 1) { /* an animated skin: rows are good, ok, big good, big ok */
            int f = (int)((now - fx.explode_t) / assets_cfg(CFG_EXPLOSION_INTERVAL_MS, 50));
            frame = (frame + (fx.explode_big ? 2 : 0)) * frames + f;
            alpha = f >= 0 && f < frames ? 32 : 0;
        }
        lane_sprite(SPR_EXPLOSION, frame, JUDGE_X - gfx_sprite_w(SPR_EXPLOSION) / 2,
                    LANE_CY - gfx_sprite_h(SPR_EXPLOSION) / 2, alpha);
    }
    /* judgment text: drops 8 px into place within 67 ms while fading in from half, holds,
     * and is gone after 350 ms */
    {
        double t = now - fx.judge_t;
        if (t >= 0 && t < 350) {
            int dy = t < 66.7 ? (int)(t * 8 / 66.7) - 4 : 4;
            int alpha = t < 66.7 ? 16 + (int)(t * 16 / 66.7) : t > 283.3 ? 32 - (int)((t - 283.3) * 16 / 66.7) : 32;
            lane_sprite(SPR_JUDGE, fx.judge_kind, JUDGE_X - gfx_sprite_w(SPR_JUDGE) / 2, LANE_Y - 22 + dy, alpha);
        }
    }
    prof_mark(PROF_HUD);
    /* counters, on top: the drumroll count stays 2.5 s after the last hit, then fades */
    if (fx.roll_count > 0) {
        int alpha = envelope(fx.roll_t, 0, 2532, 2698);
        int x = JUDGE_X + 30, y = LANE_Y - 36;
        int bw = gfx_sprite_w(SPR_ROLL_BALLOON), bh = gfx_sprite_h(SPR_ROLL_BALLOON);
        gfx_sprite_alpha(SPR_ROLL_BALLOON, 0, x, y, alpha);
        gfx_number_fx(SPR_NUM_ROLL, fx.roll_count, x + (bw ? bw : 60) / 2,
                      y + ((bh ? bh : 30) - gfx_sprite_h(SPR_NUM_ROLL)) / 2 + 1, ALIGN_CENTER, -2,
                      stretch_px(fx.roll_t), alpha);
    }
    {
        int alpha = j->is_balloon && fx.balloon_left > 0 ? 32 : envelope(fx.balloon_end_t, 0, 0, 166);
        int balloon = fx.kusudama ? SPR_POTATO_BALLOON : SPR_BURST_BALLOON;
        int digits = fx.kusudama ? SPR_NUM_POTATO : SPR_NUM_BALLOON;
        int bw = gfx_sprite_w(balloon), bh = gfx_sprite_h(balloon);
        /* the kusudama counter sits on the middle of the lane, as in Taikojiro */
        int x = fx.kusudama ? LANE_X + (GFX_W - LANE_X - bw) / 2 : JUDGE_X + 30;
        int y = fx.kusudama ? LANE_CY - bh / 2 : LANE_Y - 36;
        /* (on the sprite layer where that has the picture: it must cover the notes) */
        lane_sprite(balloon, 0, x, y, alpha);
        if (j->is_balloon && fx.balloon_left > 0) {
            lane_number(digits, fx.balloon_left, x + (bw ? bw : 60) / 2, y + ((bh ? bh : 30) - gfx_sprite_h(digits)) / 2,
                        -1, stretch_px(fx.balloon_t), 32);
        }
    }
    overlay_end();
    if (lyric && lyric_len && !paused) { /* the pause menu is where the lyric goes */
        gfx_text_outline(FONT_L, GFX_W / 2, LYRIC_Y, lyric, lyric_len, RGB(255, 255, 255), RGB(0, 0, 80), ALIGN_CENTER,
                         GFX_W - 16);
    }

    uint32_t us = (uint32_t)(pf_time_us() - t0);
    frame_us_sum += us;
    if (us > frame_us_max) {
        frame_us_max = us;
    }
    frame_count++;
    if ((uint32_t)act_n > peak_notes) {
        peak_notes = (uint32_t)act_n;
    }
    /* blended pixels count seven times: that is what they cost on the Pocket */
    uint32_t px = prof_counter[PROF_PX_COPY] + prof_counter[PROF_PX_REPAIR] + 7 * prof_counter[PROF_PX_BLEND];
    if (frame_count > 3 && px > peak_px) { /* the first frames paint the whole screen */
        peak_px = px;
        peak_ms = now;
    }
}

const scene scene_game = {game_enter, game_update, game_draw};
