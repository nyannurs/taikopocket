/* The software audio engine (the reference model of the gateware): music
 * position, lead-in, ring wrap, starvation, end of stream, hit voices, mixing. */
#include "platform/common/softmix.h"
#include "test.h"

static uint8_t ring[4096];
static int16_t out[8192];

static void put_pcm(uint32_t at_byte, int count, int first) { /* mono s16: sample n has the value first + n */
    for (int i = 0; i < count; i++) {
        int v = first + i;
        ring[(at_byte + (uint32_t)i * 2) % sizeof ring] = (uint8_t)v;
        ring[(at_byte + (uint32_t)i * 2 + 1) % sizeof ring] = (uint8_t)(v >> 8);
    }
}

int main(void) {
    pf_music_fmt mono = {PF_CODEC_PCM16, 1, 2, 1};
    sm_init(ring, sizeof ring);

    /* --- lead-in: 5 samples of silence, then the stream from sample 0 --- */
    put_pcm(0, 100, 1000);
    sm_music_begin(&mono, sizeof ring, 0, -5, 256);
    sm_music_feed(200);
    sm_render(out, 4);
    CHECK_EQ(sm_music_pos(), -5); /* armed, not running */
    CHECK_EQ(sm_counter(), 4);    /* the output counter runs regardless */
    sm_music_run(true);
    sm_render(out, 8);
    CHECK_EQ(out[0], 0);
    CHECK_EQ(out[4 * 2], 0);
    CHECK_EQ(out[5 * 2], 1000); /* song sample 0 */
    CHECK_EQ(out[5 * 2 + 1], 1000); /* mono goes to both channels */
    CHECK_EQ(out[7 * 2], 1002);
    CHECK_EQ(sm_music_pos(), 3);
    CHECK_EQ(sm_music_consumed(), 6); /* three 2-byte frames */

    /* --- starvation: the position waits for data instead of skipping it --- */
    sm_render(out, 200); /* 97 samples are left */
    CHECK_EQ(sm_music_pos(), 100);
    CHECK(sm_music_starved());
    CHECK_EQ(out[96 * 2], 1099);
    CHECK_EQ(out[97 * 2], 0);
    put_pcm(200, 10, 2000);
    sm_music_feed(220);
    sm_render(out, 4);
    CHECK(!sm_music_starved());
    CHECK_EQ(out[0], 2000);
    CHECK_EQ(sm_music_pos(), 104);

    /* --- end of stream: the position keeps counting in silence --- */
    sm_music_finish();
    sm_render(out, 20);
    CHECK_EQ(out[5 * 2], 2009);
    CHECK_EQ(out[6 * 2], 0);
    CHECK_EQ(sm_music_pos(), 124);
    CHECK(!sm_music_starved());

    /* --- pause holds the position --- */
    sm_music_run(false);
    sm_render(out, 50);
    CHECK_EQ(sm_music_pos(), 124);
    CHECK(!sm_music_advancing());

    /* --- the ring wraps: data written past the end continues at the start --- */
    sm_music_begin(&mono, sizeof ring, 0, 0, 256);
    put_pcm(0, 2048, 0);
    sm_music_feed(4096);
    sm_music_run(true);
    sm_render(out, 2048);
    CHECK_EQ(out[2047 * 2], 2047);
    put_pcm(4096, 100, 5000); /* lands at ring offset 0 again */
    sm_music_feed(4096 + 200);
    sm_render(out, 10);
    CHECK_EQ(out[0], 5000);
    CHECK_EQ(out[9 * 2], 5009);

    /* --- starting in the middle (preview): ring byte 0 is song sample 4000, play from 4010 --- */
    sm_music_begin(&mono, sizeof ring, 4000, 4010, 256);
    put_pcm(0, 50, 100);
    sm_music_feed(100);
    sm_music_run(true);
    sm_render(out, 2);
    CHECK_EQ(out[0], 110);
    CHECK_EQ(sm_music_pos(), 4012);

    /* --- gain, music volume, stereo --- */
    pf_music_fmt stereo = {PF_CODEC_PCM16, 2, 4, 1};
    static const uint8_t frame[4] = {0x10, 0x27, 0xF0, 0xD8}; /* L = 10000, R = -10000 */
    for (int i = 0; i < 4; i++) {
        ring[i] = frame[i];
    }
    sm_music_begin(&stereo, sizeof ring, 0, 0, 128); /* song gain 0.5 */
    sm_music_feed(4);
    sm_music_run(true);
    sm_volume(128, 256); /* music volume 0.5 */
    sm_render(out, 1);
    CHECK_EQ(out[0], 2500);
    CHECK_EQ(out[1], -2500);
    sm_music_stop();

    /* --- hit voices: start on the next sample, add up, saturate, restart when all are busy --- */
    static int16_t click[8] = {20000, 20000, 20000, 20000, 20000, 20000, 20000, 20000};
    sm_volume(256, 256);
    sm_hit_load(PF_HIT_DON, click, 8, PF_SAMPLE_RATE);
    sm_hit_play(PF_HIT_DON);
    sm_render(out, 2);
    CHECK_EQ(out[0], 20000); /* audible on the very next sample */
    sm_hit_play(PF_HIT_DON);
    sm_render(out, 2);
    CHECK_EQ(out[0], 32767); /* two voices: 40000, saturated */
    sm_render(out, 8);
    CHECK_EQ(out[7 * 2], 0); /* both finished */
    for (int i = 0; i < SM_VOICES + 1; i++) {
        sm_hit_play(PF_HIT_DON); /* the fifth restarts the oldest voice */
    }
    sm_render(out, 1);
    CHECK_EQ(out[0], 32767);
    sm_render(out, 10);
    sm_volume(256, 128);
    sm_hit_play(PF_HIT_DON);
    sm_render(out, 1);
    CHECK_EQ(out[0], 10000); /* hit volume 0.5 */

    /* a 24 kHz sample plays at half speed: each stored sample covers two output samples */
    static int16_t ramp[4] = {100, 200, 300, 400};
    sm_render(out, 10);
    sm_volume(256, 256);
    sm_hit_load(PF_HIT_KA, ramp, 4, PF_SAMPLE_RATE / 2);
    sm_hit_play(PF_HIT_KA);
    sm_render(out, 8);
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[1 * 2], 100);
    CHECK_EQ(out[2 * 2], 200);
    CHECK_EQ(out[7 * 2], 400);
    TEST_MAIN_END;
}
