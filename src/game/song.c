/* Loading a song (chart text -> parser -> judge) and streaming its audio.
 *
 * Audio is never copied by the game: the library is read straight into the
 * platform's music ring in chunks, ahead of the play position, and the platform
 * (gateware on the Pocket, softmix on the desktop) plays from the ring. The
 * position it reports is the game clock. */
#include <string.h>

#include "game.h"

/* fixed storage for one chart; see docs/tja-notes.md for what real charts need */
#define MAX_MASTER 16384
#define MAX_BRANCH 32768
#define MAX_EVENTS 4096
#define MAX_SECTIONS TP_JUDGE_MAX_SECTIONS
#define MAX_TEMPO 2048
#define MAX_LINES 16384
#define MAX_TJA_BYTES (256u << 10)

#define CHUNK_BYTES (128u << 10)
#define PREBUFFER_BYTES (192u << 10)
#define PREVIEW_DELAY_FRAMES 30
#define RING_MAX (48u << 20)

static tp_note master[MAX_MASTER], branch[MAX_BRANCH];
static tp_event events[MAX_EVENTS];
static tp_section sections[MAX_SECTIONS];
static tp_tempo_point tempo[MAX_TEMPO];
static tp_str lines[MAX_LINES];
static const tp_note *lane_buf[3 * (MAX_MASTER + MAX_BRANCH)];
static const tp_event *timeline_buf[MAX_EVENTS];
static char tja_text[MAX_TJA_BYTES];

song_t song;
static song_state state = SONG_IDLE;
static const char *error = "";
static enum { L_NONE, L_WAIT_IDLE, L_TJA, L_PREBUFFER } load_step;

/* ---------- audio stream ---------- */

static struct {
    bool active, started, want_run, failed;
    bool held; /* paused before it started: want_run is given back on resume */
    uint64_t blob_offset; /* library offset of the first byte streamed */
    uint32_t blob_bytes;  /* bytes from there to the end of the audio */
    uint32_t done;        /* bytes in the ring so far */
    uint32_t inflight;    /* size of the read in progress */
    uint32_t prebuffer;
    uint8_t *ring;
    uint32_t ring_bytes;
    uint32_t block_bytes;
} st;

bool stream_idle(void) { return !pf_lib_busy(); }

static void stream_close(void) {
    pf_music_stop();
    st.active = false;
    /* a read that is still in flight finishes into the ring, harmlessly */
}

/* Opens the audio of `info` so that playback can start at `start_sample`
 * (negative: that much silence before sample 0). */
static void stream_open(const lib_song *info, int64_t start_sample, uint32_t gain, uint32_t prebuffer) {
    stream_close();
    memset(&st, 0, sizeof st);
    uint32_t capacity;
    st.ring = pf_music_ring(&capacity);
    if (capacity > RING_MAX) {
        capacity = RING_MAX;
    }
    pf_music_fmt fmt = {PF_CODEC_PCM16, 2, 4, 1};
    int64_t origin = 0;
    if (info->audio_format != LIB_AUDIO_NONE && info->block_bytes) {
        fmt.codec = info->audio_format == LIB_AUDIO_IMA_ADPCM ? PF_CODEC_IMA_ADPCM : PF_CODEC_PCM16;
        fmt.channels = info->audio_channels;
        fmt.block_bytes = info->block_bytes;
        fmt.block_samples = info->block_samples;
        int64_t first = start_sample > 0 ? start_sample : 0;
        if (first >= info->audio_samples) {
            first = 0;
        }
        uint64_t block = (uint64_t)first / info->block_samples;
        uint64_t skip = block * info->block_bytes;
        if (skip > info->audio_bytes) {
            skip = 0;
            block = 0;
        }
        origin = (int64_t)(block * info->block_samples);
        st.blob_offset = info->audio_offset + skip;
        st.blob_bytes = info->audio_bytes - (uint32_t)skip;
    }
    st.block_bytes = fmt.block_bytes;
    st.ring_bytes = capacity / fmt.block_bytes * fmt.block_bytes;
    st.prebuffer = prebuffer < st.blob_bytes ? prebuffer : st.blob_bytes;
    st.active = true;
    pf_music_begin(&fmt, st.ring_bytes, origin, start_sample, gain);
    if (st.blob_bytes == 0) {
        pf_music_finish();
    }
}

static bool stream_ready(void) { return st.done >= st.prebuffer || st.failed; }

