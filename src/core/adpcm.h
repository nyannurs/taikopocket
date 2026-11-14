/* IMA ADPCM, WAV block layout (what `ffmpeg -c:a adpcm_ima_wav` writes).
 * A block is: per channel {s16 predictor, u8 step index, u8 0}, then groups of
 * 4 bytes per channel, 8 samples each, low nibble first. The predictor is the
 * block's first sample, so a block holds 1 + (bytes/channels - 4) * 2 samples
 * and can be decoded on its own. The gateware streamer decodes the same layout. */
#ifndef TP_ADPCM_H
#define TP_ADPCM_H

#include <stdint.h>

static inline int tp_adpcm_block_samples(int block_bytes, int channels) {
    return 1 + (block_bytes / channels - 4) * 2;
}

/* Decodes one block into interleaved s16. Returns samples per channel. */
int tp_adpcm_decode_block(const uint8_t *block, int block_bytes, int channels, int16_t *out);

#endif
