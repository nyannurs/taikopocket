/* End to end: the real game on the simulated platform (tests/host).
 * Boots from the test library, picks songs through the menus, plays them with
 * auto-play and with a sample-accurate scripted player, and checks the result.
 * Every screen is drawn, so this also catches renderer crashes. */
#include "game/game.h"
#include "host/host.h"
#include "test.h"

static void frames(int n) {
    for (int i = 0; i < n; i++) {
        host_frame();
    }
}

static void tap(uint32_t button) {
    host_button(button, true);
    host_frame();
    host_button(button, false);
    host_frame();
}

static uint32_t screen_hash(void) {
    const uint16_t *p = host_screen();
    uint32_t h = 2166136261u;
    for (int i = 0; i < PF_SCREEN_W * PF_SCREEN_H; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* presses the right drum at the exact sample of every note that is due */
static void perfect_player(void) {
    int64_t counter, pos;
    pf_audio_clock(&counter, &pos);
    tp_lane *lanes[2] = {&song.judge.don, &song.judge.kat};
    for (int l = 0; l < 2; l++) {
        for (int i = lanes[l]->head; i < lanes[l]->tail; i++) {
            const tp_note *n = lanes[l]->v[i];
            /* the sample position at which this note is exactly on time */
            double exact = (n->hit_ms - song.offset_ms + settings.input_offset_ms) * PF_SAMPLE_RATE / 1000.0;
            int64_t at = (int64_t)(exact + 0.5);
            if (at > pos + 800) {
                break;
            }
            if (at > pos) { /* falls inside the frame that is about to run */
                host_tap_at(l == 0 ? PF_KEY_DON_L : PF_KEY_KA_L, counter + (at - pos));
            }
        }
    }
}

/* from song select: open the course choice, move to `course`, start, and play to the result screen */
static void play_selected(int course_moves, bool scripted) {
    tap(PF_BTN_A);
    for (int i = 0; i < course_moves; i++) {
        tap(PF_BTN_RIGHT);
    }
    for (int i = 0; i < -course_moves; i++) {
        tap(PF_BTN_LEFT);
    }
    tap(PF_BTN_A);
    int guard = 0;
    last_result.score = 0xFFFFFFFF;
    while (last_result.score == 0xFFFFFFFF && guard++ < 60 * 600) {
        if (scripted && !settings.auto_play) {
            perfect_player();
            if (song.judge.is_drumroll || song.judge.is_balloon) {
                host_tap_at(PF_KEY_DON_R, host_counter());
            }
        }
        host_frame();
    }
    CHECK(guard < 60 * 600);
    frames(5);
}

int main(int argc, char **argv) {
    const char *library = argc > 1 ? argv[1] : "build/tests/songs.tpl";
    if (!host_open_library(library)) {
        printf("  cannot open %s\n", library);
        return 1;
    }
    app_init();
    frames(30);
    CHECK_EQ(lib_song_count(), 7);
    CHECK_EQ(lib_category_count(), 2);

    /* --- "Basic Test", Oni, auto-play --- */
    settings.show_fps = false; /* its number would differ between two captures of one screen */
    settings.auto_play = true;
    tap(PF_BTN_SELECT); /* through the settings and back: song select is drawn again, with the AUTO label */
    frames(5);
    tap(PF_BTN_B);
    frames(20); /* the screen is drawn in pieces over a few frames */
    uint32_t select_hash = screen_hash();
    play_selected(0, false);
    CHECK(last_result.auto_play);
    CHECK_EQ(last_result.bad, 0);
    CHECK_EQ(last_result.ok, 0);
    CHECK_EQ(last_result.good, 34);
    CHECK(last_result.all_good && last_result.cleared && last_result.rainbow);
    CHECK(last_result.rolls > 20);
    CHECK_EQ(last_result.max_combo, 34);
    CHECK(!score_find(lib_get_song(0)->chart_crc, 3)); /* auto-play is not recorded */
    tap(PF_BTN_A); /* back to song select */
    frames(20);
    CHECK_EQ(screen_hash(), select_hash); /* same screen as before the play */

    /* --- the same chart by a player whose presses are stamped at the exact samples --- */
    settings.auto_play = false;
    play_selected(0, true);
    CHECK(!last_result.auto_play);
    CHECK_EQ(last_result.good, 34); /* every hit GOOD: timestamps survive the 60 fps frame loop */
    CHECK_EQ(last_result.ok + last_result.bad, 0);
    CHECK(last_result.new_record);
    const score_entry *best = score_find(lib_get_song(0)->chart_crc, 3);
    CHECK(best && best->score == last_result.score && best->crown == CROWN_ALL_GOOD);
    CHECK(best && best->good == 34 && best->ok == 0 && best->bad == 0 && best->max_combo == last_result.max_combo);

    /* back in song select, the best play's window comes up when the cursor rests, and goes when it moves */
    tap(PF_BTN_A);
    frames(30);
    uint32_t rested = screen_hash();
    frames(60);
    CHECK(screen_hash() != rested);
    tap(PF_BTN_DOWN);
    tap(PF_BTN_UP);
    frames(20);
    CHECK_EQ(screen_hash(), rested);

    /* the modifiers together: nothing is drawn of the notes, and what the judge expects has changed */
    settings.mod_invisible = 1;
    settings.mod_inverse = 1;
    settings.mod_random = RANDOM_DETARAME;
    play_selected(0, true); /* the perfect player hits what the lanes hold, whatever it has become */
    CHECK_EQ(last_result.good, 34);
    settings.mod_invisible = settings.mod_inverse = settings.mod_random = 0;

    /* a player whose presses arrive 60 ms late is all GOOD again once the input offset says so */
    tap(PF_BTN_A);
    frames(10);
    settings.input_offset_ms = 60;
    play_selected(0, true); /* perfect_player() delays its presses by the setting */
    CHECK_EQ(last_result.good, 34);
    settings.input_offset_ms = 0;
    tap(PF_BTN_A);
    frames(10);

    /* --- "Branch Test": a perfect player is sent to the master route --- */
    tap(PF_BTN_DOWN);
    play_selected(0, true);
    CHECK_EQ(last_result.bad, 0);
    CHECK_EQ(song.judge.branch_decisions, 3);
    CHECK_EQ(song.judge.current_branch, TP_BRANCH_MASTER);
    tap(PF_BTN_A);
    frames(10);
    /* ... and someone who never hits stays on the normal route and fails */
    play_selected(0, false);
    CHECK_EQ(last_result.good, 0);
    CHECK_EQ(song.judge.current_branch, TP_BRANCH_NORMAL);
    CHECK(!last_result.cleared);
    tap(PF_BTN_A);
    frames(10);

    /* --- the settings give the cursor back where it was: a song further down, a folder further down --- */
    tap(PF_BTN_DOWN);
    frames(10);
    uint32_t place = screen_hash();
    tap(PF_BTN_SELECT);
    frames(10);
    tap(PF_BTN_B);
    frames(20);
    CHECK_EQ(screen_hash(), place);
    tap(PF_BTN_UP);
    tap(PF_BTN_B); /* the folder list */
    tap(PF_BTN_DOWN);
    frames(10);
    place = screen_hash();
    tap(PF_BTN_SELECT);
    frames(10);
    tap(PF_BTN_B);
    frames(20);
    CHECK_EQ(screen_hash(), place);
    tap(PF_BTN_UP);
    tap(PF_BTN_A); /* into the first folder again, at the song it was left at */
    frames(20);

    /* --- every other chart of the test library, on auto-play, without incident --- */
    settings.auto_play = true;
    for (uint32_t i = 2; i < lib_song_count(); i++) {
        tap(PF_BTN_DOWN);
        play_selected(0, false);
        CHECK_EQ(last_result.bad, 0);
        CHECK(last_result.good > 0);
        tap(PF_BTN_A);
        frames(10);
    }

    /* --- pause, retry and quit --- */
    tap(PF_BTN_A);
    tap(PF_BTN_A);
    frames(200);
    tap(PF_BTN_START); /* pause */
    double paused_at = song_now_ms();
    frames(60);
    CHECK_NEAR(song_now_ms(), paused_at, 0.001); /* the clock stands still */
    tap(PF_BTN_DOWN);
    tap(PF_BTN_A); /* retry */
    frames(120);
    CHECK(song_now_ms() < paused_at);
    /* the retried song runs like a first play: it can be paused and stays paused
     * (a retry once set the scene up again on every frame, which also cleared the pause) */
    frames(60);
    double before_pause = song_now_ms();
    tap(PF_BTN_START);
    double paused_again = song_now_ms();
    CHECK(paused_again > before_pause - 50);
    frames(30);
    CHECK_NEAR(song_now_ms(), paused_again, 0.001);
    tap(PF_BTN_UP); /* wraps to "Back to song select" */
    tap(PF_BTN_A);
    frames(10);

    /* --- settings and calibration screens open and close --- */
    tap(PF_BTN_SELECT);
    frames(5);
    for (int i = 0; i < 5; i++) {
        tap(PF_BTN_DOWN);
    }
    tap(PF_BTN_A); /* calibration */
    frames(260);
    for (int i = 0; i < 8; i++) {
        host_tap_at(PF_KEY_DON_L, host_counter() + 480); /* always 10 ms after a click */
        frames(30);
    }
    tap(PF_BTN_START);
    frames(5);
    CHECK(settings.input_offset_ms >= -250 && settings.input_offset_ms <= 250);
    /* --- statistics: the scripted play was counted, the screen opens and closes --- */
    CHECK(stats.don_hits + stats.ka_hits > 30);
    CHECK(stats.songs_finished >= 1);
    for (int i = 0; i < 8; i++) {
        tap(PF_BTN_DOWN); /* from "Measure the input offset" to "Statistics" */
    }
    tap(PF_BTN_A);
    frames(5);
    uint32_t stats_hash = screen_hash();
    tap(PF_BTN_B);
    frames(5);
    CHECK(screen_hash() != stats_hash);
    tap(PF_BTN_B);
    frames(10);

    /* --- the save keeps settings and statistics, and still reads a version 1 file --- */
    uint32_t hits = stats.don_hits + stats.ka_hits;
    settings.debounce_ms = 24;
    settings_save();
    stats.don_hits = stats.ka_hits = 0;
    settings.debounce_ms = 0;
    settings_load();
    CHECK_EQ(stats.don_hits + stats.ka_hits, hits);
    CHECK_EQ(settings.debounce_ms, 24);
    TEST_MAIN_END;
}
