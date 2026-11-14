/* Judge: a port of YataiDON's JudgeCore and GaugeCore (src/libs/judge.cpp).
 * Timing windows, lanes, drumrolls, balloons, kusudama, shinuchi scoring, the
 * soul gauge and branch decisions. Pure logic: time comes in as chart
 * milliseconds, results go out as counters and a queue of events to draw. */
#ifndef TP_JUDGE_H
#define TP_JUDGE_H

#include "chart.h"

/* exact constants of YataiDON's Timing namespace (ms, as float) */
#define TP_TIMING_GOOD 25.0250015258789f
#define TP_TIMING_OK 75.0750045776367f
#define TP_TIMING_BAD 108.441665649414f
#define TP_TIMING_GOOD_EASY 41.7083358764648f
#define TP_TIMING_OK_EASY 108.441665649414f
#define TP_TIMING_BAD_EASY 125.125f

#define TP_JUDGE_MAX_SECTIONS 2048
#define TP_JUDGE_EVENTS 64

typedef enum { TP_GOOD = 0, TP_OK = 1, TP_BAD = 2 } tp_judgment;
typedef enum { TP_DRUM_NONE = 0, TP_DRUM_DON = 1, TP_DRUM_KAT = 2 } tp_drum;
typedef enum { TP_BRANCH_NORMAL = 0, TP_BRANCH_EXPERT = 1, TP_BRANCH_MASTER = 2 } tp_branch;

typedef struct {
    double good_points, ok_points, bad_points;
    double points;
    int max_points, clear_points;
    int difficulty;
} tp_gauge;

void tp_gauge_init(tp_gauge *g, int total_notes, int difficulty, int level);
static inline bool tp_gauge_clear(const tp_gauge *g) { return g->points >= g->clear_points; }
static inline bool tp_gauge_rainbow(const tp_gauge *g) { return g->points >= g->max_points; }
int tp_gauge_percent(const tp_gauge *g);

/* What the judge did, for the game to show and sound. */
typedef enum {
    TP_JE_JUDGMENT,       /* note, judgment, drum (TP_DRUM_NONE: the note passed unhit) */
    TP_JE_NOTE_CORRECT,   /* note: a GOOD/OK hit or a burst balloon; stop drawing it */
    TP_JE_SCORE,          /* value: points just added */
    TP_JE_DRUMROLL_HIT,   /* drum; value: hits on this roll so far */
    TP_JE_BALLOON_HIT,    /* note; value: hits so far; flag: this hit burst it */
    TP_JE_BALLOON_START,
    TP_JE_BALLOON_MISSED,
    TP_JE_BRANCH,         /* value: tp_branch chosen; section: index merged, or -1 */
    TP_JE_AUTO_HIT        /* auto-play hit a drum: drum; flag: big note */
} tp_judge_event_kind;

typedef struct {
    uint8_t kind, judgment, drum, flag;
    int32_t value;
    int32_t section;
    const tp_note *note;
} tp_judge_event;

typedef struct {
    const tp_note **v;
    int head, tail, cap;
} tp_lane;

typedef struct {
    /* configuration */
    const tp_chart *chart;
    int difficulty;
    bool auto_play;
    bool extensions; /* #LEVELHOLD and the 's' branch condition (not in YataiDON) */

    /* results */
    int good_count, ok_count, bad_count;
    int combo, max_combo;
    int score, total_drumroll;
    tp_gauge gauge;
    int judgeable_note_count;
    double chart_end_ms;
    int base_score;

    /* state the renderer reads */
    bool is_drumroll, is_balloon;
    int curr_drumroll_count, curr_balloon_count;
    int kusudama_shared_hits;
    uint8_t current_branch; /* tp_branch last chosen */
    int branch_decisions;

    /* internals */
    tp_lane don, kat, other;
    const tp_event **timeline;
    int tl_head, tl_tail, tl_cap;
    uint16_t path_sections[3][TP_JUDGE_MAX_SECTIONS];
    int path_head[3], path_count[3];
    long long kusudama_key;
    bool is_branch, level_hold;
    float branch_e_req, branch_m_req;
    double branch_end_ms;
    float branch_p_count;
    int branch_r_count, branch_note_count;
    char branch_condition;
    int branch_score_base;
    double last_subdivision;

    tp_judge_event events[TP_JUDGE_EVENTS];
    unsigned ev_head, ev_tail;
} tp_judge;

/* `lane_buf` needs room for 3 * (master + branch note count) pointers,
 * `timeline_buf` for the chart's event count. `audio_end_ms` may be NULL. */
void tp_judge_load(tp_judge *j, const tp_chart *chart, int difficulty, int level, bool auto_play,
                   bool extensions, const double *audio_end_ms, const tp_note **lane_buf, int lane_cap,
                   const tp_event **timeline_buf, int timeline_cap);

/* One frame: begin, then every hit (and/or auto-play), then end. */
void tp_judge_begin_frame(tp_judge *j, double ms);
void tp_judge_hit(tp_judge *j, double ms, tp_drum drum);
void tp_judge_autoplay(tp_judge *j, double ms, double bpm);
void tp_judge_end_frame(tp_judge *j, double ms);

bool tp_judge_next_event(tp_judge *j, tp_judge_event *out);

static inline bool tp_judge_finished(const tp_judge *j) {
    return j->don.head == j->don.tail && j->kat.head == j->kat.tail && j->other.head == j->other.tail &&
           !j->is_branch && j->path_head[0] == j->path_count[0] && j->path_head[1] == j->path_count[1] &&
           j->path_head[2] == j->path_count[2];
}

#endif
