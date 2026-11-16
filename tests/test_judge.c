/* Judge: timing windows, scoring, gauge, drumrolls, balloons, branches, auto-play.
 * (Fidelity to YataiDON's JudgeCore is covered separately by `make golden`.) */
#include <stdlib.h>

#include "core/judge.h"
#include "core/tja.h"
#include "test.h"

#define NOTES 4096
static tp_note master[NOTES], branch[NOTES];
static tp_event events[512];
static tp_section sections[64];
static tp_tempo_point tempo[64];
static tp_str lines[2048];
static const tp_note *lane_buf[3 * 2 * NOTES];
static const tp_event *timeline_buf[512];
static tp_tja tja;
static tp_chart chart;
static tp_judge j;
static char text[65536];

static void load_text(const char *src, int course, bool auto_play, bool extensions) {
    size_t n = strlen(src);
    memcpy(text, src, n + 1);
    tja_load(&tja, text, n, lines, 2048, true);
    tp_chart c = {master, NOTES, branch, NOTES, events, 512, sections, 64, tempo, 64};
    chart = c;
    tja_chart(&tja, course, 0, &chart);
    tp_modifiers mods = {false, false, 1.0f};
    tp_chart_prepare(&chart, &mods, NULL);
    tp_judge_load(&j, &chart, course, (int)tja.course[course].level, auto_play, extensions, NULL, lane_buf,
                  3 * 2 * NOTES, timeline_buf, 512);
}

static void load_file(const char *path, int course, bool auto_play, bool extensions) {
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  cannot open %s\n", path);
        exit(1);
    }
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    load_text(buf, course, auto_play, extensions);
}

/* one frame at `ms` with one hit at that time (drum 0 = no hit) */
static void frame(double ms, tp_drum drum) {
    tp_judge_begin_frame(&j, ms);
    if (drum) {
        tp_judge_hit(&j, ms, drum);
    }
    tp_judge_end_frame(&j, ms);
}

/* play the whole chart at 1 ms steps; hit_all: hit every note exactly on time */
static void play(bool perfect, int rolls_per_second, double offset_ms) {
    double next_roll = -1e9;
    for (double t = -3000; t < j.chart_end_ms + 3000; t += 1.0) {
        tp_judge_begin_frame(&j, t);
        if (perfect) {
            while (j.don.head < j.don.tail && j.don.v[j.don.head]->hit_ms + offset_ms <= t) {
                tp_judge_hit(&j, j.don.v[j.don.head]->hit_ms + offset_ms, TP_DRUM_DON);
            }
            while (j.kat.head < j.kat.tail && j.kat.v[j.kat.head]->hit_ms + offset_ms <= t) {
                tp_judge_hit(&j, j.kat.v[j.kat.head]->hit_ms + offset_ms, TP_DRUM_KAT);
            }
        }
        if ((j.is_drumroll || j.is_balloon) && rolls_per_second && t >= next_roll) {
            tp_judge_hit(&j, t, TP_DRUM_DON);
            next_roll = t + 1000.0 / rolls_per_second;
        }
        tp_judge_end_frame(&j, t);
    }
}

/* four dons a bar at 120 BPM: notes at 0, 500, 1000, 1500, ... */
static const char *QUARTERS = "BPM:120\nCOURSE:%s\nLEVEL:%d\n#START\n1111,\n1111,\n1111,\n1111,\n1111,\n#END\n";

static void quarters(const char *course_name, int level, int course) {
    char src[256];
    snprintf(src, sizeof src, QUARTERS, course_name, level);
    load_text(src, course, false, false);
}

