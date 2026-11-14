/* Port of YataiDON's TJAParser (GPL-3.0, https://github.com/Yonokid/YataiDON,
 * src/libs/parsers/tja.cpp at 309d7b55). Function names and the order of
 * operations follow the original so the two can be compared side by side;
 * floating-point expressions keep its float/double mix on purpose. */
#include "tja.h"

#include <math.h>
#include <string.h>

#define MAX_PARTS 2048

/* ---------- small string helpers ---------- */

static bool is_name_char(char c) { return tp_isalpha(c) || c == '_'; }

/* A NAME:value header line, such as "EXAM4:r,58,65,m" or "BALLOON:5" */
static bool is_header_line(tp_str line) {
    if (!line.n || !is_name_char(line.p[0])) { /* every line of notes ends here */
        return false;
    }
    const char *colon = memchr(line.p, ':', line.n);
    if (!colon) {
        return false;
    }
    for (const char *p = line.p; p < colon; p++) {
        if (!is_name_char(*p) && !tp_isdigit(*p)) {
            return false;
        }
    }
    return true;
}

static tp_str after_colon(tp_str s) {
    const char *colon = memchr(s.p, ':', s.n);
    if (!colon) {
        return tp_str_make(s.p + s.n, 0);
    }
    return tp_str_make(colon + 1, (size_t)(s.p + s.n - (colon + 1)));
}

static tp_str key_of(tp_str s) {
    const char *colon = memchr(s.p, ':', s.n);
    return tp_str_make(s.p, colon ? (size_t)(colon - s.p) : s.n);
}

static tp_str skip(tp_str s, size_t n) { return tp_str_make(s.p + n, s.n - n); }

/* lower-cased copy for comparisons; false when it does not fit */
static bool lower(tp_str s, char *buf, size_t cap) {
    if (s.n >= cap) {
        return false;
    }
    for (size_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    buf[s.n] = 0;
    return true;
}

static int course_alias(tp_str value) {
    static const struct {
        const char *name;
        int difficulty;
    } aliases[] = {
        {"0", 0}, {"easy", 0}, {"1", 1},   {"normal", 1}, {"2", 2},     {"hard", 2}, {"3", 3},   {"oni", 3},
        {"4", 4}, {"edit", 4}, {"ura", 4}, {"5", 5},      {"tower", 5}, {"6", 6},    {"dan", 6},
    };
    char buf[16];
    if (!lower(tp_trim(value), buf, sizeof buf)) {
        return -1;
    }
    for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++) {
        if (!strcmp(buf, aliases[i].name)) {
            return aliases[i].difficulty;
        }
    }
    return -1;
}

/* "5,10.20" -> ints. '.' also separates. An invalid token rejects the whole list. */
static bool parse_balloon_data(tp_tja *t, tp_str data, tja_ints *out) {
    int first = t->int_count;
    size_t i = 0;
    while (i <= data.n) {
        size_t j = i;
        while (j < data.n && data.p[j] != ',' && data.p[j] != '.') {
            j++;
        }
        if (j > i) {
            int v;
            bool range = false;
            if (!tp_stoi(tp_str_make(data.p + i, j - i), &v, &range)) {
                t->int_count = first;
                return false;
            }
            if (range) {
                v = INT32_MAX;
            }
            if (t->int_count < TJA_MAX_INTS) {
                t->ints[t->int_count++] = v;
            }
        }
        i = j + 1;
    }
    out->first = first;
    out->count = t->int_count - first;
    return true;
}

/* ---------- loading and metadata ---------- */

