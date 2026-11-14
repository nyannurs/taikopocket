/* Library file access (docs/library-format.md).
 * The boot region (header, tables, glyph atlas, sounds, sprites) is read into
 * memory once; charts and audio are read on demand by offset. */
#ifndef TP_LIBRARY_H
#define TP_LIBRARY_H

#include <stdbool.h>
#include <stdint.h>

#define LIB_VERSION 1
#define LIB_HEADER_BYTES 512
#define LIB_BOOT_MAX (12u << 20)
#define LIB_NO_COURSE 255
#define LIB_COURSES 5

enum { LIB_SEC_CATEGORIES, LIB_SEC_SONGS, LIB_SEC_STRINGS, LIB_SEC_FONTS, LIB_SEC_SOUNDS,
       LIB_SEC_SPRITES, LIB_SEC_SKINCFG, LIB_SEC_COUNT = 8 };

enum { LIB_AUDIO_NONE = 0, LIB_AUDIO_PCM16 = 1, LIB_AUDIO_IMA_ADPCM = 2 };

typedef struct {
    uint32_t title, subtitle; /* offsets into the string pool */
    uint16_t category;
    uint8_t audio_format; /* LIB_AUDIO_* */
    uint8_t audio_channels;
    uint8_t level[LIB_COURSES]; /* LIB_NO_COURSE when the course does not exist */
    uint8_t branch_mask;        /* bit n: course n has branches */
    uint8_t flags;              /* bit 0: show the whole subtitle */
    uint8_t reserved;
    uint32_t tja_bytes;
    uint64_t tja_offset;
    uint64_t audio_offset;
    uint32_t audio_bytes;
    uint32_t audio_samples;
    float demostart; /* seconds */
    uint32_t chart_crc;
    uint16_t block_bytes, block_samples;
    uint32_t reserved2;
} lib_song;

typedef struct {
    uint32_t name;
    uint32_t first_song, song_count;
    uint32_t color, text_color; /* 0xRRGGBB */
    uint32_t reserved;
} lib_category;

typedef enum { LIB_IDLE, LIB_LOADING, LIB_READY, LIB_ERROR } lib_state;

/* Boot: call lib_boot_begin(), then lib_boot_poll() every frame until it
 * returns LIB_READY or LIB_ERROR. */
void lib_boot_begin(void);
lib_state lib_boot_poll(void);
uint32_t lib_boot_progress(void); /* 0..256 */
const char *lib_error(void);

uint32_t lib_song_count(void);
uint32_t lib_category_count(void);
const lib_song *lib_get_song(uint32_t index);
const lib_category *lib_get_category(uint32_t index);
const char *lib_string(uint32_t offset);
const uint8_t *lib_section(int index, uint32_t *bytes);
uint32_t lib_build_id(void);

/* Start reading `len` bytes at a library offset (handles volumes).
 * Completion is pf_lib_busy() / pf_lib_failed(). */
void lib_read(uint64_t offset, void *dst, uint32_t len);

#endif
