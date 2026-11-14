/* Software audio engine: music streamer + hit-sound voices + mixer.
 * It is the reference model for the gateware in gateware/taiko (same behaviour,
 * sample for sample), the audio backend of the SDL build, and the fallback on
 * the Pocket when the stock openfpga-litex audio FIFO is used.
 * Not thread safe: the caller serialises sm_render() against everything else. */
#ifndef TP_SOFTMIX_H
#define TP_SOFTMIX_H

#include "../platform.h"

#define SM_VOICES 4

void sm_init(void *ring, uint32_t ring_capacity);

void sm_music_begin(const pf_music_fmt *fmt, uint32_t ring_bytes, int64_t ring_origin,
                    int64_t start_pos, uint32_t gain);
void sm_music_feed(uint64_t total_written);
void sm_music_finish(void);
void sm_music_run(bool run);
void sm_music_stop(void);
uint64_t sm_music_consumed(void);
bool sm_music_starved(void);
int64_t sm_music_pos(void);
bool sm_music_advancing(void); /* position moves one per output sample right now */

uint32_t sm_hit_room(int slot);
void sm_hit_load(int slot, const int16_t *pcm, uint32_t samples, uint32_t rate);
void sm_hit_play(int slot);
void sm_volume(uint32_t music, uint32_t hits);

int64_t sm_counter(void); /* samples rendered so far */
void sm_render(int16_t *out, int frames);

#endif
