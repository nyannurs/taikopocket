#include "adpcm.h"

static const int8_t index_table[16] = {-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8};

static const int16_t step_table[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
    25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
    88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
    307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
    1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,  3327,
    3660,  4026,  4428,  4871,  5358,  5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767};

int tp_adpcm_decode_block(const uint8_t *block, int block_bytes, int channels, int16_t *out) {
    int samples = tp_adpcm_block_samples(block_bytes, channels);
    for (int ch = 0; ch < channels; ch++) {
        const uint8_t *hdr = block + ch * 4;
        int pred = (int16_t)(hdr[0] | (hdr[1] << 8));
        int index = hdr[2];
        if (index > 88) {
            index = 88;
        }
        int16_t *dst = out + ch;
        *dst = (int16_t)pred;
        dst += channels;
        const uint8_t *src = block + channels * 4 + ch * 4;
        for (int n = 1; n < samples;) {
            for (int b = 0; b < 4; b++) {
                uint8_t byte = src[b];
                for (int half = 0; half < 2; half++, n++) {
                    int nib = half ? byte >> 4 : byte & 15;
                    int step = step_table[index];
                    int diff = step >> 3;
                    if (nib & 4) {
                        diff += step;
                    }
                    if (nib & 2) {
                        diff += step >> 1;
                    }
                    if (nib & 1) {
                        diff += step >> 2;
                    }
                    pred += (nib & 8) ? -diff : diff;
                    if (pred > 32767) {
                        pred = 32767;
                    } else if (pred < -32768) {
                        pred = -32768;
                    }
                    index += index_table[nib];
                    if (index < 0) {
                        index = 0;
                    } else if (index > 88) {
                        index = 88;
                    }
                    *dst = (int16_t)pred;
                    dst += channels;
                }
            }
            src += channels * 4;
        }
    }
    return samples;
}
