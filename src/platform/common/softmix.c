#include "softmix.h"

#include <string.h>

#include "../../core/adpcm.h"

#define SM_BLOCK_MAX_SAMPLES 4096

static struct {
    uint8_t *ring;
    uint32_t ring_capacity;

    pf_music_fmt fmt;
    uint32_t ring_bytes;
    int64_t origin;       /* song sample held at ring byte 0 */
    int64_t pos;          /* song sample that goes out next */
    int64_t stream_pos;   /* song sample the decoder yields next */
    uint64_t written;     /* ring bytes written by the game */
    uint64_t consumed;    /* ring bytes decoded */
    uint32_t gain;
    bool armed, running, finished, starved;

    int16_t block[SM_BLOCK_MAX_SAMPLES * 2];
    int block_len, block_idx;

    int16_t hit_pcm[PF_HIT_SLOTS][PF_HIT_MAX_SAMPLES];
    uint32_t hit_len[PF_HIT_SLOTS];
    uint32_t hit_step[PF_HIT_SLOTS]; /* 16.16 samples per output sample */
    struct {
        int slot;
        uint32_t phase; /* 16.16 */
        bool active;
    } voice[SM_VOICES];
    int next_voice;

    uint32_t vol_music, vol_hits;
    int64_t counter;
} sm;

void sm_init(void *ring, uint32_t ring_capacity) {
    memset(&sm, 0, sizeof sm);
    sm.ring = ring;
    sm.ring_capacity = ring_capacity;
    sm.vol_music = 256;
    sm.vol_hits = 256;
}

void sm_music_begin(const pf_music_fmt *fmt, uint32_t ring_bytes, int64_t ring_origin,
                    int64_t start_pos, uint32_t gain) {
    sm.fmt = *fmt;
    sm.ring_bytes = ring_bytes;
    sm.origin = ring_origin;
    sm.pos = start_pos;
    sm.stream_pos = ring_origin;
    sm.written = 0;
    sm.consumed = 0;
    sm.gain = gain;
    sm.block_len = sm.block_idx = 0;
    sm.finished = sm.starved = sm.running = false;
    sm.armed = true;
}

void sm_music_feed(uint64_t total_written) { sm.written = total_written; }
void sm_music_finish(void) { sm.finished = true; }
void sm_music_run(bool run) { sm.running = run && sm.armed; }
void sm_music_stop(void) { sm.armed = sm.running = false; }
uint64_t sm_music_consumed(void) { return sm.consumed; }
bool sm_music_starved(void) { return sm.starved; }
int64_t sm_music_pos(void) { return sm.pos; }
bool sm_music_advancing(void) { return sm.running && !sm.starved; }

/* the shares of the gateware's 40960-sample memory */
uint32_t sm_hit_room(int slot) {
    static const uint32_t room[PF_HIT_SLOTS] = {16384, 12288, 6144, 6144};
    return slot >= 0 && slot < PF_HIT_SLOTS ? room[slot] : 0;
}

void sm_hit_load(int slot, const int16_t *pcm, uint32_t samples, uint32_t rate) {
    if (slot < 0 || slot >= PF_HIT_SLOTS) {
        return;
    }
    if (samples > sm_hit_room(slot)) {
        samples = sm_hit_room(slot);
    }
    memcpy(sm.hit_pcm[slot], pcm, samples * sizeof(int16_t));
    sm.hit_len[slot] = samples;
    sm.hit_step[slot] = (uint32_t)(((uint64_t)rate << 16) / PF_SAMPLE_RATE);
}

void sm_hit_play(int slot) {
    if (slot < 0 || slot >= PF_HIT_SLOTS || sm.hit_len[slot] == 0) {
        return;
    }
    /* round robin: with every voice busy the oldest one restarts */
    int v = sm.next_voice;
    sm.next_voice = (v + 1) % SM_VOICES;
    sm.voice[v].slot = slot;
    sm.voice[v].phase = 0;
    sm.voice[v].active = true;
}

void sm_volume(uint32_t music, uint32_t hits) {
    sm.vol_music = music;
    sm.vol_hits = hits;
}

int64_t sm_counter(void) { return sm.counter; }

/* Next decoded frame of the stream. false: no data available right now. */
static bool stream_next(int *l, int *r) {
    if (sm.block_idx >= sm.block_len) {
        uint32_t bb = sm.fmt.block_bytes;
        if (sm.consumed + bb > sm.written) {
            return false;
        }
        const uint8_t *src = sm.ring + (uint32_t)(sm.consumed % sm.ring_bytes);
        if (sm.fmt.codec == PF_CODEC_IMA_ADPCM) {
            sm.block_len = tp_adpcm_decode_block(src, bb, sm.fmt.channels, sm.block);
        } else {
            for (int c = 0; c < sm.fmt.channels; c++) {
                sm.block[c] = (int16_t)(src[c * 2] | (src[c * 2 + 1] << 8));
            }
            sm.block_len = 1;
        }
        sm.block_idx = 0;
        sm.consumed += bb;
    }
    const int16_t *s = &sm.block[sm.block_idx * sm.fmt.channels];
    *l = s[0];
    *r = sm.fmt.channels > 1 ? s[1] : s[0];
    sm.block_idx++;
    sm.stream_pos++;
    return true;
}

static void music_frame(int *l, int *r) {
    *l = *r = 0;
    if (!sm.running) {
        return;
    }
    sm.starved = false;
    if (sm.pos < sm.origin) { /* lead-in */
        sm.pos++;
        return;
    }
    int sl = 0, sr = 0;
    while (sm.stream_pos <= sm.pos) {
        if (!stream_next(&sl, &sr)) {
            if (sm.finished) { /* past the end: keep counting, in silence */
                sm.pos++;
            } else {
                sm.starved = true;
            }
            return;
        }
    }
    *l = (sl * (int)sm.gain) >> 8;
    *r = (sr * (int)sm.gain) >> 8;
    sm.pos++;
}

static inline int16_t sat16(int v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

void sm_render(int16_t *out, int frames) {
    for (int i = 0; i < frames; i++) {
        int l, r;
        music_frame(&l, &r);
        int hit = 0;
        for (int v = 0; v < SM_VOICES; v++) {
            if (!sm.voice[v].active) {
                continue;
            }
            int slot = sm.voice[v].slot;
            uint32_t idx = sm.voice[v].phase >> 16;
            if (idx >= sm.hit_len[slot]) {
                sm.voice[v].active = false;
                continue;
            }
            hit += sm.hit_pcm[slot][idx];
            sm.voice[v].phase += sm.hit_step[slot];
        }
        hit = (hit * (int)sm.vol_hits) >> 8;
        out[i * 2] = sat16(((l * (int)sm.vol_music) >> 8) + hit);
        out[i * 2 + 1] = sat16(((r * (int)sm.vol_music) >> 8) + hit);
        sm.counter++;
    }
}
