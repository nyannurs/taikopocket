/* Port of YataiDON's JudgeCore / GaugeCore (GPL-3.0, src/libs/judge.cpp at
 * 309d7b55). Names and statement order follow the original. Scoring is
 * shinuchi only (YataiDON's default); its "gen3" mode is not ported. */
#include "judge.h"

#include <math.h>
#include <string.h>

/* ---------- gauge ---------- */

#define POINTS_EPS 1e-6

/* soul_percent, ok_multiplier, bad_multiplier per course and level (stars) */
static const double GAUGE_TABLE[4][10][3] = {
    /* Easy */
    {{60.0, 0.75, -0.5}, {63.333, 0.75, -0.5}, {63.333, 0.75, -0.5}, {73.333, 0.75, -0.5}, {73.333, 0.75, -0.5},
     {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
    /* Normal */
    {{65.6, 0.75, -0.5}, {65.6, 0.75, -0.5}, {69.5, 0.75, -0.5}, {70.3, 0.75, -0.75}, {75.0, 0.75, -1.0},
     {75.0, 0.75, -1.0}, {75.0, 0.75, -1.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
    /* Hard */
    {{77.6, 0.75, -0.75}, {77.6, 0.75, -0.75}, {72.5, 0.75, -1.0}, {69.15, 0.75, -1.17}, {67.5, 0.75, -1.25},
     {68.74, 0.75, -1.25}, {68.74, 0.75, -1.25}, {68.74, 0.75, -1.25}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}},
    /* Oni */
    {{70.75, 0.5, -1.6}, {70.75, 0.5, -1.6}, {70.75, 0.5, -1.6}, {70.75, 0.5, -1.6}, {70.75, 0.5, -1.6},
     {70.75, 0.5, -1.6}, {70.75, 0.5, -1.6}, {70.0, 0.5, -2.0}, {76.75, 0.5, -2.0}, {76.75, 0.5, -2.0}},
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void tp_gauge_init(tp_gauge *g, int total_notes, int difficulty_param, int level) {
    g->difficulty = clampi(difficulty_param, 0, 3);
    g->max_points = 10000;
    g->clear_points = g->difficulty <= 0 ? 6000 : g->difficulty <= 2 ? 7000 : 8000;
    const double *row = GAUGE_TABLE[g->difficulty][clampi(level - 1, 0, 9)];
    const double denom = (total_notes > 1 ? total_notes : 1) * row[0];
    g->good_points = (denom > 0.0) ? 1000000.0 / denom : 0;
    g->ok_points = g->good_points * row[1];
    g->bad_points = g->good_points * row[2];
    g->points = 0;
}

static void apply_points_clamped(tp_gauge *g, double delta) {
    double p = g->points + delta;
    g->points = p < 0.0 ? 0.0 : p > g->max_points ? g->max_points : p;
    if (fabs(g->points - g->max_points) < POINTS_EPS) {
        g->points = g->max_points;
    }
    if (fabs(g->points - g->clear_points) < POINTS_EPS) {
        g->points = g->clear_points;
    }
}

int tp_gauge_percent(const tp_gauge *g) { return (int)floor((g->points + POINTS_EPS) / g->max_points * 100.0); }

/* ---------- lanes (YataiDON's std::deque<Note>) ---------- */

static bool lane_empty(const tp_lane *l) { return l->head == l->tail; }
static int lane_size(const tp_lane *l) { return l->tail - l->head; }
static const tp_note *lane_front(const tp_lane *l) { return l->v[l->head]; }
static void lane_pop(tp_lane *l) { l->head++; }

static void lane_push(tp_lane *l, const tp_note *n) {
    if (l->tail < l->cap) {
        l->v[l->tail++] = n;
    }
}

/* insert before the first note that is not earlier (std::ranges::lower_bound by hit_ms) */
static void lane_insert(tp_lane *l, const tp_note *n) {
    if (l->tail >= l->cap) {
        if (l->head == 0) {
            return;
        }
        memmove(l->v, l->v + l->head, (size_t)(l->tail - l->head) * sizeof *l->v);
        l->tail -= l->head;
        l->head = 0;
    }
    int lo = l->head, hi = l->tail;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (l->v[mid]->hit_ms < n->hit_ms) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    memmove(l->v + lo + 1, l->v + lo, (size_t)(l->tail - lo) * sizeof *l->v);
    l->v[lo] = n;
    l->tail++;
}

static bool same_note(const tp_note *a, const tp_note *b) { return a->hit_ms == b->hit_ms && a->index == b->index; }

/* ---------- events for the game ---------- */

static void emit(tp_judge *j, int kind, const tp_note *note, int judgment, int drum, int flag, int value) {
    unsigned next = (j->ev_head + 1) % TP_JUDGE_EVENTS;
    if (next == j->ev_tail) {
        j->ev_tail = (j->ev_tail + 1) % TP_JUDGE_EVENTS; /* full: the oldest is lost */
    }
    tp_judge_event *e = &j->events[j->ev_head];
    e->kind = (uint8_t)kind;
    e->judgment = (uint8_t)judgment;
    e->drum = (uint8_t)drum;
    e->flag = (uint8_t)flag;
    e->value = value;
    e->section = -1;
    e->note = note;
    j->ev_head = next;
}

bool tp_judge_next_event(tp_judge *j, tp_judge_event *out) {
    if (j->ev_head == j->ev_tail) {
        return false;
    }
    *out = j->events[j->ev_tail];
    j->ev_tail = (j->ev_tail + 1) % TP_JUDGE_EVENTS;
    return true;
}

/* ---------- judge timeline: #BRANCHSTART and #SECTION entries ---------- */

static bool is_judge_timeline_entry(const tp_judge *j, const tp_event *e) {
    return e->kind == TP_EV_BRANCH || e->kind == TP_EV_SECTION || (j->extensions && e->kind == TP_EV_LEVELHOLD);
}

/* append the judge entries of events[first .. first+count), then stable sort by start time */
static void timeline_add(tp_judge *j, const tp_event *events, int count) {
    if (j->tl_head > 0) {
        memmove(j->timeline, j->timeline + j->tl_head, (size_t)(j->tl_tail - j->tl_head) * sizeof *j->timeline);
        j->tl_tail -= j->tl_head;
        j->tl_head = 0;
    }
    for (int i = 0; i < count; i++) {
        if (is_judge_timeline_entry(j, &events[i]) && j->tl_tail < j->tl_cap) {
            j->timeline[j->tl_tail++] = &events[i];
        }
    }
    for (int i = j->tl_head + 1; i < j->tl_tail; i++) {
        const tp_event *e = j->timeline[i];
        int k = i;
        while (k > j->tl_head && j->timeline[k - 1]->start_ms > e->start_ms) {
            j->timeline[k] = j->timeline[k - 1];
            k--;
        }
        j->timeline[k] = e;
    }
}

/* ---------- load_chart ---------- */

void tp_judge_load(tp_judge *j, const tp_chart *c, int difficulty, int level, bool auto_play, bool extensions,
                   const double *audio_end_ms, const tp_note **lane_buf, int lane_cap,
                   const tp_event **timeline_buf, int timeline_cap) {
    memset(j, 0, sizeof *j);
    j->chart = c;
    j->difficulty = difficulty;
    j->auto_play = auto_play;
    j->extensions = extensions;
    j->kusudama_key = -1;
    j->last_subdivision = -1;
    int each = lane_cap / 3;
    j->don.v = lane_buf;
    j->kat.v = lane_buf + each;
    j->other.v = lane_buf + 2 * each;
    j->don.cap = j->kat.cap = j->other.cap = each;
    j->timeline = timeline_buf;
    j->tl_cap = timeline_cap;

    for (int i = 0; i < c->master_count; i++) {
        const tp_note *note = &c->master[i];
        if (tp_note_unplayable(note, c->scroll_type, audio_end_ms)) {
            continue;
        }
        if (note->type == TP_DON || note->type == TP_DON_L) {
            lane_push(&j->don, note);
        } else if (note->type == TP_KAT || note->type == TP_KAT_L) {
            lane_push(&j->kat, note);
        } else if (note->type != TP_BARLINE) {
            lane_push(&j->other, note);
        }
        j->chart_end_ms = fmax(j->chart_end_ms, note->hit_ms);
    }
    int gauge_total_notes = 0;
    for (int i = 0; i < c->master_count; i++) {
        const tp_note *note = &c->master[i];
        if (!tp_note_unplayable(note, c->scroll_type, audio_end_ms) && note->type >= TP_DON &&
            note->type <= TP_KAT_L) {
            gauge_total_notes++;
        }
    }
    for (int s = 0; s < c->section_count; s++) {
        const tp_section *sec = &c->sections[s];
        if (j->path_count[sec->path] < TP_JUDGE_MAX_SECTIONS) {
            j->path_sections[sec->path][j->path_count[sec->path]++] = (uint16_t)s;
        }
        for (int i = 0; i < sec->note_count; i++) {
            const tp_note *note = &c->branch[sec->first_note + i];
            j->chart_end_ms = fmax(j->chart_end_ms, note->hit_ms);
            if (sec->path == 2 && note->type >= TP_DON && note->type <= TP_KAT_L) {
                gauge_total_notes++; /* the gauge counts the master route */
            }
        }
    }
    timeline_add(j, c->events, c->master_event_count);

    int stars = level;
    if (stars == 0) {
        j->difficulty = 3;
        stars = 10;
    }
    j->judgeable_note_count = gauge_total_notes;
    tp_gauge_init(&j->gauge, gauge_total_notes, j->difficulty, stars);
    j->base_score = tp_chart_base_score(c, audio_end_ms);
}

/* ---------- branches ---------- */

static void handle_judge_entry(tp_judge *j, double ms_from_start, const tp_event *entry) {
    if (entry->kind == TP_EV_LEVELHOLD) { /* extension */
        j->level_hold = true;
        return;
    }
    if (entry->kind != TP_EV_BRANCH) { /* #SECTION */
        j->branch_p_count = 0;
        j->branch_r_count = 0;
        j->branch_note_count = 0;
        j->branch_score_base = j->score;
        return;
    }
    if (!entry->parts_ok || j->is_branch) {
        return;
    }
    j->is_branch = true;
    j->branch_condition = entry->cond;

    /* decided when the next section's first note comes on screen */
    double branch_condition_end_time = ms_from_start;
    for (int path = 2; path >= 0; path--) { /* branch_m, branch_e, branch_n */
        if (j->path_head[path] < j->path_count[path]) {
            const tp_section *sec = &j->chart->sections[j->path_sections[path][j->path_head[path]]];
            if (sec->note_count > 0) {
                branch_condition_end_time = j->chart->branch[sec->first_note].load_ms;
                break;
            }
        }
    }
    if (j->branch_condition == 'r' || j->branch_condition == 'p' ||
        (j->extensions && j->branch_condition == 's')) {
        j->branch_e_req = entry->e_req;
        j->branch_m_req = entry->m_req;
        j->branch_end_ms = branch_condition_end_time;
    }
}

static void handle_judge_timeline(tp_judge *j, double ms_from_start) {
    /* everything due this frame, newest first */
    int first = j->tl_head;
    while (j->tl_head < j->tl_tail && ms_from_start > j->timeline[j->tl_head]->start_ms) {
        j->tl_head++;
    }
    for (int i = j->tl_head - 1; i >= first; i--) {
        handle_judge_entry(j, ms_from_start, j->timeline[i]);
    }
}

static void merge_branch_section(tp_judge *j, int section) {
    const tp_chart *c = j->chart;
    const tp_section *sec = &c->sections[section];
    timeline_add(j, c->events + sec->first_event, sec->event_count);
    for (int i = 0; i < sec->note_count; i++) {
        const tp_note *note = &c->branch[sec->first_note + i];
        if (note->type == TP_DON || note->type == TP_DON_L) {
            lane_insert(&j->don, note);
        } else if (note->type == TP_KAT || note->type == TP_KAT_L) {
            lane_insert(&j->kat, note);
        } else if (note->type != TP_BARLINE) {
            lane_insert(&j->other, note);
        }
    }
}

static void evaluate_branch(tp_judge *j, double current_ms) {
    float e_req = j->branch_e_req;
    float m_req = j->branch_m_req;
    if (current_ms < j->branch_end_ms) {
        return;
    }
    j->is_branch = false;
    if (j->branch_condition == 'p') {
        int pct = j->branch_note_count != 0 ? (int)((double)j->branch_p_count / j->branch_note_count * 100) : 0;
        j->branch_p_count = (float)(pct > 100 ? 100 : pct < 0 ? 0 : pct);
    } else if (j->branch_condition == 'r') {
        j->branch_r_count = j->curr_drumroll_count > j->branch_r_count ? j->curr_drumroll_count : j->branch_r_count;
    }
    float count = j->branch_condition == 'p' ? j->branch_p_count : (float)j->branch_r_count;
    if (j->extensions && j->branch_condition == 's') {
        count = (float)(j->score - j->branch_score_base); /* extension: score since #SECTION */
    }
    int chosen = (count >= e_req && count < m_req && e_req >= 0) ? TP_BRANCH_EXPERT
                 : (count >= m_req)                              ? TP_BRANCH_MASTER
                                                                 : TP_BRANCH_NORMAL;
    if (j->extensions && j->level_hold) {
        chosen = j->current_branch; /* extension: #LEVELHOLD */
    }
    j->current_branch = (uint8_t)chosen;
    j->branch_decisions++;

    int merged = -1;
    if (j->path_head[chosen] < j->path_count[chosen]) {
        merged = j->path_sections[chosen][j->path_head[chosen]];
        j->path_head[chosen]++;
        merge_branch_section(j, merged);
    }
    for (int path = 0; path < 3; path++) {
        if (path != chosen && j->path_head[path] < j->path_count[path]) {
            j->path_head[path]++;
        }
    }
    emit(j, TP_JE_BRANCH, NULL, 0, 0, 0, chosen);
    j->events[(j->ev_head + TP_JUDGE_EVENTS - 1) % TP_JUDGE_EVENTS].section = merged;
    j->branch_p_count = 0;
    j->branch_r_count = 0;
    j->branch_note_count = 0;
    j->branch_score_base = j->score;
}

/* ---------- per frame ---------- */

static void play_note_manager(tp_judge *j, double current_ms) {
    const double miss_window = (j->difficulty <= 1) ? TP_TIMING_BAD_EASY : TP_TIMING_BAD;
    tp_lane *lanes[2] = {&j->don, &j->kat};
    for (int l = 0; l < 2; l++) {
        tp_lane *lane = lanes[l];
        while (!lane_empty(lane) && lane_front(lane)->hit_ms + miss_window < current_ms) {
            j->combo = 0;
            j->bad_count++;
            apply_points_clamped(&j->gauge, j->gauge.bad_points);
            const tp_note *missed = lane_front(lane);
            lane_pop(lane);
            j->branch_note_count++;
            emit(j, TP_JE_JUDGMENT, missed, TP_BAD, TP_DRUM_NONE, 0, 0);
        }
    }
    if (lane_empty(&j->other)) {
        return;
    }
    const tp_note *note = lane_front(&j->other);
    if (note->hit_ms <= current_ms) {
        if (note->type == TP_ROLL_HEAD || note->type == TP_ROLL_HEAD_L) {
            j->is_drumroll = true;
        } else if (note->type == TP_BALLOON_HEAD || note->type == TP_KUSUDAMA) {
            if (!j->is_balloon) {
                emit(j, TP_JE_BALLOON_START, note, 0, 0, 0, 0);
            }
            j->is_balloon = true;
        } else if (note->type == TP_TAIL) {
            lane_pop(&j->other);
            j->is_drumroll = false;
            j->is_balloon = false;
            j->curr_drumroll_count = 0;
            j->curr_balloon_count = 0;
            return;
        }
        if (lane_size(&j->other) < 2) {
            return;
        }
        const tp_note *tail = j->other.v[j->other.head + 1];
        if (tail->hit_ms <= current_ms) {
            const bool balloon_missed = note->type == TP_BALLOON_HEAD; /* a burst one is gone already */
            lane_pop(&j->other);
            lane_pop(&j->other);
            j->is_drumroll = false;
            j->is_balloon = false;
            j->curr_drumroll_count = 0;
            j->curr_balloon_count = 0;
            if (balloon_missed) {
                emit(j, TP_JE_BALLOON_MISSED, note, 0, 0, 0, 0);
            }
        }
    }
}

void tp_judge_begin_frame(tp_judge *j, double ms_from_start) {
    play_note_manager(j, ms_from_start);
    handle_judge_timeline(j, ms_from_start);
}

void tp_judge_end_frame(tp_judge *j, double ms_from_start) {
    if (j->is_branch) {
        evaluate_branch(j, ms_from_start);
    }
}

static void note_correct(tp_judge *j, const tp_note *note) {
    if (!lane_empty(&j->don) && same_note(lane_front(&j->don), note)) {
        lane_pop(&j->don);
    } else if (!lane_empty(&j->kat) && same_note(lane_front(&j->kat), note)) {
        lane_pop(&j->kat);
    } else if (!lane_empty(&j->other) && same_note(lane_front(&j->other), note)) {
        lane_pop(&j->other);
    }
    if ((note->type == TP_BALLOON_HEAD || note->type == TP_KUSUDAMA) && !lane_empty(&j->other)) {
        lane_pop(&j->other);
    }
    if (note->type < TP_BALLOON_HEAD) {
        j->combo++;
        if (j->combo > j->max_combo) {
            j->max_combo = j->combo;
        }
    }
    emit(j, TP_JE_NOTE_CORRECT, note, 0, 0, 0, 0);
}

static void check_drumroll(tp_judge *j, tp_drum drum_type) {
    j->curr_drumroll_count++;
    j->total_drumroll++;
    j->branch_r_count++;
    j->score += 100;
    emit(j, TP_JE_SCORE, NULL, 0, 0, 1, 100);
    emit(j, TP_JE_DRUMROLL_HIT, lane_empty(&j->other) ? NULL : lane_front(&j->other), 0, drum_type, 0,
         j->curr_drumroll_count);
}

static void check_balloon(tp_judge *j, tp_drum drum_type, const tp_note *balloon) {
    if (drum_type != TP_DRUM_DON || balloon->count == TP_NO_COUNT) {
        return;
    }
    j->curr_balloon_count++;
    j->total_drumroll++;
    j->score += 100;
    emit(j, TP_JE_SCORE, NULL, 0, 0, 0, 100);
    const bool popped = j->curr_balloon_count == balloon->count;
    emit(j, TP_JE_BALLOON_HIT, balloon, 0, drum_type, popped, j->curr_balloon_count);
    if (popped) {
        j->is_balloon = false;
        note_correct(j, balloon);
        j->curr_balloon_count = 0;
    }
}

static void check_kusudama(tp_judge *j, tp_drum drum_type, const tp_note *balloon) {
    if (drum_type != TP_DRUM_DON || balloon->count == TP_NO_COUNT) {
        return;
    }
    const long long key = llround(balloon->hit_ms);
    if (j->kusudama_key != key) { /* a new kusudama starts its count from zero */
        j->kusudama_key = key;
        j->kusudama_shared_hits = 0;
    }
    j->total_drumroll++;
    j->score += 100;
    emit(j, TP_JE_SCORE, NULL, 0, 0, 0, 100);
    j->kusudama_shared_hits++;
    const bool popped = j->kusudama_shared_hits == balloon->count;
    emit(j, TP_JE_BALLOON_HIT, balloon, 0, drum_type, popped, j->kusudama_shared_hits);
    if (popped) {
        j->is_balloon = false;
        note_correct(j, balloon);
        j->kusudama_shared_hits = 0;
    }
}

/* first note of `lane` after `curr` sits before `next` (by index): something else comes in between */
static bool blocked_by(const tp_lane *lane, const tp_note *curr, const tp_note *next) {
    for (int i = lane->head; i < lane->tail; i++) {
        if (lane->v[i]->index > curr->index) {
            return lane->v[i]->index < next->index;
        }
    }
    return false;
}

void tp_judge_hit(tp_judge *j, double ms_from_start, tp_drum drum_type) {
    if (lane_empty(&j->don) && lane_empty(&j->kat) && lane_empty(&j->other)) {
        return;
    }
    float good_window_ms, ok_window_ms, bad_window_ms;
    if (j->difficulty <= 1) {
        good_window_ms = TP_TIMING_GOOD_EASY;
        ok_window_ms = TP_TIMING_OK_EASY;
        bad_window_ms = TP_TIMING_BAD_EASY;
    } else {
        good_window_ms = TP_TIMING_GOOD;
        ok_window_ms = TP_TIMING_OK;
        bad_window_ms = TP_TIMING_BAD;
    }

    if (j->is_drumroll && !lane_empty(&j->other)) {
        check_drumroll(j, drum_type);
        return;
    }
    if (j->is_balloon && !lane_empty(&j->other)) {
        const tp_note *balloon = lane_front(&j->other);
        if (balloon->type == TP_BALLOON_HEAD) {
            check_balloon(j, drum_type, balloon);
        }
        if (balloon->type == TP_KUSUDAMA) {
            check_kusudama(j, drum_type, balloon);
        }
        return;
    }

    tp_lane *lane = (drum_type == TP_DRUM_DON) ? &j->don : &j->kat;
    if (lane_empty(lane)) {
        return;
    }
    const tp_note *curr_note = lane_front(lane);
    if (!j->auto_play && lane_size(lane) > 1 && ms_from_start > curr_note->hit_ms + ok_window_ms) {
        const tp_note *next = lane->v[lane->head + 1];
        const tp_lane *other_lane = (drum_type == TP_DRUM_DON) ? &j->kat : &j->don;
        if (!blocked_by(other_lane, curr_note, next) && !blocked_by(&j->other, curr_note, next) &&
            ms_from_start > next->hit_ms - ok_window_ms) {
            /* hitting toward the next note gives up on this one */
            j->combo = 0;
            j->bad_count++;
            apply_points_clamped(&j->gauge, j->gauge.bad_points);
            j->branch_note_count++;
            lane_pop(lane);
            emit(j, TP_JE_JUDGMENT, curr_note, TP_BAD, TP_DRUM_NONE, 0, 0);
            curr_note = lane_front(lane);
        }
    }

    if (ms_from_start > (curr_note->hit_ms + bad_window_ms)) {
        return;
    }
    if ((curr_note->hit_ms - good_window_ms <= ms_from_start) && (ms_from_start <= curr_note->hit_ms + good_window_ms)) {
        if (!j->auto_play) {
        }
        j->good_count++;
        j->score += j->base_score;
        emit(j, TP_JE_SCORE, NULL, 0, 0, 1, j->base_score);
        emit(j, TP_JE_JUDGMENT, curr_note, TP_GOOD, drum_type, 0, 0);
        note_correct(j, curr_note);
        apply_points_clamped(&j->gauge, j->gauge.good_points);
        j->branch_p_count++;
        j->branch_note_count++;
    } else if ((curr_note->hit_ms - ok_window_ms) <= ms_from_start && ms_from_start <= (curr_note->hit_ms + ok_window_ms)) {
        if (!j->auto_play) {
        }
        j->ok_count++;
        const int ok_score = 10 * (j->base_score / 2 / 10);
        j->score += ok_score;
        emit(j, TP_JE_SCORE, NULL, 0, 0, 1, ok_score);
        emit(j, TP_JE_JUDGMENT, curr_note, TP_OK, drum_type, 0, 0);
        note_correct(j, curr_note);
        apply_points_clamped(&j->gauge, j->gauge.ok_points);
        j->branch_p_count += 0.5f;
        j->branch_note_count++;
    } else if ((curr_note->hit_ms - bad_window_ms) <= ms_from_start && ms_from_start <= (curr_note->hit_ms + bad_window_ms)) {
        j->bad_count++;
        j->combo = 0;
        j->branch_note_count++;
        if (!j->auto_play) {
        }
        lane_pop(lane);
        apply_points_clamped(&j->gauge, j->gauge.bad_points);
        emit(j, TP_JE_JUDGMENT, curr_note, TP_BAD, drum_type, 0, 0);
    }
}

/* Port of Player::autoplay_manager. Notes are judged at their own time, so
 * auto-play is all GOOD whatever the frame rate. */
void tp_judge_autoplay(tp_judge *j, double ms_from_start, double bpm) {
    if (j->is_drumroll || j->is_balloon) {
        double subdivision = bpm == 0 ? 0 : (int)(ms_from_start / ((240000.0 / bpm) / 24.0));
        if (subdivision > j->last_subdivision) {
            j->last_subdivision = subdivision;
            emit(j, TP_JE_AUTO_HIT, NULL, 0, TP_DRUM_DON, 0, 0);
            tp_judge_hit(j, ms_from_start, TP_DRUM_DON);
        }
        return;
    }
    const double bad_window = j->difficulty <= 1 ? TP_TIMING_BAD_EASY : TP_TIMING_BAD;
    tp_lane *lanes[2] = {&j->don, &j->kat};
    for (int l = 0; l < 2; l++) {
        tp_lane *lane = lanes[l];
        while (!lane_empty(lane) && ms_from_start >= lane_front(lane)->hit_ms) {
            const tp_note *note = lane_front(lane);
            if (ms_from_start > note->hit_ms + bad_window) {
                break;
            }
            int remaining = lane_size(lane);
            tp_drum drum = l == 0 ? TP_DRUM_DON : TP_DRUM_KAT;
            emit(j, TP_JE_AUTO_HIT, note, 0, drum, note->type == TP_DON_L || note->type == TP_KAT_L, 0);
            tp_judge_hit(j, note->hit_ms, drum);
            if (lane_size(lane) == remaining) {
                break;
            }
        }
    }
}
