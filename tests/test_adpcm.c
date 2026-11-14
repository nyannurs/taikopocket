/* IMA ADPCM decoder: block geometry and a hand-checked block. */
#include "core/adpcm.h"
#include "test.h"

int main(void) {
    CHECK_EQ(tp_adpcm_block_samples(1024, 1), 2041);
    CHECK_EQ(tp_adpcm_block_samples(2048, 2), 2041);

    /* mono, 12 bytes: header (predictor 100, index 0) + 2 groups of 8 nibbles */
    uint8_t block[12] = {100, 0, 0, 0, 0x07, 0x8F, 0x00, 0x00, 0, 0, 0, 0};
    int16_t out[17];
    CHECK_EQ(tp_adpcm_decode_block(block, 12, 1, out), 17);
    CHECK_EQ(out[0], 100);
    /* nibble 7, step 7: diff = 0 + 7 + 3 + 1 = 11 -> 111, index 0+8 = 8 */
    CHECK_EQ(out[1], 111);
    /* nibble 0, step 16: diff = 2 -> 113, index 7 */
    CHECK_EQ(out[2], 113);
    /* nibble F, step 14: diff = 1 + 14 + 7 + 3 = 25, negative -> 88, index 15 */
    CHECK_EQ(out[3], 88);
    /* nibble 8, step 31: diff = 3, negative -> 85, index 14 */
    CHECK_EQ(out[4], 85);

    /* stereo: channels decode independently */
    uint8_t st[16] = {10, 0, 0, 0, 0xF6, 0xFF, 0, 0, 0x07, 0, 0, 0, 0x0F, 0, 0, 0};
    int16_t so[18];
    CHECK_EQ(tp_adpcm_decode_block(st, 16, 2, so), 9);
    CHECK_EQ(so[0], 10);
    CHECK_EQ(so[1], -10);
    CHECK_EQ(so[2], 21);
    CHECK_EQ(so[3], -21);
    TEST_MAIN_END;
}