static void get_metadata(tp_tja *t) {
    int current_diff = t->compat ? TJA_COURSES : -1;
    bool first_course = true;
    tja_course style_base[TJA_COURSES + 1];
    bool has_style_base[TJA_COURSES + 1] = {false};
    tp_str title_other = {0}, subtitle_other = {0};
    bool title_set = false, subtitle_set = false;
    t->start_count = 0;

    for (int i = 0; i < t->line_count; i++) {
        tp_str item = t->lines[i];
        if (tp_starts_with(item, "#START") && current_diff != -1) {
            if (current_diff == TJA_COURSES) { /* compat: a chart before any COURSE: is Oni */
                current_diff = 3;
                t->course[3] = t->course[TJA_COURSES];
                t->course[3].present = true;
                t->implicit_oni = true;
                first_course = false;
            }
            if (t->start_count < TJA_MAX_STARTS) {
                t->starts[t->start_count].line = i + 1;
                t->starts[t->start_count].balloon = t->course[current_diff].balloon;
                t->start_count++;
            }
        }
        if (tp_starts_with(item, "#BRANCH") && current_diff != -1) {
            t->course[current_diff].is_branching = true;
        } else if (item.p[0] == '#' || tp_isdigit(item.p[0])) {
            continue;
        } else if (tp_starts_with(item, "SUBTITLE")) {
            bool en = !(item.n > 8 && item.p[8] != ':');
            tp_str value = after_colon(item);
            if (tp_starts_with(value, "++") || tp_starts_with(value, "/ ")) {
                t->subtitle_full = true;
                value = skip(value, 2);
            } else if (tp_starts_with(value, "--")) {
                t->subtitle_full = false;
                value = skip(value, 2);
            }
            if (en) {
                t->subtitle = value;
                subtitle_set = true;
            } else if (!subtitle_other.n) {
                subtitle_other = value;
            }
        } else if (tp_starts_with(item, "TITLE")) {
            if (!(item.n > 5 && item.p[5] != ':')) {
                t->title = after_colon(item);
                title_set = true;
            } else if (!title_other.n) {
                title_other = after_colon(item);
            }
        } else if (tp_starts_with(item, "BPM") || tp_starts_with(item, "OFFSET") ||
                   tp_starts_with(item, "DEMOSTART") || tp_starts_with(item, "MOVIEOFFSET")) {
            tp_str data = after_colon(item);
            double value = 0.0;
            if (data.n && !tp_stof(data, &value)) {
                value = 0.0;
                t->warnings++;
            }
            if (tp_starts_with(item, "BPM")) {
                t->bpm = value;
            } else if (tp_starts_with(item, "OFFSET")) {
                t->offset = value;
            } else if (tp_starts_with(item, "DEMOSTART")) {
                t->demostart = value;
            }
        } else if (tp_starts_with(item, "WAVE")) {
            t->wave = tp_trim(after_colon(item));
        } else if (tp_starts_with(item, "BGMOVIE") || tp_starts_with(item, "PREIMAGE") ||
                   tp_starts_with(item, "SCENEPRESET")) {
            continue;
        } else if (tp_starts_with(item, "COURSE")) {
            if (t->compat && !tp_equals(key_of(item), "COURSE")) {
                continue; /* COURSEDESIGNER4: and the like */
            }
            current_diff = course_alias(after_colon(item));
            if (current_diff == -1) {
                t->warnings++;
            } else {
                tja_course fresh;
                memset(&fresh, 0, sizeof fresh);
                /* compat: headers written before the first COURSE: belong to it */
                t->course[current_diff] = (t->compat && first_course) ? t->course[TJA_COURSES] : fresh;
                t->course[current_diff].present = true;
                has_style_base[current_diff] = false;
                first_course = false;
            }
        } else if (tp_equals(key_of(item), "SONGVOL") || tp_equals(key_of(item), "SEVOL")) {
            double value;
            if (tp_stof(after_colon(item), &value)) {
                *(item.p[1] == 'O' ? &t->songvol : &t->sevol) = value;
            }
        } else if (tp_equals(key_of(item), "GENRE")) {
            t->genre = tp_trim(after_colon(item));
        } else if (current_diff != -1) {
            tja_course *cd = &t->course[current_diff];
            if (tp_starts_with(item, "LEVEL")) {
                tp_str data = after_colon(item);
                double value = 0.0;
                if (data.n && !tp_stof(data, &value)) {
                    value = 0.0;
                    t->warnings++;
                }
                cd->level = (double)(int)value;
            } else if (tp_starts_with(item, "STYLE")) {
                if (!has_style_base[current_diff]) {
                    style_base[current_diff] = *cd;
                    has_style_base[current_diff] = true;
                }
                cd->balloon = style_base[current_diff].balloon;
                cd->scoreinit = style_base[current_diff].scoreinit;
                cd->scorediff = style_base[current_diff].scorediff;
            } else if (tp_starts_with(item, "BALLOON")) {
                tja_ints *balloons = tp_starts_with(item, "BALLOONNOR")   ? &cd->branch_balloon[0]
                                     : tp_starts_with(item, "BALLOONEXP") ? &cd->branch_balloon[1]
                                     : tp_starts_with(item, "BALLOONMAS") ? &cd->branch_balloon[2]
                                                                          : &cd->balloon;
                if (balloons == &cd->balloon && !tp_contains(item, ':')) {
                    balloons->count = 0;
                    continue;
                }
                tp_str data = after_colon(item);
                if (data.n && !parse_balloon_data(t, data, balloons)) {
                    t->warnings++;
                }
            } else if (tp_starts_with(item, "SCOREINIT")) {
                tp_str data = after_colon(item);
                if (data.n && !parse_balloon_data(t, data, &cd->scoreinit)) {
                    cd->scoreinit.count = 0;
                }
            } else if (tp_starts_with(item, "SCOREDIFF")) {
                tp_str data = after_colon(item);
                double value;
                if (data.n && tp_stof(data, &value)) {
                    cd->scorediff = (double)(int)value;
                }
            }
        }
    }
    if (!title_set) {
        t->title = title_other;
    }
    if (!subtitle_set) {
        t->subtitle = subtitle_other;
    }
    t->course[TJA_COURSES].present = false;
}

bool tja_load(tp_tja *t, const char *text, size_t len, tp_str *line_buf, int line_cap, bool compat) {
    memset(t, 0, sizeof *t);
    t->lines = line_buf;
    t->compat = compat;
    t->bpm = 120.0;
    t->songvol = 100.0;
    t->sevol = 100.0;

    const char *p = text, *end = text + len;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3;
    }
    bool ok = true;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        tp_str line = tp_str_make(p, (size_t)(line_end - p));
        p = nl ? nl + 1 : end;

        /* drop a // comment; a line that is only a comment disappears */
        for (size_t i = 0; i + 1 < line.n; i++) {
            if (line.p[i] == '/' && line.p[i + 1] == '/') {
                line.n = i;
                if (!tp_trim(line).n) {
                    line.n = 0;
                }
                break;
            }
        }
        while (line.n && strchr(" \t\r\n", line.p[0])) {
            line.p++;
            line.n--;
        }
        while (line.n && strchr(" \t\r\n", line.p[line.n - 1])) {
            line.n--;
        }
        if (!line.n) {
            continue;
        }
        if (t->line_count >= line_cap) {
            ok = false;
            break;
        }
        line_buf[t->line_count++] = line;
    }
    get_metadata(t);
    return ok;
}

