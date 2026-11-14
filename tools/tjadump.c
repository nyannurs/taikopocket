/* tjadump [--compat] FILE.tja...: dumps every course of each chart as text.
 * tests/yatai/yatai_dump.cpp prints the same from YataiDON's parser; the
 * golden test diffs the two. Files must be UTF-8. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/tja.h"

#define MAX_NOTES 60000
#define MAX_EVENTS 8192
#define MAX_SECTIONS 2048
#define MAX_TEMPO 8192
#define MAX_LINES 60000

static tp_note master[MAX_NOTES], branch[MAX_NOTES];
static tp_event events[MAX_EVENTS];
static tp_section sections[MAX_SECTIONS];
static tp_tempo_point tempo[MAX_TEMPO];
static tp_str lines[MAX_LINES];
static tp_tja tja;

static void dump_notes(const char *list, const tp_note *n, int count) {
    for (int i = 0; i < count; i++, n++) {
        printf("N %s %d t=%d i=%d hit=%.17g bpm=%.17g sx=%.17g sy=%.17g d=%d bs=%d br=%d p=%d", list, i, n->type,
               n->index, n->hit_ms, n->bpm, n->scroll_x, n->scroll_y, !!(n->flags & TP_NOTE_DISPLAY),
               !!(n->flags & TP_NOTE_BAR_START), !!(n->flags & TP_NOTE_BRANCH_START),
               !!(n->flags & TP_NOTE_PARTNER));
        if (n->count != TP_NO_COUNT) {
            printf(" c=%d", n->count);
        }
        if (n->flags & TP_NOTE_SUDDEN) {
            printf(" sa=%.17g sm=%.17g", n->sudden_appear_ms, n->sudden_moving_ms);
        }
        printf(" load=%.17g unload=%.17g m=%d\n", n->load_ms, n->unload_ms, n->moji);
    }
}

static void dump_events(const char *list, const tp_event *e, int count) {
    for (int i = 0; i < count; i++, e++) {
        switch (e->kind) {
        case TP_EV_BPM: printf("E %s bpm %.17g %.17g\n", list, e->start_ms, e->bpm); break;
        case TP_EV_GOGO: printf("E %s gogo %.17g %d\n", list, e->start_ms, e->gogo); break;
        case TP_EV_BRANCH:
            printf("E %s branch %.17g ok=%d cond=%c e=%.9g m=%.9g\n", list, e->start_ms, e->parts_ok,
                   e->cond == 'p' || e->cond == 'r' ? e->cond : '-', e->parts_ok ? e->e_req : 0.0, e->parts_ok ? e->m_req : 0.0);
            break;
        case TP_EV_SECTION: printf("E %s section %.17g\n", list, e->start_ms); break;
        case TP_EV_JPOS:
            printf("E %s jpos %.17g %.17g jx=%.17g jy=%.17g dx=%.17g dy=%.17g\n", list, e->start_ms, e->end_ms,
                   e->judge_x, e->judge_y, e->delta_x, e->delta_y);
            break;
        case TP_EV_LYRIC: printf("E %s lyric %.17g [%.*s]\n", list, e->start_ms, (int)e->text_len, e->text); break;
        case TP_EV_LEVELHOLD: printf("E %s levelhold %.17g\n", list, e->start_ms); break;
        }
    }
}

static void dump_ints(const char *name, const tp_tja *t, tja_ints v) {
    printf(" %s=[", name);
    for (int i = 0; i < v.count; i++) {
        printf("%s%d", i ? "," : "", t->ints[v.first + i]);
    }
    printf("]");
}

int main(int argc, char **argv) {
    bool compat = false;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--compat")) {
            compat = true;
            continue;
        }
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
        printf("#file %s\n", name ? name + 1 : argv[a]);
        tja_load(&tja, text, (size_t)size, lines, MAX_LINES, compat);
        printf("meta bpm=%.17g offset=%.17g demostart=%.17g title=[%.*s] subtitle=[%.*s] full=%d\n", tja.bpm,
               tja.offset, tja.demostart, (int)tja.title.n, tja.title.p, (int)tja.subtitle.n, tja.subtitle.p,
               tja.subtitle_full);
        for (int d = 0; d < TJA_COURSES; d++) {
            if (!tja.course[d].present) {
                continue;
            }
            tp_chart c = {master, MAX_NOTES, branch, MAX_NOTES, events, MAX_EVENTS, sections, MAX_SECTIONS,
                          tempo, MAX_TEMPO};
            tja_chart(&tja, d, 0, &c);
            tp_modifiers mods = {false, false, 1.0f};
            tp_chart_prepare(&c, &mods, NULL);
            printf("== course %d level=%.17g branching=%d scroll=%d base=%d", d, tja.course[d].level,
                   tja.course[d].is_branching, c.scroll_type, tp_chart_base_score(&c, NULL));
            if (c.overflow) {
                printf(" OVERFLOW");
            }
            dump_ints("nor", &tja, tja.course[d].branch_balloon[0]);
            dump_ints("exp", &tja, tja.course[d].branch_balloon[1]);
            dump_ints("mas", &tja, tja.course[d].branch_balloon[2]);
            printf("\n");
            dump_notes("master", c.master, c.master_count);
            dump_events("master", c.events, c.master_event_count);
            for (int p = 0; p < c.tempo_count; p++) {
                printf("P %.17g %.17g %.17g\n", c.tempo[p].ms, c.tempo[p].beat, c.tempo[p].bpm);
            }
            for (int path = 0; path < 3; path++) {
                int ordinal = 0;
                for (int s = 0; s < c.section_count; s++) {
                    if (c.sections[s].path != path) {
                        continue;
                    }
                    char list[16];
                    snprintf(list, sizeof list, "%c%d", "NEM"[path], ordinal++);
                    dump_notes(list, c.branch + c.sections[s].first_note, c.sections[s].note_count);
                    dump_events(list, c.events + c.sections[s].first_event, c.sections[s].event_count);
                }
            }
        }
        free(text);
    }
    return 0;
}
