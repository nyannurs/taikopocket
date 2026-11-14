/* Settings and high scores. One block, read at boot and written back through
 * pf_save_write() when something changes. Platforms without working storage
 * (pf_save_supported() false) simply run with defaults every time. */
#include <stdio.h>
#include <string.h>

#include "game.h"

#define SAVE_MAGIC 0x56535054u /* "TPSV" */
#define SAVE_VERSION 3 /* 2: statistics at the end. 3: judge counts in every score, 20 bytes instead of 12 */

settings_t settings;
stats_t stats;

/* The file has had one size since version 2: the score table is 49152 bytes, once 4096
 * entries of 12 bytes, now 2457 of 20. Growing a file on the Pocket's SD card is a
 * step that has never been tried on the device, so it is not needed. */
typedef struct {
    uint32_t chart_crc;
    uint32_t score;
    uint16_t max_combo;
    uint8_t course, crown;
} score_entry_v2;
#define SCORE_BYTES (4096 * sizeof(score_entry_v2))

static struct {
    settings_t settings;
    uint32_t score_count;
    union {
        score_entry scores[SCORE_MAX];
        uint8_t bytes[SCORE_BYTES];
    };
    stats_t stats; /* since version 2; a version 1 file ends before it */
} save;
_Static_assert(sizeof(score_entry) == 20 && sizeof(score_entry) * SCORE_MAX <= SCORE_BYTES, "score table size");

void settings_defaults(void) {
    memset(&settings, 0, sizeof settings);
    settings.magic = SAVE_MAGIC;
    settings.version = SAVE_VERSION;
    settings.speed_x10 = 10;
    settings.music_volume = 80;
    settings.hit_volume = 100;
    settings.last_course = 3;
    settings.show_fps = 1;
    settings.debounce_ms = 10;
    settings.song_order = SORT_TITLE;
    settings.map[DRUM_DON_L] = PF_BTN_DOWN | PF_BTN_RIGHT | PF_KEY_DON_L;
    settings.map[DRUM_DON_R] = PF_BTN_Y | PF_BTN_B | PF_KEY_DON_R;
    settings.map[DRUM_KA_L] = PF_BTN_UP | PF_BTN_LEFT | PF_BTN_L1 | PF_KEY_KA_L;
    settings.map[DRUM_KA_R] = PF_BTN_X | PF_BTN_A | PF_BTN_R1 | PF_KEY_KA_R;
}

void settings_load(void) {
    settings_defaults();
    memset(&stats, 0, sizeof stats);
    bool ok = false;
    if (pf_save_supported()) {
        ok = pf_save_read(&save, sizeof save);
        if (!ok) { /* a version 1 file has no statistics at its end */
            memset(&save, 0, sizeof save);
            ok = pf_save_read(&save, (uint32_t)((char *)&save.stats - (char *)&save)) && save.settings.version == 1;
        }
    }
    ok = ok && save.settings.magic == SAVE_MAGIC && save.settings.version >= 1 && save.settings.version <= SAVE_VERSION &&
         save.score_count <= (save.settings.version < 3 ? 4096u : (uint32_t)SCORE_MAX);
    if (!ok) {
        memset(&save, 0, sizeof save);
        return;
    }
    if (save.settings.version == 1) { /* the two bytes were reserved and zero */
        save.settings.show_fps = settings.show_fps;
        save.settings.debounce_ms = settings.debounce_ms;
        memset(&save.stats, 0, sizeof save.stats);
    }
    if (save.settings.version < 3) { /* scores without counts: the same bests in the wider entries */
        static score_entry_v2 old[4096];
        memcpy(old, save.bytes, sizeof old);
        memset(save.bytes, 0, sizeof save.bytes);
        if (save.score_count > SCORE_MAX) {
            save.score_count = SCORE_MAX;
        }
        for (uint32_t i = 0; i < save.score_count; i++) {
            score_entry *e = &save.scores[i];
            e->chart_crc = old[i].chart_crc;
            e->score = old[i].score;
            e->max_combo = old[i].max_combo;
            e->course = old[i].course;
            e->crown = old[i].crown;
            e->good = SCORE_NO_COUNTS;
        }
        /* three bytes that were reserved and zero are now the modifiers */
    }
    save.settings.version = SAVE_VERSION;
    if (save.settings.song_order != SORT_DIFFICULTY) { /* "as in the folder" is gone */
        save.settings.song_order = SORT_TITLE;
    }
    if (save.settings.mod_random > RANDOM_DETARAME) {
        save.settings.mod_random = RANDOM_OFF;
    }
    /* a damaged file must not send the game out of its tables */
    if (save.settings.last_course >= LIB_COURSES) {
        save.settings.last_course = 3;
    }
    if (save.settings.speed_x10 < 5 || save.settings.speed_x10 > 40) {
        save.settings.speed_x10 = 10;
    }
    if (save.settings.music_volume > 100) {
        save.settings.music_volume = 80;
    }
    if (save.settings.hit_volume > 100) {
        save.settings.hit_volume = 100;
    }
    settings = save.settings;
    stats = save.stats;
}