bool tja_find_chart(const tp_tja *t, int diff, int player_num, int *start, int *end, int *scroll_out) {
    static const char *names[TJA_COURSES] = {"easy", "normal", "hard", "oni", "edit", "tower", "dan"};
    if (diff < 0 || diff >= TJA_COURSES) {
        return false;
    }
    const char *diff_name = names[diff];
    int note_start = -1, note_end = -1, p1_start = -1, p2_start = -1;
    bool target_found = t->compat && t->implicit_oni && diff == 3;
    int scroll_type = TP_NMSCROLL;

    for (int i = 0; i < t->line_count; i++) {
        tp_str line = t->lines[i];
        if (tp_starts_with(line, "COURSE:")) {
            /* P1/P2 charts may follow the Single chart, so keep looking until the course ends */
            if (target_found && note_start != -1) {
                break;
            }
            char value[16];
            target_found = false;
            if (lower(tp_trim(skip(line, 7)), value, sizeof value)) {
                bool is_digit = value[0] != 0;
                for (const char *c = value; *c; c++) {
                    is_digit = is_digit && tp_isdigit(*c);
                }
                int number = -1;
                if (is_digit) {
                    tp_stoi(tp_str_make(value, strlen(value)), &number, NULL);
                }
                target_found = (is_digit && number == diff) || !strcmp(value, diff_name) ||
                               (!strcmp(value, "ura") && diff == 4);
            }
        } else if (target_found) {
            if (tp_equals(line, "#START") && note_start == -1) {
                note_start = i + 1;
            } else if (tp_equals(line, "#START P1") && p1_start == -1) {
                p1_start = i + 1;
            } else if (tp_equals(line, "#START P2") && p2_start == -1) {
                p2_start = i + 1;
            }
            /* Like Taiko-san Jiro, the scroll mode only counts before #START */
            bool started = note_start != -1 || p1_start != -1 || p2_start != -1;
            static const char *modes[3] = {"#NMSCROLL", "#BMSCROLL", "#HBSCROLL"};
            for (int m = 0; m < 3 && !started; m++) {
                bool contains = false;
                for (size_t k = 0; k + 9 <= line.n && !contains; k++) {
                    contains = memcmp(line.p + k, modes[m], 9) == 0;
                }
                if (contains) {
                    if (!started) {
                        scroll_type = m;
                    }
                    break;
                }
            }
        }
    }

    if (note_start != -1) {
        if (player_num == 1 && p1_start != -1) {
            note_start = p1_start;
        } else if (player_num == 2 && p2_start != -1) {
            note_start = p2_start;
        }
    } else {
        if (player_num == 1 && p1_start != -1) {
            note_start = p1_start;
        } else if (player_num == 2 && p2_start != -1) {
            note_start = p2_start;
        } else if (player_num == 0 && p1_start != -1) {
            note_start = p1_start;
        } else if (player_num == 0 && p2_start != -1) {
            note_start = p2_start;
        }
    }

    /* chart ends at the first #END after its own #START */
    if (note_start != -1) {
        for (int i = note_start; i < t->line_count; i++) {
            if (tp_equals(t->lines[i], "#END")) {
                note_end = i;
                break;
            }
            if (t->compat && tp_starts_with(t->lines[i], "COURSE:")) {
                note_end = i; /* compat: a missing #END ends the chart at the next course */
                break;
            }
        }
        if (note_end == -1 && t->compat) {
            note_end = t->line_count;
        }
    }
    if (note_start == -1 || note_end == -1) {
        return false;
    }
    *start = note_start;
    *end = note_end;
    *scroll_out = scroll_type;
    return true;
}

/* ---------- tempo map ---------- */

static double tempo_lead(const tp_chart *c) { return c->lead_bpm != 0.0 ? c->lead_bpm : c->tempo[0].bpm; }

double tp_tempo_beat_at(const tp_chart *c, double ms) {
    if (c->tempo_count == 0) {
        return 0.0;
    }
    /* upper_bound: first point after ms */
    int lo = 0, hi = c->tempo_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (ms < c->tempo[mid].ms) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    if (lo == 0) {
        return c->tempo[0].beat + (ms - c->tempo[0].ms) * tempo_lead(c) / 60000.0;
    }
    const tp_tempo_point *p = &c->tempo[lo - 1];
    return p->beat + (ms - p->ms) * p->bpm / 60000.0;
}

/* One linear stretch of the beat count: through (ms, beat) at bpm, over [from, to] */
static void stretch(double lo, double hi, double ms, double beat, double bpm, double from, double to,
                    double *first, double *last) {
    double enter, leave;
    if (bpm == 0.0) {
        if (beat < lo || beat > hi) {
            return;
        }
        enter = from;
        leave = to;
    } else {
        double a = ms + (lo - beat) * 60000.0 / bpm;
        double b = ms + (hi - beat) * 60000.0 / bpm;
        enter = fmax(fmin(a, b), from);
        leave = fmin(fmax(a, b), to);
        if (enter > leave) {
            return;
        }
    }
    *first = fmin(*first, enter);
    *last = fmax(*last, leave);
}

void tp_tempo_ms_within(const tp_chart *c, double lo, double hi, double *first, double *last) {
    *first = INFINITY;
    *last = -INFINITY;
    if (c->tempo_count == 0) {
        return;
    }
    stretch(lo, hi, c->tempo[0].ms, c->tempo[0].beat, tempo_lead(c), -INFINITY, c->tempo[0].ms, first, last);
    for (int i = 0; i < c->tempo_count; i++) {
        const tp_tempo_point *p = &c->tempo[i];
        if (i + 1 < c->tempo_count) {
            /* The beat runs from this point's to the next one's. Clear of [lo, hi] by more
             * than rounding could matter: nothing to find here, and two divisions saved.
             * (A beat-scrolled chart asks this for every note against every tempo point.) */
            double b0 = p->beat, b1 = c->tempo[i + 1].beat;
            if ((b0 < lo - 1e-6 && b1 < lo - 1e-6) || (b0 > hi + 1e-6 && b1 > hi + 1e-6)) {
                continue;
            }
        }
        stretch(lo, hi, p->ms, p->beat, p->bpm, p->ms, i + 1 < c->tempo_count ? c->tempo[i + 1].ms : INFINITY,
                first, last);
    }
}

/* ---------- notes_to_position ---------- */