void stream_poll(void) {
    if (!st.active) {
        return;
    }
    if (st.inflight) {
        if (pf_lib_busy()) {
            return;
        }
        if (pf_lib_failed()) {
            st.failed = true;
            st.blob_bytes = st.done; /* play what we have, then silence */
        } else {
            st.done += st.inflight;
        }
        st.inflight = 0;
        pf_music_feed(st.done);
        if (st.done >= st.blob_bytes) {
            pf_music_finish();
        }
    }
    if (st.want_run && !st.started && stream_ready()) {
        pf_music_run(true);
        st.started = true;
    }
    if (st.done >= st.blob_bytes || pf_lib_busy()) {
        return;
    }
    uint32_t used = st.done - (uint32_t)pf_music_consumed();
    uint32_t space = st.ring_bytes - used;
    uint32_t chunk = st.blob_bytes - st.done;
    uint32_t to_wrap = st.ring_bytes - st.done % st.ring_bytes;
    if (chunk > CHUNK_BYTES) {
        chunk = CHUNK_BYTES;
    }
    if (chunk > to_wrap) {
        chunk = to_wrap;
    }
    if (chunk > space) {
        chunk = space / st.block_bytes * st.block_bytes;
    }
    if (chunk == 0) {
        return;
    }
    st.inflight = chunk;
    lib_read(st.blob_offset + st.done, st.ring + st.done % st.ring_bytes, chunk);
}

/* ---------- song ---------- */

static song_state fail(const char *msg) {
    error = msg;
    state = SONG_ERROR;
    load_step = L_NONE;
    return state;
}

const char *song_error(void) { return error; }

void song_unload(void) {
    stream_close();
    state = SONG_IDLE;
    load_step = L_NONE;
    settings_apply_audio();
}

void song_load(uint32_t index, int course) {
    preview_stop();
    stream_close();
    memset(&song.tja, 0, sizeof song.tja);
    song.index = index;
    song.course = course;
    song.info = lib_get_song(index);
    state = SONG_LOADING;
    load_step = L_WAIT_IDLE;
    error = "";
}

static song_state parse_song(void) {
    const lib_song *info = song.info;
    uint64_t t0 = pf_time_us();
    if (!tja_load(&song.tja, tja_text, info->tja_bytes, lines, MAX_LINES, true)) {
        return fail("This chart has too many lines.");
    }
    tp_chart c = {master, MAX_MASTER, branch, MAX_BRANCH, events, MAX_EVENTS, sections, MAX_SECTIONS, tempo, MAX_TEMPO,
                  0, 0, 0, 0, 0, 0, 0, 0, false, 0};
    song.chart = c;
    song.tja.jpos_field_width = 0;
    if (!tja_chart(&song.tja, song.course, 0, &song.chart) || song.chart.master_count == 0) {
        return fail("This course has no chart.");
    }
    if (song.chart.overflow) {
        return fail("This chart is too large.");
    }
    song.offset_ms = song.tja.offset * 1000.0;
    song.has_audio_end = info->audio_format != LIB_AUDIO_NONE;
    song.audio_end_ms = info->audio_samples * 1000.0 / PF_SAMPLE_RATE + song.offset_ms;
    const double *audio_end = song.has_audio_end ? &song.audio_end_ms : NULL;

    static uint32_t plays;
    tp_modifiers mods = {false, settings.mod_inverse != 0, settings.speed_x10 / 10.0f, settings.mod_invisible != 0,
                         settings.mod_random, (uint32_t)pf_time_us() ^ (++plays * 0x9E3779B9u)};
    tp_chart_prepare(&song.chart, &mods, audio_end);
    /* "#DELAY inf", "BPM:nan": such a note is never reached and the song would never end */
    for (int i = 0; i < song.chart.master_count + song.chart.branch_count; i++) {
        const tp_note *n = i < song.chart.master_count ? &master[i] : &branch[i - song.chart.master_count];
        if (!(n->hit_ms - n->hit_ms == 0) || !(n->bpm - n->bpm == 0)) {
            return fail("This chart has a time or tempo that is not a number.");
        }
    }
    /* YataiDON's gauge table is empty for Easy above level 5, Normal above 7 and Hard
     * above 8: the gauge would not move and the course could not be cleared. Such a
     * course gets the highest row its table has (docs/judge.md). */
    static const int top_level[3] = {5, 7, 8};
    int level = (int)song.tja.course[song.course].level;
    if (song.course < 3 && level > top_level[song.course]) {
        level = top_level[song.course];
    }
    tp_judge_load(&song.judge, &song.chart, song.course, level,
                  settings.auto_play, true, audio_end, lane_buf, 3 * (MAX_MASTER + MAX_BRANCH), timeline_buf,
                  MAX_EVENTS);
    song.parse_us = (uint32_t)(pf_time_us() - t0);
    pf_log("chart: %d notes, %d in branches, parsed in %u us", song.chart.master_count, song.chart.branch_count,
           (unsigned)song.parse_us);

    /* lead-in: at least half a second, and 2.5 s before the first note */
    double first_hit = song.judge.chart_end_ms;
    for (int i = 0; i < song.chart.master_count; i++) {
        if (master[i].type != TP_BARLINE) {
            first_hit = master[i].hit_ms;
            break;
        }
    }
    double start_ms = first_hit - song.offset_ms - 2500.0;
    if (start_ms > -500.0) {
        start_ms = -500.0;
    }
    double vol = song.tja.songvol;
    if (vol < 0) {
        vol = 0;
    } else if (vol > 400) {
        vol = 400;
    }
    stream_open(info, (int64_t)(start_ms * PF_SAMPLE_RATE / 1000.0), (uint32_t)(vol * 256 / 100), PREBUFFER_BYTES);

    double se = song.tja.sevol < 0 ? 0 : song.tja.sevol > 200 ? 200 : song.tja.sevol;
    pf_volume((uint32_t)settings.music_volume * 256 / 100, (uint32_t)(settings.hit_volume * se * 256 / 10000));
    load_step = L_PREBUFFER;
    return state;
}

