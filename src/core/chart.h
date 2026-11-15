/* Chart model: what the TJA parser produces and the judge and renderer consume.
 * Mirrors YataiDON's Note / TimelineObject / NoteList / TempoMap, with fixed
 * storage supplied by the caller instead of deques. */
#ifndef TP_CHART_H
#define TP_CHART_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TP_BARLINE = 0,
    TP_DON = 1,
    TP_KAT = 2,
    TP_DON_L = 3,
    TP_KAT_L = 4,
    TP_ROLL_HEAD = 5,
    TP_ROLL_HEAD_L = 6,
    TP_BALLOON_HEAD = 7,
    TP_TAIL = 8,
    TP_KUSUDAMA = 9
} tp_note_type;

typedef enum { TP_NMSCROLL = 0, TP_BMSCROLL = 1, TP_HBSCROLL = 2 } tp_scroll_type;

enum {
    TP_NOTE_DISPLAY = 1 << 0,
    TP_NOTE_BAR_START = 1 << 1,    /* bar line: first of its bar */
    TP_NOTE_BRANCH_START = 1 << 2, /* bar line: first of a branch section */
    TP_NOTE_PARTNER = 1 << 3,      /* 'A' / 'B' notes */
    TP_NOTE_SUDDEN = 1 << 4,       /* sudden_appear_ms / sudden_moving_ms are set */
};

typedef struct {
    double hit_ms;
    double load_ms, unload_ms; /* filled by tp_chart_prepare() */
    double bpm;
    double scroll_x, scroll_y;
    double sudden_appear_ms, sudden_moving_ms;
    int32_t index;
    int32_t count; /* balloon / kusudama hits; TP_NO_COUNT for other notes */
    uint8_t type;  /* tp_note_type */
    uint8_t flags; /* TP_NOTE_* */
    uint8_t moji;  /* note name under the lane (YataiDON's "moji") */
    uint8_t pad;
} tp_note;

typedef enum {
    TP_EV_BPM,
    TP_EV_GOGO,
    TP_EV_BRANCH,  /* #BRANCHSTART: start of a branch condition */
    TP_EV_SECTION, /* #SECTION: reset the branch counters */
    TP_EV_JPOS,
    TP_EV_LYRIC,
    TP_EV_LEVELHOLD /* extension: not in YataiDON */
} tp_event_kind;

typedef struct {
    double start_ms, end_ms;
    uint8_t kind;      /* tp_event_kind */
    uint8_t gogo;      /* TP_EV_GOGO: on / off */
    uint8_t parts_ok;  /* TP_EV_BRANCH: the command had at least 3 comma-separated parts */
    char cond;         /* TP_EV_BRANCH: 'p', 'r', 's', or 0 for anything else */
    int16_t list;      /* 0 = master, n = branch section n-1 */
    int16_t seq;
    int32_t block;     /* TP_EV_BRANCH: which #BRANCHSTART, counted from 1 */
    float e_req, m_req; /* TP_EV_BRANCH */
    double bpm;         /* TP_EV_BPM */
    double judge_x, judge_y, delta_x, delta_y; /* TP_EV_JPOS */
    const char *text;   /* TP_EV_LYRIC: slice of the chart text */
    uint32_t text_len;
} tp_event;

typedef struct {
    double ms, beat, bpm; /* bpm 0 while a #DELAY holds the beat still */
} tp_tempo_point;

/* One branch path between a #N/#E/#M and the next one (or #BRANCHEND). */
typedef struct {
    uint8_t path; /* 0 normal, 1 expert, 2 master */
    int32_t block; /* which #BRANCHSTART it belongs to, counted from 1 */
    int32_t first_note, note_count;   /* in tp_chart.branch */
    int32_t first_event, event_count; /* in tp_chart.events */
} tp_section;

typedef struct {
    /* storage, set by the caller */
    tp_note *master;
    int master_cap;
    tp_note *branch;
    int branch_cap;
    tp_event *events;
    int event_cap;
    tp_section *sections;
    int section_cap;
    tp_tempo_point *tempo;
    int tempo_cap;

    int master_count, branch_count, event_count, section_count, tempo_count;
    int master_event_count; /* events[0 .. master_event_count) belong to the master list */
    double lead_bpm;
    uint8_t scroll_type; /* tp_scroll_type */
    bool overflow;       /* a storage limit was hit; the chart is incomplete */
    int warnings;
} tp_chart;

/* Tempo map (#BMSCROLL / #HBSCROLL scroll by beat count, not by time). */
double tp_tempo_beat_at(const tp_chart *c, double ms);
void tp_tempo_ms_within(const tp_chart *c, double lo, double hi, double *first, double *last);

/* YataiDON's 720p lane, used for the load times the judge depends on. */
#define TP_LANE_TRAVEL 948.0f
#define TP_LANE_NOTE_HALF_W 64
#define TP_LANE_JUDGE_X 414.0f

typedef struct {
    bool no_kusudama; /* play kusudama as balloons */
    bool inverse;     /* "abekobe": swap don and ka */
    float speed;      /* scroll multiplier, 1.0 = as charted */
    bool invisible;   /* "doron": notes are not drawn; bar lines and note names are */
    uint8_t random;   /* 1 "kimagure": a fifth of the notes swap don and ka; 2 "detarame": two fifths */
    uint32_t seed;    /* which ones */
} tp_modifiers;

#define TP_NO_COUNT INT32_MIN /* tp_note.count of notes that are not balloons */

/* A note past the end of the audio cannot be played in a beat-scrolled chart. */
bool tp_note_unplayable(const tp_note *n, int scroll_type, const double *audio_end_ms);

/* YataiDON's prepare_chart(): modifiers, note names, load/unload times.
 * `audio_end_ms` (chart time of the end of the audio) may be NULL. */
void tp_chart_prepare(tp_chart *c, const tp_modifiers *mods, const double *audio_end_ms);

/* Shinuchi score of one GOOD (YataiDON calculate_base_score), over the master
 * list plus every master-branch section. */
int tp_chart_base_score(const tp_chart *c, const double *audio_end_ms);

#endif