typedef struct {
    tp_tja *t;
    tp_chart *c;
    double current_ms;

    double time_signature;
    double bpm;
    double scroll_x_modifier, scroll_y_modifier;
    int scroll_type;
    bool barline_display;
    int list; /* current note list / timeline: 0 master, n = section n-1 */
    double index;
    tja_ints balloons;
    size_t balloon_cursor;
    double balloon_index;
    size_t branch_balloon_cursor;
    tja_ints route_balloons[3];
    size_t route_cursor[3];
    int branch; /* 0 #N, 1 #E, 2 #M; -1 outside the branches */
    bool barline_added;
    double sudden_appear, sudden_moving;
    double judge_pos_x, judge_pos_y;
    bool is_branching, is_section_start;
    bool doron;
    double start_branch_ms, start_branch_bpm, start_branch_time_sig;
    double start_branch_x_scroll, start_branch_y_scroll;
    bool start_branch_barline;
    double branch_balloon_index;
    bool has_section_bar;
    double section_bar_hit_ms;
    int last_section[3]; /* latest section of each path, -1 when none */
    int list_events[1];  /* events in the master timeline (sections count their own) */
} parser_state;

static double get_ms_per_measure(double bpm_val, double time_sig) {
    if (bpm_val == 0) {
        return 0;
    }
    return 60000 * (time_sig * 4) / bpm_val;
}

static tp_note *push_note(parser_state *s) {
    tp_chart *c = s->c;
    if (s->list == 0) {
        if (c->master_count >= c->master_cap) {
            c->overflow = true;
            return NULL;
        }
        return &c->master[c->master_count++];
    }
    if (c->branch_count >= c->branch_cap) {
        c->overflow = true;
        return NULL;
    }
    c->sections[s->list - 1].note_count++;
    return &c->branch[c->branch_count++];
}

static tp_event *push_event_to(parser_state *s, int list, double start_ms) {
    tp_chart *c = s->c;
    if (c->event_count >= c->event_cap) {
        c->overflow = true;
        return NULL;
    }
    tp_event *e = &c->events[c->event_count++];
    memset(e, 0, sizeof *e);
    e->start_ms = start_ms;
    e->list = (int16_t)list;
    if (list == 0) {
        e->seq = (int16_t)s->list_events[0]++;
    } else {
        e->seq = (int16_t)c->sections[list - 1].event_count++;
    }
    return e;
}

static tp_event *push_event(parser_state *s, int kind) {
    tp_event *e = push_event_to(s, s->list, s->current_ms);
    if (e) {
        e->kind = (uint8_t)kind;
    }
    return e;
}

/* Branches rewind current_ms; their repeated tempo changes land before the last point and are
 * skipped, so the beat map follows the first branch. */
static void add_tempo_point(parser_state *s, double bpm) {
    tp_chart *c = s->c;
    if (c->tempo_count == 0) { /* a caller that asked for no tempo map */
        return;
    }
    tp_tempo_point *back = &c->tempo[c->tempo_count - 1];
    if (s->current_ms < back->ms) {
        return;
    }
    if (s->current_ms == back->ms) {
        back->bpm = bpm;
        return;
    }
    if (c->tempo_count >= c->tempo_cap) {
        c->overflow = true;
        return;
    }
    double beat = tp_tempo_beat_at(c, s->current_ms);
    tp_tempo_point *p = &c->tempo[c->tempo_count++];
    p->ms = s->current_ms;
    p->beat = beat;
    p->bpm = bpm;
}

static void handle_MEASURE(parser_state *s, tp_str value) {
    const char *slash = memchr(value.p, '/', value.n);
    if (!slash) {
        return;
    }
    double num, den;
    if (!tp_stof(tp_str_make(value.p, (size_t)(slash - value.p)), &num) ||
        !tp_stof(tp_str_make(slash + 1, (size_t)(value.p + value.n - slash - 1)), &den)) {
        s->c->warnings++;
        return;
    }
    /* #BMSCROLL / #HBSCROLL charts pair a negative measure with a negative BPM, or use a
     * zero-length measure and space its notes with #DELAY */
    bool beat_scroll = s->scroll_type != TP_NMSCROLL;
    if (den != 0.0 && (num > 0.0 || beat_scroll)) {
        s->time_signature = num / den;
    } else {
        s->c->warnings++;
    }
}

/* One term of the complex-number pattern: [+-]?[0-9]*\.?[0-9]+ ; returns its length or 0 */
static size_t match_term(const char *p, size_t n, bool sign_required) {
    size_t i = 0;
    if (i < n && (p[i] == '+' || p[i] == '-')) {
        i++;
    } else if (sign_required) {
        return 0;
    }
    size_t digits_start = i;
    while (i < n && tp_isdigit(p[i])) {
        i++;
    }
    if (i < n && p[i] == '.' && i + 1 < n && tp_isdigit(p[i + 1])) {
        i++;
        while (i < n && tp_isdigit(p[i])) {
            i++;
        }
    }
    return i > digits_start ? i : 0;
}