song_state song_poll(void) {
    stream_poll();
    switch (load_step) {
    case L_WAIT_IDLE:
        if (!pf_lib_busy()) {
            if (song.info->tja_bytes >= MAX_TJA_BYTES) {
                return fail("This chart file is too large.");
            }
            lib_read(song.info->tja_offset, tja_text, song.info->tja_bytes);
            load_step = L_TJA;
        }
        break;
    case L_TJA:
        if (!pf_lib_busy()) {
            if (pf_lib_failed()) {
                return fail("Could not read the chart from the library.");
            }
            return parse_song();
        }
        break;
    case L_PREBUFFER:
        if (stream_ready()) {
            load_step = L_NONE;
            state = SONG_READY;
        }
        break;
    default: break;
    }
    return state;
}

void song_play(void) { st.want_run = true; }

void song_pause(bool paused) {
    if (st.started) {
        pf_music_run(!paused);
    } else if (paused && st.want_run) { /* asked for before the stream was ready: it must not start under the menu */
        st.want_run = false;
        st.held = true;
    } else if (!paused && st.held) {
        st.want_run = true;
        st.held = false;
    }
}

double song_now_ms(void) {
    int64_t counter, pos;
    pf_audio_clock(&counter, &pos);
    return pos * 1000.0 / PF_SAMPLE_RATE + song.offset_ms - settings.audio_offset_ms;
}

double song_event_ms(int64_t event_counter) {
    int64_t counter, pos;
    pf_audio_clock(&counter, &pos);
    pos += event_counter - counter;
    return pos * 1000.0 / PF_SAMPLE_RATE + song.offset_ms - settings.input_offset_ms;
}

bool song_audio_done(void) { return st.done >= st.blob_bytes && pf_music_consumed() >= st.done; }

/* ---------- preview ---------- */

static struct {
    uint32_t index;
    int countdown; /* frames until it starts; 0 = nothing pending */
    bool playing;
} pv;

void preview_request(uint32_t index) {
    if (pv.playing && pv.index == index) {
        return;
    }
    preview_stop();
    pv.index = index;
    pv.countdown = PREVIEW_DELAY_FRAMES;
}

void preview_stop(void) {
    if (pv.playing) {
        stream_close();
    }
    pv.playing = false;
    pv.countdown = 0;
}

void preview_poll(void) {
    stream_poll();
    if (pv.countdown > 0 && --pv.countdown == 0) {
        if (pf_lib_busy()) {
            pv.countdown = 1; /* a read is still finishing: try again next frame */
            return;
        }
        const lib_song *info = lib_get_song(pv.index);
        if (info->audio_format == LIB_AUDIO_NONE) {
            return;
        }
        int64_t start = (int64_t)((double)info->demostart * PF_SAMPLE_RATE);
        if (start < 0 || start >= info->audio_samples) {
            start = 0;
        }
        stream_open(info, start, 256, 96u << 10);
        st.want_run = true;
        pv.playing = true;
    }
}
