/* Shared state of the game: scenes, input, settings, the loaded song. */
#ifndef TP_GAME_H
#define TP_GAME_H

#include <stdbool.h>
#include <stdint.h>

#include "../core/judge.h"
#include "../core/tja.h"
#include "../platform/platform.h"
#include "assets.h"
#include "gfx.h"
#include "library.h"
#include "prof.h"

/* ---------- scenes ---------- */
typedef struct {
    void (*enter)(void);
    void (*update)(void); /* logic; may redraw the background layer */
    void (*draw)(void);   /* what moves, on top of the background layer */
} scene;

extern const scene scene_boot, scene_select, scene_game, scene_result, scene_settings, scene_calibrate, scene_stats;
void app_goto(const scene *next);
extern uint32_t app_frame_count;

/* ---------- input ---------- */
#define INPUT_EVENTS 32
typedef struct {
    uint32_t held;    /* buttons down now */
    uint32_t pressed; /* press edges this frame */
    uint32_t repeat;  /* press edges plus auto-repeat for held direction buttons */
    pf_input_event events[INPUT_EVENTS];
    int event_count;
} input_state;
extern input_state in;

enum { DRUM_DON_L, DRUM_DON_R, DRUM_KA_L, DRUM_KA_R, DRUM_KEYS };

/* ---------- settings and scores (persisted when the platform can) ---------- */
#define SCORE_MAX 2457 /* 20 bytes each: the size the score table has had since the first save file */
enum { SORT_TITLE = 1, SORT_DIFFICULTY = 2 };
enum { RANDOM_OFF = 0, RANDOM_KIMAGURE = 1, RANDOM_DETARAME = 2 };
enum { CROWN_NONE = 0, CROWN_PLAYED = 1, CROWN_CLEAR = 2, CROWN_FULL_COMBO = 3, CROWN_ALL_GOOD = 4 };

typedef struct {
    uint32_t magic, version;
    int16_t audio_offset_ms; /* how late sound reaches the ear; notes are drawn that much later */
    int16_t input_offset_ms; /* how late presses arrive; taken off every hit's time */
    uint8_t speed_x10;       /* scroll speed, 10 = 1.0 */
    uint8_t music_volume, hit_volume; /* 0..100 */
    uint8_t auto_play;
    uint8_t unused_effects; /* was a setting until 0.1.1 */
    uint8_t last_course;
    uint8_t show_fps;    /* frame rate in a corner of every screen */
    uint8_t debounce_ms; /* a button is ignored for this long after each of its edges */
    uint32_t map[DRUM_KEYS]; /* button masks of don left/right, ka left/right */
    uint32_t library_id;     /* build id the position below belongs to */
    uint32_t last_song;
    uint8_t song_order; /* SORT_*: how a folder's songs are listed */
    /* modifiers, as in YataiDON; they can be combined */
    uint8_t mod_invisible; /* "doron": notes are not drawn */
    uint8_t mod_inverse;   /* "abekobe": don and ka swapped */
    uint8_t mod_random;    /* RANDOM_* */
    uint32_t reserved2[3];
} settings_t;

/* The best play of a course. score, the counts and the rolls are those of the play with
 * the highest score; max_combo and crown are the best ever reached. */
#define SCORE_NO_COUNTS 0xFFFF /* in `good`: a best from before the counts were kept */
typedef struct {
    uint32_t chart_crc;
    uint32_t score;
    uint16_t max_combo;
    uint8_t course, crown;
    uint16_t good, ok, bad, rolls;
} score_entry;

/* Lifetime counters, kept in the save next to the scores. */
typedef struct {
    uint32_t don_hits, ka_hits; /* drum hits by the player (not auto-play), in songs */
    uint32_t active_s;          /* time the game has been running */
    uint32_t play_s;            /* of that, time inside songs, not paused */
    uint32_t songs_finished;
    uint32_t reserved[3];
} stats_t;
extern stats_t stats;
void stats_tick(bool in_song); /* once per frame */

extern settings_t settings;
void settings_load(void);
void settings_save(void);
void settings_defaults(void);
void settings_apply_audio(void); /* volumes, the hardware hit-sound button map, debounce */
const score_entry *score_find(uint32_t chart_crc, int course);
struct result_s;
bool score_submit(uint32_t chart_crc, int course, const struct result_s *r, int crown); /* true: new record */
const char *modifier_tags(void); /* "Doron Abekobe Kimagure", "" with none on */

/* ---------- the loaded song ---------- */
typedef enum { SONG_IDLE, SONG_LOADING, SONG_READY, SONG_ERROR } song_state;

typedef struct {
    uint32_t index; /* in the library */
    int course;
    const lib_song *info;
    tp_tja tja;
    tp_chart chart;
    tp_judge judge;
    uint32_t parse_us; /* time the parser took, for docs/performance.md */
    double offset_ms;  /* chart time = audio time + offset_ms (the TJA's OFFSET) */
    double audio_end_ms;
    bool has_audio_end;
} song_t;

extern song_t song;
void song_load(uint32_t index, int course); /* asynchronous; poll until READY or ERROR */
song_state song_poll(void);
const char *song_error(void);
void song_play(void);              /* start from the lead-in */
void song_pause(bool paused);
void song_unload(void);
double song_now_ms(void);          /* chart time for drawing and for the judge's frame */
double song_event_ms(int64_t counter); /* chart time of an input edge */
bool song_audio_done(void);

/* Song-select preview (from DEMOSTART). */
void preview_request(uint32_t index); /* starts after a short delay; replaces any earlier request */
void preview_stop(void);
void preview_poll(void);

/* Low-level audio stream shared by the song, the preview and the calibration click. */
void stream_poll(void);
bool stream_idle(void); /* no library read in flight */

bool game_is_playing(void); /* in a song, not paused or loading */

/* ---------- result of the last play ---------- */
typedef struct result_s {
    uint32_t score;
    int good, ok, bad, max_combo, rolls, gauge_percent;
    bool cleared, rainbow, full_combo, all_good, new_record, auto_play;
    uint32_t avg_frame_us, max_frame_us; /* time spent per frame in update + draw */
} result_t;
extern result_t last_result;

/* ---------- shared drawing helpers ---------- */
void ui_panel(int x, int y, int w, int h, uint16_t color, int alpha);
/* word-wrapped text; returns the y below the last line */
int ui_text_wrap(int font, int x, int y, int w, int line_h, const char *text, uint16_t color, int align);
int ui_corner_x(void); /* right edge for text in the bottom right corner: left of the frame rate counter */
void ui_sound_move(void);
void ui_sound_ok(void);

#endif