static void handle_SCROLL(parser_state *s, tp_str value) {
    if (s->scroll_type == TP_BMSCROLL) {
        return;
    }
    if (!value.n) {
        s->c->warnings++;
        return;
    }
    bool has_i = tp_contains(value, 'i');
    /* Polar form (TaikoManyGimmicks): #SCROLL <speed>, <rotation-lower>, <rotation-upper> */
    if (tp_contains(value, ',') && !has_i) {
        double parts[3];
        int n = 0;
        bool ok = true;
        for (size_t i = 0; i < value.n;) { /* std::getline(ss, part, ',') */
            size_t j = i;
            while (j < value.n && value.p[j] != ',') {
                j++;
            }
            double v;
            if (!tp_stod(tp_trim(tp_str_make(value.p + i, j - i)), &v)) {
                ok = false;
                break;
            }
            if (n < 3) {
                parts[n] = v;
            }
            n++;
            i = j + 1;
        }
        if (!ok || n != 3 || parts[1] == 0.0) {
            s->c->warnings++;
            return;
        }
        double angle = 2.0 * 3.14159265358979323846 * parts[2] / parts[1];
        s->scroll_x_modifier = parts[0] * cos(angle);
        s->scroll_y_modifier = parts[0] * sin(angle);
        return;
    }
    if (has_i) {
        /* ".i" -> "j", "i" -> "j", "," removed, then ([+-]?\d*\.?\d+)?([+-]\d*\.?\d+)?j? */
        char buf[64];
        size_t n = 0;
        for (size_t i = 0; i < value.n; i++) {
            char ch = value.p[i];
            if (ch == '.' && i + 1 < value.n && value.p[i + 1] == 'i') {
                continue;
            }
            if (ch == ',') {
                continue;
            }
            if (n + 1 >= sizeof buf) {
                s->c->warnings++;
                return;
            }
            buf[n++] = ch == 'i' ? 'j' : ch;
        }
        size_t a = match_term(buf, n, false);
        size_t b = match_term(buf + a, n - a, true);
        size_t rest = a + b;
        bool ends_j = rest < n && buf[rest] == 'j';
        if (rest + (ends_j ? 1 : 0) != n) {
            s->c->warnings++;
            return;
        }
        double real = 0.0, imag = 0.0;
        if (a && !tp_stof(tp_str_make(buf, a), &real)) {
            s->c->warnings++;
            return;
        }
        if (b) {
            if (!tp_stof(tp_str_make(buf + a, b), &imag)) {
                s->c->warnings++;
                return;
            }
        } else if (a && buf[n - 1] == 'j') {
            imag = real;
            real = 0.0;
        }
        s->scroll_x_modifier = real;
        s->scroll_y_modifier = imag;
        return;
    }
    double v;
    if (tp_stof(value, &v)) {
        s->scroll_x_modifier = v;
        s->scroll_y_modifier = 0.0;
    } else {
        s->c->warnings++;
    }
}

static void handle_BPMCHANGE(parser_state *s, tp_str value) {
    double parsed;
    if (!value.n || !tp_stof(value, &parsed)) {
        s->c->warnings++;
        return;
    }
    tp_event *e = push_event(s, TP_EV_BPM);
    if (e) {
        e->bpm = parsed;
    }
    s->bpm = parsed;
    add_tempo_point(s, parsed);
}

static void handle_GOGO(parser_state *s, bool on) {
    tp_event *e = push_event(s, TP_EV_GOGO);
    if (e) {
        e->gogo = on;
    }
}

static void handle_DELAY(parser_state *s, tp_str value) {
    double v;
    if (!value.n || !tp_stof(value, &v)) {
        s->c->warnings++;
        return;
    }
    double delay_ms = (float)v * 1000.0f; /* a float product in the original */
    if (delay_ms > 0) {
        /* The beat count stands still through a delay (seen with #BMSCROLL / #HBSCROLL) */
        add_tempo_point(s, 0.0);
        s->current_ms += delay_ms;
        add_tempo_point(s, s->bpm);
    } else {
        s->current_ms += delay_ms;
    }
}

static void handle_BRANCHSTART(parser_state *s, tp_str value) {
    s->start_branch_ms = s->current_ms;
    s->start_branch_bpm = s->bpm;
    s->start_branch_time_sig = s->time_signature;
    s->start_branch_x_scroll = s->scroll_x_modifier;
    s->start_branch_y_scroll = s->scroll_y_modifier;
    s->start_branch_barline = s->barline_display;
    s->branch_balloon_index = s->balloon_index;
    s->branch_balloon_cursor = s->balloon_cursor;

    double two_measures = get_ms_per_measure(s->bpm, s->time_signature) * 2.0;
    double start_time;
    if (s->has_section_bar) {
        start_time = s->section_bar_hit_ms - two_measures;
        s->has_section_bar = false;
    } else {
        start_time = s->current_ms - two_measures;
    }

    /* the judge splits the parameters on ','; do it here, once */
    tp_event proto;
    memset(&proto, 0, sizeof proto);
    proto.kind = TP_EV_BRANCH;
    tp_str part[3];
    int parts = 0;
    for (size_t i = 0; i < value.n;) { /* std::getline(ss, part, ',') */
        size_t j = i;
        while (j < value.n && value.p[j] != ',') {
            j++;
        }
        if (parts < 3) {
            part[parts] = tp_str_make(value.p + i, j - i);
        }
        parts++;
        i = j + 1;
    }
    if (parts >= 3) {
        proto.parts_ok = 1;
        proto.cond = tp_equals(part[0], "p") ? 'p' : tp_equals(part[0], "r") ? 'r' : tp_equals(part[0], "s") ? 's' : 0;
        double e_req, m_req;
        if (tp_stof(part[1], &e_req) && tp_stof(part[2], &m_req)) {
            proto.e_req = (float)e_req;
            proto.m_req = (float)m_req;
        }
    }

    int targets[4];
    int n = 0;
    targets[n++] = s->list;
    for (int path = 2; path >= 0; path--) { /* branch_m, branch_e, branch_n */
        if (s->last_section[path] >= 0) {
            targets[n++] = s->last_section[path] + 1;
        }
    }
    for (int k = 0; k < n; k++) {
        tp_event *e = push_event_to(s, targets[k], start_time);
        if (e) {
            int16_t list = e->list, seq = e->seq;
            *e = proto;
            e->start_ms = start_time;
            e->list = list;
            e->seq = seq;
        }
    }
}

static void handle_path(parser_state *s, int path) {
    tp_chart *c = s->c;
    if (c->section_count >= c->section_cap) {
        c->overflow = true;
        return;
    }
    tp_section *sec = &c->sections[c->section_count];
    memset(sec, 0, sizeof *sec);
    sec->path = (uint8_t)path;
    sec->first_note = c->branch_count;
    s->last_section[path] = c->section_count;
    s->list = ++c->section_count;

    s->current_ms = s->start_branch_ms;
    s->bpm = s->start_branch_bpm;
    s->time_signature = s->start_branch_time_sig;
    s->scroll_x_modifier = s->start_branch_x_scroll;
    s->scroll_y_modifier = s->start_branch_y_scroll;
    s->barline_display = s->start_branch_barline;
    s->balloon_index = s->branch_balloon_index;
    s->balloon_cursor = s->branch_balloon_cursor;
    s->is_branching = true;
    s->branch = path;
}