int main(void) {
    /* --- timing windows, Oni: GOOD 25.025, OK 75.075, BAD 108.442 ms --- */
    quarters("Oni", 8, 3);
    CHECK_EQ(j.judgeable_note_count, 20);
    CHECK_EQ(j.base_score, 50000);
    frame(25.0, TP_DRUM_DON); /* note at 0 */
    CHECK_EQ(j.good_count, 1);
    frame(500 - 25.03, TP_DRUM_DON); /* just outside GOOD, early */
    CHECK_EQ(j.ok_count, 1);
    frame(1000 + 75.0, TP_DRUM_DON);
    CHECK_EQ(j.ok_count, 2);
    CHECK_EQ(j.combo, 3);
    frame(1500 + 75.1, TP_DRUM_DON); /* outside OK: BAD, combo broken */
    CHECK_EQ(j.bad_count, 1);
    CHECK_EQ(j.combo, 0);
    frame(2000 - 108.5, TP_DRUM_DON); /* too early for anything: ignored */
    CHECK_EQ(j.good_count + j.ok_count + j.bad_count, 4);
    frame(2000 - 108.4, TP_DRUM_DON); /* inside BAD */
    CHECK_EQ(j.bad_count, 2);
    frame(2500, TP_DRUM_KAT); /* wrong drum: nothing */
    CHECK_EQ(j.good_count, 1);
    frame(2500 + 108.5, 0); /* passes unhit: BAD */
    CHECK_EQ(j.bad_count, 3);
    CHECK_EQ(j.score, 50000 + 2 * 25000); /* OK is half, rounded down to a multiple of 10 */
    CHECK_EQ(j.max_combo, 3);

    /* --- Easy/Normal windows are wider: GOOD 41.708, OK 108.442, BAD 125.125 --- */
    quarters("Easy", 3, 0);
    frame(41.7, TP_DRUM_DON);
    CHECK_EQ(j.good_count, 1);
    frame(500 + 108.4, TP_DRUM_DON);
    CHECK_EQ(j.ok_count, 1);
    frame(1000 + 125.0, TP_DRUM_DON);
    CHECK_EQ(j.bad_count, 1);

    /* --- hitting toward the next note gives up on the one before --- */
    quarters("Oni", 8, 3);
    frame(500 - 20, TP_DRUM_DON); /* note 0 was never hit; this is clearly meant for the note at 500 */
    CHECK_EQ(j.bad_count, 1);
    CHECK_EQ(j.good_count, 1);
    CHECK_EQ(j.combo, 1);

    /* --- gauge: clear line and full gauge --- */
    quarters("Oni", 8, 3);
    play(true, 0, 0);
    CHECK_EQ(j.good_count, 20);
    CHECK(tp_gauge_clear(&j.gauge) && tp_gauge_rainbow(&j.gauge));
    CHECK_EQ(tp_gauge_percent(&j.gauge), 100);
    CHECK_EQ(j.score, 1000000);
    quarters("Oni", 8, 3);
    play(true, 0, 50); /* all OK: half the points per note */
    CHECK_EQ(j.ok_count, 20);
    CHECK_EQ(j.score, 500000);
    CHECK(!tp_gauge_clear(&j.gauge)); /* Oni needs 80%: 20 OKs at 0.5 x 100/70 fall short */
    quarters("Easy", 1, 0);
    play(true, 0, 80);
    CHECK_EQ(j.ok_count, 20);
    CHECK(tp_gauge_clear(&j.gauge)); /* Easy clears at 60% */
    quarters("Oni", 8, 3);
    play(false, 0, 0);
    CHECK_EQ(j.bad_count, 20);
    CHECK_EQ(tp_gauge_percent(&j.gauge), 0);
    CHECK(tp_judge_finished(&j));

    /* --- drumroll and balloon: 100 points a hit; a balloon bursts at its count --- */
    load_text("BPM:120\nCOURSE:Oni\nLEVEL:8\nBALLOON:5\n#START\n5000000000000008,\n7000000000000008,\n1,\n#END\n", 3,
              false, false);
    play(true, 10, 0);
    CHECK_EQ(j.good_count, 1);
    CHECK(j.total_drumroll >= 19 + 5 && j.total_drumroll <= 20 + 5);
    CHECK_EQ(j.score, j.base_score + 100 * j.total_drumroll);
    CHECK_EQ(j.combo, 1); /* rolls and balloons do not add combo */
    bool burst = false;
    tp_judge_event e;
    while (tp_judge_next_event(&j, &e)) {
        burst = burst || (e.kind == TP_JE_BALLOON_HIT && e.flag);
    }
    (void)burst; /* the queue only keeps the newest events; the counters above are the check */

    /* a balloon only counts don hits */
    load_text("BPM:120\nCOURSE:Oni\nLEVEL:8\nBALLOON:3\n#START\n7000000000000008,\n#END\n", 3, false, false);
    frame(10, TP_DRUM_KAT);
    CHECK_EQ(j.total_drumroll, 0);
    frame(20, TP_DRUM_DON);
    frame(30, TP_DRUM_DON);
    CHECK(j.is_balloon);
    frame(40, TP_DRUM_DON);
    CHECK(!j.is_balloon);
    CHECK_EQ(j.total_drumroll, 3);

    /* --- branches (tests/data/songs/Basics/branch.tja, Oni) --- */
    load_file("tests/data/songs/Basics/branch.tja", 3, false, true);
    play(true, 20, 0); /* 100% accuracy -> master; then enough roll hits -> master */
    CHECK_EQ(j.branch_decisions, 3);
    CHECK_EQ(j.current_branch, TP_BRANCH_MASTER);
    CHECK_EQ(j.bad_count, 0);

    load_file("tests/data/songs/Basics/branch.tja", 3, false, true);
    play(false, 0, 0); /* nothing hit -> normal */
    CHECK_EQ(j.current_branch, TP_BRANCH_NORMAL);
    CHECK_EQ(j.good_count, 0);

    /* One route per branch point, and no #BRANCHEND (seen in the wild): only #M at the first
     * #BRANCHSTART, only #N at the second, only #M at the third, with conditions that
     * force exactly those. Every section must be played at its own branch point. */
    static const char *ONE_ROUTE =
        "BPM:120\nCOURSE:Oni\nLEVEL:8\n#START\n1111,\n1111,\n1111,\n"
        "#BRANCHSTART p,-2,-1\n#M\n1111,\n1111,\n1111,\n"
        "#BRANCHSTART p,101,101\n#N\n2222,\n2222,\n2222,\n"
        "#BRANCHSTART p,-2,-1\n#M\n1212,\n1212,\n#END\n";
    load_text(ONE_ROUTE, 3, false, true);
    play(true, 0, 0);
    CHECK_EQ(j.branch_decisions, 3);
    CHECK_EQ(j.current_branch, TP_BRANCH_MASTER);
    CHECK_EQ(j.good_count, 12 + 12 + 12 + 8);
    CHECK_EQ(j.bad_count, 0);
    CHECK(tp_judge_finished(&j));
    /* as YataiDON does it, the first decision throws the #N section away with the routes not taken */
    load_text(ONE_ROUTE, 3, false, false);
    play(true, 0, 0);
    CHECK(j.good_count < 12 + 12 + 12 + 8);

    /* #LEVELHOLD (extension) keeps the route of the second branch for the third */
    load_file("tests/data/songs/Basics/branch.tja", 3, false, true);
    for (double t = -3000; t < j.chart_end_ms + 3000; t += 1.0) {
        tp_judge_begin_frame(&j, t);
        if (j.branch_decisions < 2) { /* play perfectly up to the second decision, then stop */
            while (j.don.head < j.don.tail && j.don.v[j.don.head]->hit_ms <= t) {
                tp_judge_hit(&j, j.don.v[j.don.head]->hit_ms, TP_DRUM_DON);
            }
            while (j.kat.head < j.kat.tail && j.kat.v[j.kat.head]->hit_ms <= t) {
                tp_judge_hit(&j, j.kat.v[j.kat.head]->hit_ms, TP_DRUM_KAT);
            }
            if (j.is_drumroll && (int)t % 350 == 0) {
                tp_judge_hit(&j, t, TP_DRUM_DON); /* 4 or 5 hits on the roll: expert (>= 3, < 6) */
            }
        }
        tp_judge_end_frame(&j, t);
    }
    CHECK_EQ(j.branch_decisions, 3);
    CHECK_EQ(j.current_branch, TP_BRANCH_EXPERT); /* held, although the last section was all missed */
    /* without the extension the third decision follows the accuracy: normal */
    load_file("tests/data/songs/Basics/branch.tja", 3, false, false);
    for (double t = -3000; t < j.chart_end_ms + 3000; t += 1.0) {
        tp_judge_begin_frame(&j, t);
        if (j.branch_decisions < 2) {
            while (j.don.head < j.don.tail && j.don.v[j.don.head]->hit_ms <= t) {
                tp_judge_hit(&j, j.don.v[j.don.head]->hit_ms, TP_DRUM_DON);
            }
            while (j.kat.head < j.kat.tail && j.kat.v[j.kat.head]->hit_ms <= t) {
                tp_judge_hit(&j, j.kat.v[j.kat.head]->hit_ms, TP_DRUM_KAT);
            }
            if (j.is_drumroll && (int)t % 350 == 0) {
                tp_judge_hit(&j, t, TP_DRUM_DON);
            }
        }
        tp_judge_end_frame(&j, t);
    }
    CHECK_EQ(j.current_branch, TP_BRANCH_NORMAL);

    /* 's' condition (extension): score gained since #SECTION */
    static const char *SCORE_BRANCH =
        "BPM:120\nCOURSE:Oni\nLEVEL:8\n#START\n1111,\n1111,\n#BRANCHSTART s,100000,400000\n#N\n1,\n#E\n2,\n#M\n3,\n"
        "#BRANCHEND\n1,\n#END\n";
    load_text(SCORE_BRANCH, 3, false, true);
    play(true, 0, 0);
    CHECK_EQ(j.current_branch, TP_BRANCH_MASTER);
    load_text(SCORE_BRANCH, 3, false, true);
    play(false, 0, 0);
    CHECK_EQ(j.current_branch, TP_BRANCH_NORMAL);

    /* --- auto-play: everything GOOD, balloons burst, at a coarse frame rate too --- */
    load_file("tests/data/songs/Basics/basic.tja", 3, true, true);
    for (double t = -1000; t < j.chart_end_ms + 2000; t += 33.3) {
        tp_judge_begin_frame(&j, t);
        tp_judge_autoplay(&j, t, 120);
        tp_judge_end_frame(&j, t);
    }
    CHECK_EQ(j.bad_count, 0);
    CHECK_EQ(j.ok_count, 0);
    CHECK_EQ(j.good_count, j.judgeable_note_count);
    CHECK(j.total_drumroll > 0);
    CHECK(tp_gauge_rainbow(&j.gauge));

    /* --- the chart ends with its last note, not with bar lines that come after it: a
     * chart with empty measures at a tempo near zero after its last note ended an hour
     * after the music --- */
    load_text("BPM:120\nCOURSE:Oni\nLEVEL:8\n#START\n1111,\n#BARLINEOFF\n#BPMCHANGE 0.5\n,\n,\n,\n#END\n", 3, false, false);
    CHECK_EQ(j.judgeable_note_count, 4);
    CHECK_NEAR(j.chart_end_ms, 1500.0, 0.001);
    TEST_MAIN_END;
}
