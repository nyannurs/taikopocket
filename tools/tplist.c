/* tplist LIBRARY.tpl: prints a library's contents using the game's own loader.
 * The output format matches `pack.py --list`, which the tests rely on. */
#include <stdio.h>

#include "game/library.h"
#include "host/host.h"

int main(int argc, char **argv) {
    if (argc < 2 || !host_open_library(argv[1])) {
        fprintf(stderr, "usage: tplist LIBRARY.tpl\n");
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
    static const char *names[LIB_COURSES] = {"Easy", "Normal", "Hard", "Oni", "Edit"};
    for (uint32_t c = 0; c < lib_category_count(); c++) {
        const lib_category *cat = lib_get_category(c);
        printf("[%s]  #%06x  %u songs\n", lib_string(cat->name), cat->color, cat->song_count);
        for (uint32_t i = 0; i < cat->song_count; i++) {
            const lib_song *s = lib_get_song(cat->first_song + i);
            printf("  %s", lib_string(s->title));
            if (*lib_string(s->subtitle)) {
                printf(" / %s", lib_string(s->subtitle));
            }
            printf("  [");
            bool first = true;
            for (int k = 0; k < LIB_COURSES; k++) {
                if (s->level[k] == LIB_NO_COURSE) {
                    continue;
                }
                printf("%s%s:%u%s", first ? "" : " ", names[k], s->level[k], s->branch_mask >> k & 1 ? "*" : "");
                first = false;
            }
            unsigned secs = s->audio_samples / 48000;
            printf("]  %u:%02u\n", secs / 60, secs % 60);
        }
    }
    return 0;
}