/* whitespace-separated tokens, like `iss >> token` */
static int split_ws(tp_str v, tp_str *out, int cap) {
    int n = 0;
    size_t i = 0;
    while (i < v.n) {
        while (i < v.n && tp_isspace(v.p[i])) {
            i++;
        }
        size_t j = i;
        while (j < v.n && !tp_isspace(v.p[j])) {
            j++;
        }
        if (j > i) {
            if (n < cap) {
                out[n] = tp_str_make(v.p + i, j - i);
            }
            n++;
        }
        i = j;
    }
    return n;
}

static void handle_SUDDEN(parser_state *s, tp_str value) {
    tp_str parts[2];
    if (split_ws(value, parts, 2) < 2) {
        return;
    }
    double appear, moving;
    if (!tp_stof(parts[0], &appear) || !tp_stof(parts[1], &moving)) {
        s->c->warnings++;
        return;
    }
    s->sudden_appear = appear * 1000;
    s->sudden_moving = moving * 1000;
    if (s->sudden_appear == 0) {
        s->sudden_appear = INFINITY;
    }
    if (s->sudden_moving == 0) {
        s->sudden_moving = INFINITY;
    }
}

/* A #JPOSSCROLL distance component: pixels ("-94") or a fraction of the note field ("3/5") */
static bool jpos_component(const char *p, size_t n, double field_width, double *out) {
    char buf[40];
    if (n + 2 > sizeof buf) {
        return false;
    }
    memcpy(buf, p, n);
    if (n == 0 || (n == 1 && (buf[0] == '+' || buf[0] == '-'))) {
        buf[n++] = '1';
    }
    const char *slash = memchr(buf, '/', n);
    if (!slash) {
        return tp_stod(tp_str_make(buf, n), out);
    }
    double num, den;
    if (!tp_stod(tp_str_make(slash + 1, (size_t)(buf + n - slash - 1)), &den) || den == 0.0 ||
        !tp_stod(tp_str_make(buf, (size_t)(slash - buf)), &num)) {
        return false;
    }
    *out = num / den * field_width;
    return true;
}

static void handle_JPOSSCROLL(parser_state *s, tp_str value) {
    tp_chart *c = s->c;
    tp_str parts[3];
    if (split_ws(value, parts, 3) < 3) {
        c->warnings++;
        return;
    }
    double duration;
    int direction;
    bool range = false;
    if (!tp_stof(parts[0], &duration) || !tp_stoi(parts[2], &direction, &range) || range) {
        c->warnings++;
        return;
    }
    double duration_ms = (float)duration * 1000.0f; /* a float product in the original */
    char dist[40];
    size_t n = 0;
    for (size_t i = 0; i < parts[1].n; i++) {
        if (parts[1].p[i] != ',') {
            if (n + 1 >= sizeof dist) {
                c->warnings++;
                return;
            }
            dist[n++] = parts[1].p[i];
        }
    }
    double delta_x = 0.0, delta_y = 0.0;
    double w = s->t->jpos_field_width;
    bool valid;
    if (n && dist[n - 1] == 'i') {
        size_t body = n - 1;
        size_t split = (size_t)-1;
        for (size_t k = body; k-- > 1;) {
            char prev = dist[k - 1];
            if ((dist[k] == '+' || dist[k] == '-') && prev != 'e' && prev != 'E' && prev != '/') {
                split = k;
                break;
            }
        }
        if (split == (size_t)-1) {
            valid = body ? jpos_component(dist, body, w, &delta_y) : jpos_component("1", 1, w, &delta_y);
        } else {
            valid = jpos_component(dist, split, w, &delta_x) && jpos_component(dist + split, body - split, w, &delta_y);
        }
    } else {
        valid = jpos_component(dist, n, w, &delta_x);
    }
    if (!valid) {
        c->warnings++;
        return;
    }
    if (direction == 0) {
        delta_x = -delta_x;
        delta_y = -delta_y;
    }
    /* cut short a move that is still running */
    for (int i = c->event_count - 1; i >= 0; i--) {
        tp_event *obj = &c->events[i];
        if (obj->list != s->list || obj->kind != TP_EV_JPOS) {
            continue;
        }
        if (obj->end_ms > s->current_ms) {
            double available = s->current_ms - obj->start_ms;
            double total = obj->end_ms - obj->start_ms;
            double ratio = (total > 0) ? fmin(1.0, available / total) : 1.0;
            s->judge_pos_x -= obj->delta_x * (1.0 - ratio);
            s->judge_pos_y -= obj->delta_y * (1.0 - ratio);
            obj->delta_x *= ratio;
            obj->delta_y *= ratio;
            obj->end_ms = s->current_ms;
            break;
        }
    }
    tp_event *e = push_event(s, TP_EV_JPOS);
    if (e) {
        e->end_ms = s->current_ms + duration_ms;
        e->judge_x = s->judge_pos_x;
        e->judge_y = s->judge_pos_y;
        e->delta_x = delta_x;
        e->delta_y = delta_y;
    }
    s->judge_pos_x += delta_x;
    s->judge_pos_y += delta_y;
}

