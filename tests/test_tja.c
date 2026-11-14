/* TJA parser: hand-checked timings and the Taikojiro compatibility rules.
 * (Fidelity to YataiDON is covered separately by `make golden`.) */
#include <stdlib.h>

#include "core/tja.h"
#include "test.h"

static tp_note master[4096], branch[4096];
static tp_event events[512];
static tp_section sections[64];
static tp_tempo_point tempo[64];
static tp_str lines[2048];
static tp_tja tja;
static tp_chart chart;
static char *text;

static void load(const char *path, bool compat) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("  cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    free(text);
    text = malloc((size_t)size);
    size = (long)fread(text, 1, (size_t)size, f);
    fclose(f);
    CHECK(tja_load(&tja, text, (size_t)size, lines, 2048, compat));
}

static bool build(int course, int player) {
    tp_chart c = {master, 4096, branch, 4096, events, 512, sections, 64, tempo, 64};
    chart = c;
    if (!tja_chart(&tja, course, player, &chart)) {
        return false;
    }
    tp_modifiers mods = {false, false, 1.0f};
    tp_chart_prepare(&chart, &mods, NULL);
    return true;
}

static int count_type(const tp_note *n, int count, int type) {
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += n[i].type == type;
    }
    return total;
}

int main(void) {
    /* --- basic.tja, Easy: "1," "2," "1," at 120 BPM, 4/4 --- */
    load("tests/data/songs/Basics/basic.tja", true);
    CHECK_NEAR(tja.bpm, 120, 0);
    CHECK_NEAR(tja.offset, -1.5, 0);
    CHECK_NEAR(tja.demostart, 4.0, 0);
    CHECK_NEAR(tja.songvol, 90, 0);
    CHECK(tp_equals(tja.title, "Basic Test"));
    CHECK(tp_equals(tja.subtitle, "Taiko Pocket tests"));
    CHECK(!tja.subtitle_full);
    CHECK_NEAR(tja.course[1].level, 4, 0); /* LEVEL:4.5 truncates */
    CHECK(!tja.course[4].present);
    CHECK(build(0, 0));
    CHECK_EQ(chart.master_count, 6); /* 3 bar lines + 3 notes */
    CHECK_EQ(master[0].type, TP_BARLINE);
    CHECK_EQ(master[1].type, TP_DON);
    CHECK_NEAR(master[1].hit_ms, 0, 0);
    CHECK_EQ(master[3].type, TP_KAT);
    CHECK_NEAR(master[3].hit_ms, 2000, 1e-9); /* one 4/4 bar at 120 BPM */
    CHECK_NEAR(master[5].hit_ms, 4000, 1e-9);
    /* on screen for (948 + 64) / 948 of a bar's duration before the hit */
    CHECK_NEAR(master[3].load_ms, 2000 - 2000.0 * 1012 / 948, 0.01);
    CHECK_EQ(tp_chart_base_score(&chart, NULL), 333340);

    /* Oni: balloons take their counts from BALLOON:5,12 in order */
    CHECK(build(3, 0));
    int balloons = 0;
    for (int i = 0; i < chart.master_count; i++) {
        if (master[i].type == TP_BALLOON_HEAD) {
            CHECK_EQ(master[i].count, balloons == 0 ? 5 : 12);
            balloons++;
        } else {
            CHECK_EQ(master[i].count, TP_NO_COUNT);
        }
    }
    CHECK_EQ(balloons, 2);
    int gogo = 0, bpm_changes = 0;
    for (int i = 0; i < chart.master_event_count; i++) {
        gogo += events[i].kind == TP_EV_GOGO;
        bpm_changes += events[i].kind == TP_EV_BPM;
    }
    CHECK_EQ(gogo, 2);
    CHECK_EQ(bpm_changes, 2);
    CHECK(!chart.overflow);

    /* --- branch.tja: three paths per branch, each with its own state --- */
    load("tests/data/songs/Basics/branch.tja", true);
    CHECK(tja.subtitle_full);
    CHECK(tja.course[3].is_branching);
    CHECK(build(3, 0));
    CHECK_EQ(chart.section_count, 9);
    for (int s = 0; s < chart.section_count; s++) {
        CHECK_EQ(sections[s].path, s % 3);
    }
    /* the three paths of the first branch start at the same time */
    CHECK_NEAR(branch[sections[0].first_note].hit_ms, branch[sections[1].first_note].hit_ms, 0);
    CHECK_NEAR(branch[sections[0].first_note].hit_ms, branch[sections[2].first_note].hit_ms, 0);
    CHECK(branch[sections[0].first_note].flags & TP_NOTE_BRANCH_START);
    /* the expert path changes BPM, the master path changes scroll; neither leaks */
    CHECK_NEAR(branch[sections[1].first_note + sections[1].note_count - 1].bpm, 200, 0);
    CHECK_NEAR(branch[sections[2].first_note + sections[2].note_count - 1].scroll_x, 1.5, 0);
    CHECK_NEAR(branch[sections[0].first_note + sections[0].note_count - 1].bpm, 150, 0);
    int branch_events = 0, holds = 0, section_events = 0;
    for (int i = 0; i < chart.event_count; i++) {
        if (events[i].kind == TP_EV_BRANCH && events[i].list == 0) {
            CHECK(events[i].parts_ok);
            CHECK(branch_events != 0 || (events[i].cond == 'p' && events[i].e_req == 60 && events[i].m_req == 90));
            CHECK(branch_events != 1 || (events[i].cond == 'r' && events[i].e_req == 3 && events[i].m_req == 6));
            branch_events++;
        }
        holds += events[i].kind == TP_EV_LEVELHOLD;
        section_events += events[i].kind == TP_EV_SECTION;
    }
    CHECK_EQ(branch_events, 3);
    CHECK_EQ(holds, 1);
    CHECK_EQ(section_events, 2);
    /* Edit: BALLOONNOR/EXP/MAS give each path its own counts; BALLOON covers the rest */
    CHECK(build(4, 0));
    CHECK_EQ(master[1].count, 2); /* outside the branches every route list advances; path lists win */
    CHECK_EQ(branch[sections[0].first_note + 1].count, 2);
    CHECK_EQ(branch[sections[1].first_note + 1].count, 3);
    CHECK_EQ(branch[sections[2].first_note + 1].count, 4);

    /* --- compat: a Taikojiro-style chart (no COURSE:, no #END) --- */
    load("tests/data/songs/Basics/taikojiro.tja", false);
    CHECK(!tja.course[3].present); /* YataiDON finds nothing */
    load("tests/data/songs/Basics/taikojiro.tja", true);
    CHECK(tja.course[3].present);
    CHECK_NEAR(tja.course[3].level, 7, 0);
    CHECK(build(3, 0));
    CHECK_EQ(count_type(master, chart.master_count, TP_BARLINE), 3);
    CHECK_EQ(count_type(master, chart.master_count, TP_DON), 6);
    CHECK_EQ(count_type(master, chart.master_count, TP_BALLOON_HEAD), 1);
    for (int i = 0; i < chart.master_count; i++) {
        if (master[i].type == TP_BALLOON_HEAD) {
            CHECK_EQ(master[i].count, 6);
        }
    }

    /* --- double.tja: single chart by default, P1/P2 on request; STYLE restarts BALLOON --- */
    load("tests/data/songs/Basics/double.tja", true);
    CHECK(build(3, 0));
    CHECK_EQ(master[1].count, 4);
    CHECK_EQ(count_type(master, chart.master_count, TP_DON), 1);
    CHECK(build(3, 1));
    CHECK_EQ(master[1].count, 9);
    CHECK_EQ(count_type(master, chart.master_count, TP_DON), 2);
    CHECK(build(3, 2));
    CHECK_EQ(master[1].count, 2);
    CHECK_EQ(count_type(master, chart.master_count, TP_KAT), 2);
    CHECK(build(2, 0)); /* a course with only P1/P2 charts plays P1 */
    CHECK_EQ(count_type(master, chart.master_count, TP_DON), 1);
    CHECK_NEAR(tja.course[2].level, 3, 0); /* COURSEDESIGNER2: did not reset the course */

    /* --- kusudama.tja: repeated 9s merge, A/B are big notes, odd characters are blanks --- */
    load("tests/data/songs/Gimmicks/kusudama.tja", true);
    CHECK(build(3, 0));
    CHECK_EQ(count_type(master, chart.master_count, TP_KUSUDAMA), 2); /* bars 1-2 are one kusudama */
    CHECK_EQ(count_type(master, chart.master_count, TP_DON_L), 3);
    CHECK_EQ(count_type(master, chart.master_count, TP_KAT_L), 3);
    int lyrics = 0;
    for (int i = 0; i < chart.master_event_count; i++) {
        lyrics += events[i].kind == TP_EV_LYRIC;
    }
    CHECK_EQ(lyrics, 3);

    /* --- beat scroll: the tempo map stops during #DELAY and runs back with negative BPM --- */
    load("tests/data/songs/Gimmicks/hbscroll.tja", true);
    CHECK(build(3, 0));
    CHECK_EQ(chart.scroll_type, TP_HBSCROLL);
    CHECK(chart.tempo_count > 4);
    bool stopped = false, backwards = false;
    for (int i = 0; i < chart.tempo_count; i++) {
        stopped = stopped || tempo[i].bpm == 0;
        backwards = backwards || tempo[i].bpm < 0;
    }
    CHECK(stopped && backwards);
    CHECK(build(2, 0));
    CHECK_EQ(chart.scroll_type, TP_BMSCROLL);
    CHECK_NEAR(master[1].scroll_x, 1, 0); /* #SCROLL is ignored under #BMSCROLL */

    /* --- modifiers (YataiDON: display, inverse, random); they can be combined --- */
    load("tests/data/songs/Basics/basic.tja", true);
    CHECK(build(3, 0));
    static uint8_t plain[4096];
    int notes = 0, swappable = 0;
    for (int i = 0; i < chart.master_count; i++) {
        plain[i] = master[i].type;
        notes += master[i].type != TP_BARLINE;
        swappable += master[i].type >= TP_DON && master[i].type <= TP_KAT_L;
        CHECK(master[i].type == TP_BARLINE || (master[i].flags & TP_NOTE_DISPLAY));
    }
    CHECK(notes >= 20 && swappable >= 10);
    int changed[3] = {0, 0, 0};
    for (int value = 1; value <= 2; value++) {
        for (int seed = 1; seed <= 2; seed++) {
            tp_chart c = {master, 4096, branch, 4096, events, 512, sections, 64, tempo, 64};
            chart = c;
            CHECK(tja_chart(&tja, 3, 0, &chart));
            tp_modifiers mods = {false, false, 1.0f, true, (uint8_t)value, (uint32_t)seed};
            tp_chart_prepare(&chart, &mods, NULL);
            int diff = 0, hidden = 0, bars_shown = 0, first_diff = -1;
            for (int i = 0; i < chart.master_count; i++) {
                bool swapped = master[i].type != plain[i];
                diff += swapped;
                first_diff = swapped && first_diff < 0 ? i : first_diff;
                /* only don and ka change, and only into each other */
                CHECK(!swapped || (plain[i] >= TP_DON && plain[i] <= TP_KAT_L && master[i].type >= TP_DON &&
                                   master[i].type <= TP_KAT_L && (plain[i] > TP_KAT) == (master[i].type > TP_KAT)));
                hidden += master[i].type != TP_BARLINE && !(master[i].flags & TP_NOTE_DISPLAY);
                bars_shown += master[i].type == TP_BARLINE && (master[i].flags & TP_NOTE_DISPLAY);
            }
            CHECK_EQ(hidden, notes);   /* invisible: every note, no bar line */
            CHECK(bars_shown > 0);
            CHECK(diff > 0 && diff <= notes / 5 * value); /* a fifth or two fifths are picked; those that are don or ka swap */
            changed[value] += diff;
            CHECK(first_diff >= 0);
        }
    }
    CHECK(changed[2] > changed[1]);
    {   /* inverse and random together: every note that random leaves alone is swapped */
        tp_chart c = {master, 4096, branch, 4096, events, 512, sections, 64, tempo, 64};
        chart = c;
        CHECK(tja_chart(&tja, 3, 0, &chart));
        tp_modifiers mods = {false, true, 1.0f, false, 1, 7};
        tp_chart_prepare(&chart, &mods, NULL);
        int same = 0;
        for (int i = 0; i < chart.master_count; i++) {
            same += plain[i] >= TP_DON && plain[i] <= TP_KAT_L && master[i].type == plain[i];
        }
        CHECK(same > 0 && same <= notes / 5);
    }

    /* --- storage limits are reported, not overrun --- */
    load("tests/data/songs/Basics/basic.tja", true);
    tp_chart tiny = {master, 4, branch, 4, events, 1, sections, 1, tempo, 1};
    CHECK(tja_chart(&tja, 3, 0, &tiny));
    CHECK(tiny.overflow);
    CHECK_EQ(tiny.master_count, 4);
    TEST_MAIN_END;
}
