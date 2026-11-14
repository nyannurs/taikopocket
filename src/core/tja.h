/* TJA parser: a port of YataiDON's TJAParser (src/libs/parsers/tja.cpp) with
 * fixed memory. The chart text must be UTF-8 and stay alive while the parser
 * and the charts made from it are in use (lyrics point into it).
 *
 * With `compat` off the parser behaves exactly like YataiDON (this is what the
 * golden tests check). With it on, four habits of Taikojiro-era charts are
 * accepted as well; see docs/tja-notes.md. */
#ifndef TP_TJA_H
#define TP_TJA_H

#include <stddef.h>

#include "chart.h"
#include "num.h"

#define TJA_COURSES 7 /* easy, normal, hard, oni, edit, tower, dan */
#define TJA_MAX_INTS 4096
#define TJA_MAX_STARTS 64

typedef struct {
    int first, count; /* slice of tp_tja.ints */
} tja_ints;

typedef struct {
    bool present;
    double level;
    tja_ints balloon;
    tja_ints branch_balloon[3]; /* BALLOONNOR / BALLOONEXP / BALLOONMAS */
    tja_ints scoreinit;
    double scorediff;
    bool is_branching;
} tja_course;

typedef struct {
    /* input */
    tp_str *lines; /* cleaned lines: comments removed, trimmed, no empty ones */
    int line_count;
    bool compat;

    /* song metadata */
    tp_str title, subtitle, wave, genre;
    bool subtitle_full;
    double bpm, offset, demostart;
    double songvol, sevol; /* percent; not read by YataiDON */
    tja_course course[TJA_COURSES + 1]; /* [TJA_COURSES]: headers seen before any COURSE: (compat) */

    /* Width of the note field in pixels, for #JPOSSCROLL distances given as a fraction.
     * 0 (YataiDON's value until a skin sets it) makes those moves zero. */
    double jpos_field_width;

    /* internals */
    bool implicit_oni; /* compat: the chart started before any COURSE: line */
    struct {
        int line;         /* first line after a #START */
        tja_ints balloon; /* BALLOON: in effect there (STYLE sections restart it) */
    } starts[TJA_MAX_STARTS];
    int start_count;
    int ints[TJA_MAX_INTS];
    int int_count;
    int warnings;
} tp_tja;

/* Splits `text` into lines (using `line_buf`) and reads the metadata.
 * Returns false when the text has more lines than `line_cap`. */
bool tja_load(tp_tja *t, const char *text, size_t len, tp_str *line_buf, int line_cap, bool compat);

/* Builds the chart of one course (YataiDON notes_to_position). `player` is 0
 * for single play, 1 or 2 to pick a side of a double-play chart.
 * Returns false when the course has no chart. */
bool tja_chart(tp_tja *t, int course, int player, tp_chart *out);

/* Number of the course's first chart line and its end, or false. */
bool tja_find_chart(const tp_tja *t, int course, int player, int *start, int *end, int *scroll_type);

#endif