static void command(parser_state *s, tp_str part) {
    /* longest name first; whole name only: #NEXTSONG is not #N */
    static const char *names[] = {"#DISABLEDORON", "#BRANCHSTART", "#ENABLEDORON", "#BARLINEOFF", "#JPOSSCROLL",
                                  "#BPMCHANGE",    "#BARLINEON",   "#BRANCHEND",   "#GOGOSTART",  "#LEVELHOLD",
                                  "#NMSCROLL",     "#BMSCROLL",    "#HBSCROLL",    "#GOGOEND",    "#MEASURE",
                                  "#SECTION",      "#SCROLL",      "#SUDDEN",      "#DELAY",      "#LYRIC",
                                  "#N",            "#E",           "#M"};
    enum { DISABLEDORON, BRANCHSTART, ENABLEDORON, BARLINEOFF, JPOSSCROLL, BPMCHANGE, BARLINEON, BRANCHEND,
           GOGOSTART, LEVELHOLD, NMSCROLL, BMSCROLL, HBSCROLL, GOGOEND, MEASURE, SECTION, SCROLL, SUDDEN,
           DELAY, LYRIC, CMD_N, CMD_E, CMD_M, CMD_COUNT };
    int cmd = -1;
    size_t len = 0;
    for (int i = 0; i < CMD_COUNT; i++) {
        len = strlen(names[i]);
        if (part.n >= len && memcmp(part.p, names[i], len) == 0 && !(part.n > len && is_name_char(part.p[len]))) {
            cmd = i;
            break;
        }
    }
    if (cmd < 0) {
        return;
    }
    tp_str value = tp_trim(skip(part, len));
    tp_event *e;
    switch (cmd) {
    case BPMCHANGE: handle_BPMCHANGE(s, value); break;
    case MEASURE: handle_MEASURE(s, value); break;
    case SCROLL: handle_SCROLL(s, value); break;
    case GOGOSTART: handle_GOGO(s, true); break;
    case GOGOEND: handle_GOGO(s, false); break;
    case DELAY: handle_DELAY(s, value); break;
    case BARLINEOFF: s->barline_display = false; break;
    case BARLINEON: s->barline_display = true; break;
    case BRANCHSTART: handle_BRANCHSTART(s, value); break;
    case BRANCHEND:
        s->list = 0;
        s->is_branching = false;
        s->branch = -1;
        break;
    case CMD_N: handle_path(s, 0); break;
    case CMD_E: handle_path(s, 1); break;
    case CMD_M: handle_path(s, 2); break;
    case SECTION:
        s->is_section_start = true;
        push_event(s, TP_EV_SECTION);
        break;
    case NMSCROLL: s->scroll_type = TP_NMSCROLL; break;
    case BMSCROLL: s->scroll_type = TP_BMSCROLL; break;
    case HBSCROLL: s->scroll_type = TP_HBSCROLL; break;
    case SUDDEN: handle_SUDDEN(s, value); break;
    case JPOSSCROLL: handle_JPOSSCROLL(s, value); break;
    case LYRIC:
        e = push_event(s, TP_EV_LYRIC);
        if (e) {
            e->text = value.p;
            e->text_len = (uint32_t)value.n;
        }
        break;
    case ENABLEDORON: s->doron = true; break;
    case DISABLEDORON: s->doron = false; break;
    case LEVELHOLD: /* not in YataiDON */
        if (s->t->compat) {
            push_event(s, TP_EV_LEVELHOLD);
        }
        break;
    default: break;
    }
}

static void add_bar(parser_state *s) {
    tp_note bar;
    memset(&bar, 0, sizeof bar);
    bar.hit_ms = s->current_ms;
    bar.type = TP_BARLINE;
    bar.count = TP_NO_COUNT;
    if (s->barline_display && !s->barline_added) {
        bar.flags |= TP_NOTE_DISPLAY;
    }
    if (!s->barline_added) {
        bar.flags |= TP_NOTE_BAR_START;
    }
    bar.bpm = s->bpm;
    bar.index = (int32_t)s->index;
    bar.scroll_x = s->scroll_x_modifier;
    bar.scroll_y = s->scroll_y_modifier;
    if (s->is_branching) {
        bar.flags |= TP_NOTE_BRANCH_START;
        s->is_branching = false;
    }
    if (s->is_section_start) {
        s->has_section_bar = true;
        s->section_bar_hit_ms = bar.hit_ms;
        s->is_section_start = false;
    }
    tp_note *dst = push_note(s);
    if (dst) {
        *dst = bar;
    }
}

static void add_note(parser_state *s, char item) {
    tp_note note;
    memset(&note, 0, sizeof note);
    note.hit_ms = s->current_ms;
    note.count = TP_NO_COUNT;
    if (!s->doron) {
        note.flags |= TP_NOTE_DISPLAY;
    }
    if (item == 'A' || item == 'B') {
        note.flags |= TP_NOTE_PARTNER;
    }
    note.type = item == 'A' ? TP_DON_L : item == 'B' ? TP_KAT_L : (uint8_t)(item - '0');
    note.index = (int32_t)s->index;
    note.bpm = s->bpm;
    note.scroll_x = s->scroll_x_modifier;
    note.scroll_y = s->scroll_y_modifier;
    if (s->sudden_appear > 0 || s->sudden_moving > 0) {
        note.flags |= TP_NOTE_SUDDEN;
        note.sudden_appear_ms = s->sudden_appear;
        note.sudden_moving_ms = s->sudden_moving;
    }
    if (note.type == TP_BALLOON_HEAD || note.type == TP_KUSUDAMA) {
        s->balloon_index++;
        /* own route list if set, else BALLOON; a balloon outside the branches advances every route */
        size_t route = (size_t)(s->branch > 0 ? s->branch : 0);
        const tja_ints *own = &s->route_balloons[route];
        size_t cursor = own->count == 0 ? s->balloon_cursor : s->route_cursor[route];
        const tja_ints *list = own->count == 0 ? &s->balloons : own;
        note.count = cursor < (size_t)list->count ? s->t->ints[list->first + (int)cursor] : 1;
        if (s->branch == -1) {
            for (int i = 0; i < 3; i++) {
                s->route_cursor[i]++;
            }
        } else {
            s->route_cursor[route]++;
        }
        s->balloon_cursor++;
    }
    tp_note *dst = push_note(s);
    if (dst) {
        *dst = note;
    }
}

