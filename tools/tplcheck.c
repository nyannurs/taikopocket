/* tplcheck LIBRARY.tpl: loads every course of every song the way the game does (same
 * parser, same storage limits, same judge set-up) and reports what would not play.
 * Run it on a library packed with --audio none to check a song folder quickly. */
#include <stdio.h>
#include <string.h>

#include "game/game.h"
#include "host/host.h"

int main(int argc, char **argv) {
    if (argc < 2 || !host_open_library(argv[1])) {
        fprintf(stderr, "usage: tplcheck LIBRARY.tpl\n");
        return 2;
    }
    lib_boot_begin();
    lib_state st;
    while ((st = lib_boot_poll()) == LIB_LOADING) {
    }
    if (st != LIB_READY) {
        fprintf(stderr, "%s\n", lib_error());
        return 1;
    }
    settings_defaults();
    static const char *names[LIB_COURSES] = {"Easy", "Normal", "Hard", "Oni", "Edit"};
    int songs = 0, courses = 0, failed = 0, empty_audio = 0;
    int most_notes = 0, most_branch = 0, most_events = 0, most_sections = 0;
    uint32_t biggest_tja = 0;
    for (uint32_t i = 0; i < lib_song_count(); i++) {
        const lib_song *s = lib_get_song(i);
        songs++;
        if (s->tja_bytes > biggest_tja) {
            biggest_tja = s->tja_bytes;
        }
        bool any = false;
        for (int c = 0; c < LIB_COURSES; c++) {
            if (s->level[c] == LIB_NO_COURSE) {
                continue;
            }
            any = true;
            courses++;
            song_load(i, c);
            song_state state;
            int guard = 0;
            while ((state = song_poll()) == SONG_LOADING && ++guard < 100000) {
            }
            if (state != SONG_READY) {
                failed++;
                printf("FAIL  %s [%s]: %s\n", lib_string(s->title), names[c],
                       state == SONG_ERROR ? song_error() : "did not finish loading");
            } else {
                const tp_chart *ch = &song.chart;
                most_notes = ch->master_count > most_notes ? ch->master_count : most_notes;
                most_branch = ch->branch_count > most_branch ? ch->branch_count : most_branch;
                most_events = ch->event_count > most_events ? ch->event_count : most_events;
                most_sections = ch->section_count > most_sections ? ch->section_count : most_sections;
                int hittable = 0;
                for (int k = 0; k < ch->master_count && !hittable; k++) {
                    hittable = ch->master[k].type != TP_BARLINE;
                }
                if (!hittable) {
                    printf("NOTE  %s [%s]: no notes to hit (bar lines only)\n", lib_string(s->title), names[c]);
                }
            }
            song_unload();
        }
        if (!any) {
            failed++;
            printf("FAIL  %s: no course at all\n", lib_string(s->title));
        }
        if (s->audio_format == LIB_AUDIO_NONE) {
            empty_audio++;
        }
    }
    printf("%d songs, %d courses checked, %d problems\n", songs, courses, failed);
    printf("largest chart file %u bytes; most notes and bar lines %d, in branches %d, events %d, sections %d\n",
           (unsigned)biggest_tja, most_notes, most_branch, most_events, most_sections);
    if (empty_audio) {
        printf("%d songs have no audio in this library\n", empty_audio);
    }
    return failed ? 1 : 0;
}