const char *modifier_tags(void) {
    static char tags[40];
    snprintf(tags, sizeof tags, "%s%s%s", settings.mod_invisible ? "Doron " : "", settings.mod_inverse ? "Abekobe " : "",
             settings.mod_random == RANDOM_KIMAGURE ? "Kimagure " : settings.mod_random == RANDOM_DETARAME ? "Detarame " : "");
    size_t n = strlen(tags);
    if (n) {
        tags[n - 1] = 0;
    }
    return tags;
}

void settings_save(void) {
    save.settings = settings;
    save.stats = stats;
    if (pf_save_supported()) {
        pf_save_write(&save, sizeof save);
    }
}

/* Lifetime counters: whole seconds are added as frames go by. */
void stats_tick(bool in_song) {
    static uint64_t last_us;
    static uint32_t active_us, play_us;
    uint64_t t = pf_time_us();
    uint32_t d = last_us ? (uint32_t)(t - last_us) : 0;
    last_us = t;
    if (d > 1000000) { /* a long stall (loading, a debugger) is not play time */
        d = 1000000;
    }
    active_us += d;
    while (active_us >= 1000000) {
        active_us -= 1000000;
        stats.active_s++;
    }
    if (in_song) {
        play_us += d;
        while (play_us >= 1000000) {
            play_us -= 1000000;
            stats.play_s++;
        }
    }
}

void settings_apply_audio(void) {
    pf_volume((uint32_t)settings.music_volume * 256 / 100, (uint32_t)settings.hit_volume * 256 / 100);
    pf_hit_map(settings.map[DRUM_DON_L] | settings.map[DRUM_DON_R], settings.map[DRUM_KA_L] | settings.map[DRUM_KA_R]);
    pf_debounce(settings.debounce_ms);
}

const score_entry *score_find(uint32_t chart_crc, int course) {
    for (uint32_t i = 0; i < save.score_count; i++) {
        if (save.scores[i].chart_crc == chart_crc && save.scores[i].course == course) {
            return &save.scores[i];
        }
    }
    return NULL;
}

static uint16_t count16(int v) { return (uint16_t)(v < 0 ? 0 : v > 65534 ? 65534 : v); }

bool score_submit(uint32_t chart_crc, int course, const result_t *r, int crown) {
    score_entry *e = (score_entry *)score_find(chart_crc, course);
    bool first = !e;
    if (!e) {
        if (save.score_count >= SCORE_MAX) {
            return false;
        }
        e = &save.scores[save.score_count++];
        memset(e, 0, sizeof *e);
        e->chart_crc = chart_crc;
        e->course = (uint8_t)course;
    }
    bool better = first || r->score > e->score;
    if (better) { /* the counts are those of the play the score is from */
        e->score = r->score;
        e->good = count16(r->good);
        e->ok = count16(r->ok);
        e->bad = count16(r->bad);
        e->rolls = count16(r->rolls);
    }
    if (r->max_combo > e->max_combo) {
        e->max_combo = count16(r->max_combo);
    }
    if (crown > e->crown) {
        e->crown = (uint8_t)crown;
    }
    settings_save();
    return better && r->score > 0;
}