/* last note of the current list that is not a bar line */
static const tp_note *last_real_note(const parser_state *s) {
    const tp_chart *c = s->c;
    const tp_note *base;
    int count;
    if (s->list == 0) {
        base = c->master;
        count = c->master_count;
    } else {
        base = c->branch + c->sections[s->list - 1].first_note;
        count = c->sections[s->list - 1].note_count;
    }
    for (int i = count - 1; i >= 0; i--) {
        if (base[i].type != TP_BARLINE) {
            return &base[i];
        }
    }
    return NULL;
}

static void process_bar(parser_state *s, const tp_str *bar, int n) {
    int bar_length = 0;
    for (int i = 0; i < n; i++) {
        if (bar[i].n == 0 || bar[i].p[0] != '#') {
            bar_length += (int)bar[i].n;
        }
    }
    s->barline_added = false;

    for (int i = 0; i < n; i++) {
        tp_str part = bar[i];
        if (part.n && part.p[0] == '#') {
            command(s, part);
            continue;
        }
        double ms_per_measure = get_ms_per_measure(s->bpm, s->time_signature);
        add_bar(s);
        s->barline_added = true;
        s->index++;

        double increment;
        if (part.n == 0) {
            s->current_ms += ms_per_measure;
            increment = 0.0;
        } else {
            increment = (bar_length > 0) ? ms_per_measure / (double)bar_length : 0.0;
        }
        for (size_t k = 0; k < part.n; k++) {
            char item = part.p[k];
            /* blank notes, and characters that are not notes */
            if (item == '0' || (!tp_isdigit(item) && item != 'A' && item != 'B')) {
                if (item != '0') {
                    s->c->warnings++;
                }
                s->current_ms += increment;
                continue;
            }
            /* Repeated 9s are one kusudama, even with barlines between them */
            if (item == '9') {
                const tp_note *last = last_real_note(s);
                if (last && last->type == TP_KUSUDAMA) {
                    s->current_ms += increment;
                    continue;
                }
            }
            add_note(s, item);
            s->current_ms += increment;
            s->index++;
        }
    }
}

bool tja_chart(tp_tja *t, int diff, int player, tp_chart *c) {
    c->master_count = c->branch_count = c->event_count = c->section_count = c->tempo_count = 0;
    c->master_event_count = 0;
    c->overflow = false;
    c->warnings = 0;
    c->scroll_type = TP_NMSCROLL;
    c->lead_bpm = t->bpm;
    if (diff < 0 || diff >= TJA_COURSES || !t->course[diff].present) {
        return false;
    }
    if (c->tempo_cap > 0) {
        c->tempo[0].ms = 0.0;
        c->tempo[0].beat = 0.0;
        c->tempo[0].bpm = t->bpm;
        c->tempo_count = 1;
    }
    int note_start, note_end, find_scroll;
    if (!tja_find_chart(t, diff, player, &note_start, &note_end, &find_scroll)) {
        return true; /* YataiDON: a course without a chart is an empty chart */
    }

    parser_state s;
    memset(&s, 0, sizeof s);
    s.t = t;
    s.c = c;
    s.time_signature = 4.0f / 4.0f;
    s.bpm = t->bpm;
    s.scroll_x_modifier = 1.0;
    s.barline_display = true;
    s.branch = -1;
    s.start_branch_bpm = 120.0;
    s.start_branch_time_sig = 4.0f / 4.0f;
    s.start_branch_x_scroll = 1.0;
    s.last_section[0] = s.last_section[1] = s.last_section[2] = -1;

    /* use the header values in effect at this chart's #START (STYLE sections) */
    const tja_course *cd = &t->course[diff];
    s.balloons = cd->balloon;
    for (int i = 0; i < t->start_count; i++) {
        if (t->starts[i].line == note_start) {
            s.balloons = t->starts[i].balloon;
            break;
        }
    }
    for (int i = 0; i < 3; i++) {
        s.route_balloons[i] = cd->branch_balloon[i];
    }

    static tp_str bar[MAX_PARTS];
    static const char *scroll_cmd[3] = {"#NMSCROLL", "#BMSCROLL", "#HBSCROLL"};
    int n = 0;
    bar[n++] = tp_str_make(scroll_cmd[find_scroll], 9);
    for (int i = note_start; i < note_end; i++) {
        tp_str line = t->lines[i];
        bool close = false;
        if (is_header_line(line)) {
            continue; /* e.g. EXAM4: after #NEXTSONG in dan charts */
        }
        if (line.p[0] == '#') {
            if (tp_starts_with(line, "#NMSCROLL") || tp_starts_with(line, "#BMSCROLL") ||
                tp_starts_with(line, "#HBSCROLL")) {
                continue;
            }
        } else if (tp_equals(line, ",")) {
            bool all_commands = true;
            for (int k = 0; k < n; k++) {
                all_commands = all_commands && bar[k].p[0] == '#';
            }
            if (!all_commands) {
                process_bar(&s, bar, n);
                n = 0;
                continue;
            }
            line.n = 0;
            close = true;
        } else if (line.p[line.n - 1] == ',') {
            line.n--;
            close = true;
        }
        if (n >= MAX_PARTS) {
            c->overflow = true;
            continue;
        }
        bar[n++] = line;
        if (close) {
            process_bar(&s, bar, n);
            n = 0;
        }
    }
    if (n) {
        process_bar(&s, bar, n);
    }
    c->scroll_type = (uint8_t)s.scroll_type;

    /* group the events by list (master first, then each section), keeping their order */
    for (int i = 1; i < c->event_count; i++) {
        tp_event e = c->events[i];
        int j = i;
        while (j > 0 && c->events[j - 1].list > e.list) {
            c->events[j] = c->events[j - 1];
            j--;
        }
        c->events[j] = e;
    }
    c->master_event_count = s.list_events[0];
    int first = c->master_event_count;
    for (int i = 0; i < c->section_count; i++) {
        c->sections[i].first_event = first;
        first += c->sections[i].event_count;
    }
    return true;
}
