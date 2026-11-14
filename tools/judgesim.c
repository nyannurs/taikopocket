/* judgesim LOGDIR FILE.tja...: for every course of each chart and each player
 * profile, plays the chart with simulated inputs, writes the inputs to
 * LOGDIR/<file>.<course>.<profile>.log and prints a trace of what the judge did.
 * tests/yatai/yatai_judge.cpp replays the same logs through YataiDON's
 * JudgeCore and prints the same trace; tests/replay.py compares them.
 *
 * Profiles: 0 perfect, 1 good, 2 average, 3 sloppy, 4 random mashing, 5 idle. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/judge.h"
#include "core/tja.h"

#define MAX_NOTES 60000
#define MAX_EVENTS 8192
#define MAX_SECTIONS 2048
#define MAX_TEMPO 8192
#define MAX_LINES 60000
#define PROFILES 6
#define FRAME_MS (1000.0 / 60.0)

static tp_note master[MAX_NOTES], branch[MAX_NOTES];
static tp_event events[MAX_EVENTS];
static tp_section sections[MAX_SECTIONS];
static tp_tempo_point tempo[MAX_TEMPO];
static tp_str lines[MAX_LINES];
static const tp_note *lane_buf[3 * 2 * MAX_NOTES];
static const tp_event *timeline_buf[MAX_EVENTS];
static tp_tja tja;
static tp_judge judge;
static unsigned char planned[4 * MAX_NOTES];

static const struct {
    double sigma, miss, wrong, roll_hz;
} profiles[PROFILES] = {{0, 0, 0, 16}, {12, 0, 0, 12}, {35, 0.05, 0.03, 9}, {70, 0.2, 0.1, 5}, {0, 1, 0, 0}, {0, 1, 0, 0}};

static unsigned rng_state;
static unsigned rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}
static double uniform(void) { return (rng() >> 8) / 16777216.0; }
static double gauss(void) { /* sum of uniforms: close enough to a bell curve */
    double s = 0;
    for (int i = 0; i < 6; i++) {
        s += uniform();
    }
    return (s - 3.0) * 1.41421356;
}

typedef struct {
    double ms;
    int drum;
} hit;
static hit pending[4096];
static int pending_count;

static void plan(double ms, int drum) {
    if (pending_count >= 4096) {
        return;
    }
    int i = pending_count++;
    while (i > 0 && pending[i - 1].ms > ms) {
        pending[i] = pending[i - 1];
        i--;
    }
    pending[i].ms = ms;
    pending[i].drum = drum;
}

static void plan_lane(const tp_lane *lane, int drum, int profile, double t) {
    for (int i = lane->head; i < lane->tail; i++) {
        const tp_note *n = lane->v[i];
        if (n->hit_ms - 220 > t) {
            break;
        }
        if (n->index < 0 || n->index >= (int)sizeof planned || planned[n->index]) {
            continue;
        }
        planned[n->index] = 1;
        if (uniform() < profiles[profile].miss) {
            continue;
        }
        double err = gauss() * profiles[profile].sigma;
        if (err > 160) {
            err = 160;
        } else if (err < -160) {
            err = -160;
        }
        int d = uniform() < profiles[profile].wrong ? 3 - drum : drum;
        double ms = n->hit_ms + err;
        if (ms < t - FRAME_MS / 2) { /* a branch note that appeared late: never date a hit before the previous frame */
            ms = t;
        }
        plan(ms, d);
    }
}

static void run(const char *name, int course, int profile, const char *logdir) {
    tp_chart c = {master, MAX_NOTES, branch, MAX_NOTES, events, MAX_EVENTS, sections, MAX_SECTIONS, tempo, MAX_TEMPO};
    tja_chart(&tja, course, 0, &c);
    tp_modifiers mods = {false, false, 1.0f};
    tp_chart_prepare(&c, &mods, NULL);
    tp_judge_load(&judge, &c, course, (int)tja.course[course].level, false, false, NULL, lane_buf,
                  3 * 2 * MAX_NOTES, timeline_buf, MAX_EVENTS);

    char path[1200];
    snprintf(path, sizeof path, "%s/%s.%d.%d.log", logdir, name, course, profile);
    FILE *log = fopen(path, "w");
    if (!log) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    printf("#run %s %d %d\n", name, course, profile);
    memset(planned, 0, sizeof planned);
    pending_count = 0;
    rng_state = 0x9E3779B9u ^ (unsigned)(course * 131 + profile * 7919 + 1);
    for (const char *p = name; *p; p++) {
        rng_state = rng_state * 31 + (unsigned char)*p;
    }
    rng_state |= 1;
    int history[512], history_count = 0;
    double next_roll = -1e18, next_mash = -1500;
    double end = judge.chart_end_ms + 3000;

    for (int k = 0;; k++) {
        double t = -2000.0 + k * FRAME_MS;
        if (t > end) {
            break;
        }
        tp_judge_begin_frame(&judge, t);
        if (profile == 4) { /* masher: hits whatever, 20 times a second */
            while (next_mash <= t) {
                plan(next_mash, uniform() < 0.5 ? 1 : 2);
                next_mash += 50;
            }
        } else if (profile != 5) {
            plan_lane(&judge.don, TP_DRUM_DON, profile, t);
            plan_lane(&judge.kat, TP_DRUM_KAT, profile, t);
            if ((judge.is_drumroll || judge.is_balloon) && profiles[profile].roll_hz > 0) {
                if (next_roll < t - FRAME_MS / 2) {
                    next_roll = t; /* never date a hit before the previous frame */
                }
                while (next_roll <= t) {
                    plan(next_roll, uniform() < 0.9 ? 1 : 2);
                    next_roll += 1000.0 / profiles[profile].roll_hz;
                }
            }
        }
        int used = 0;
        while (used < pending_count && pending[used].ms <= t) {
            fprintf(log, "%.17g %d\n", pending[used].ms, pending[used].drum);
            tp_judge_hit(&judge, pending[used].ms, (tp_drum)pending[used].drum);
            used++;
        }
        memmove(pending, pending + used, (size_t)(pending_count - used) * sizeof *pending);
        pending_count -= used;
        tp_judge_end_frame(&judge, t);

        tp_judge_event e;
        while (tp_judge_next_event(&judge, &e)) {
            if (e.kind == TP_JE_JUDGMENT) {
                printf("J %d %d %d\n", e.note->index, e.judgment, e.drum);
            } else if (e.kind == TP_JE_BRANCH && history_count < 512) {
                history[history_count++] = e.value;
            }
        }
    }
    fclose(log);
    printf("H");
    for (int i = 0; i < history_count; i++) {
        printf(" %d", history[i]);
    }
    printf("\nR score=%d good=%d ok=%d bad=%d combo=%d roll=%d clear=%d rainbow=%d gauge=%d\n", judge.score,
           judge.good_count, judge.ok_count, judge.bad_count, judge.max_combo, judge.total_drumroll,
           tp_gauge_clear(&judge.gauge), tp_gauge_rainbow(&judge.gauge), tp_gauge_percent(&judge.gauge));
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: judgesim LOGDIR FILE.tja...\n");
        return 2;
    }
    for (int a = 2; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        if (!f) {
            fprintf(stderr, "cannot open %s\n", argv[a]);
            return 1;
        }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *text = malloc((size_t)size + 1);
        size = (long)fread(text, 1, (size_t)size, f);
        fclose(f);
        const char *name = strrchr(argv[a], '/');
        name = name ? name + 1 : argv[a];
        tja_load(&tja, text, (size_t)size, lines, MAX_LINES, false);
        for (int course = 0; course < 5; course++) {
            if (!tja.course[course].present) {
                continue;
            }
            for (int profile = 0; profile < PROFILES; profile++) {
                run(name, course, profile, argv[1]);
            }
        }
        free(text);
    }
    return 0;
}
