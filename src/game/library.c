#include "library.h"

#include <string.h>

#include "../platform/platform.h"

_Static_assert(sizeof(lib_song) == 64, "song entry layout");
_Static_assert(sizeof(lib_category) == 24, "category entry layout");

#define BOOT_CHUNK (256u << 10)

typedef struct {
    char magic[8];
    uint32_t version, header_bytes;
    uint64_t total_bytes, volume_bytes;
    uint32_t boot_bytes, song_count, category_count, flags, build_id;
    uint32_t reserved[3];
    struct {
        uint32_t offset, bytes, count, reserved;
    } section[LIB_SEC_COUNT];
} lib_header;

static uint8_t boot[LIB_BOOT_MAX] __attribute__((aligned(16)));
static lib_header hdr;
static lib_state state = LIB_IDLE;
static const char *error = "";
static uint32_t boot_read;      /* bytes of the boot region in memory */
static uint32_t boot_requested; /* end of the read in flight */

static lib_state fail(const char *msg) {
    error = msg;
    state = LIB_ERROR;
    return state;
}

void lib_boot_begin(void) {
    error = "";
    boot_read = 0;
    memset(&hdr, 0, sizeof hdr);
    if (pf_lib_size() < LIB_HEADER_BYTES) {
        fail("No library file. Pack your songs with tools/packer/pack.py.");
        return;
    }
    state = LIB_LOADING;
    boot_requested = LIB_HEADER_BYTES;
    pf_lib_read(0, 0, boot, LIB_HEADER_BYTES);
}

lib_state lib_boot_poll(void) {
    if (state != LIB_LOADING || pf_lib_busy()) {
        return state;
    }
    if (pf_lib_failed()) {
        return fail("Could not read the library file.");
    }
    boot_read = boot_requested;
    if (boot_read == LIB_HEADER_BYTES) {
        memcpy(&hdr, boot, sizeof hdr);
        if (memcmp(hdr.magic, "TAIKOLIB", 8) != 0) {
            return fail("This is not a Taiko Pocket library file.");
        }
        if (hdr.version != LIB_VERSION) {
            return fail("The library was made by a different packer version. Pack it again.");
        }
        if (hdr.boot_bytes > LIB_BOOT_MAX || hdr.boot_bytes < LIB_HEADER_BYTES) {
            return fail("The library's skin and tables are too large for this build.");
        }
    }
    if (boot_read >= hdr.boot_bytes) {
        for (int i = 0; i < LIB_SEC_COUNT; i++) {
            if ((uint64_t)hdr.section[i].offset + hdr.section[i].bytes > hdr.boot_bytes) {
                return fail("The library file is damaged.");
            }
        }
        if (hdr.section[LIB_SEC_SONGS].bytes < (uint64_t)hdr.song_count * sizeof(lib_song) ||
            hdr.section[LIB_SEC_CATEGORIES].bytes < (uint64_t)hdr.category_count * sizeof(lib_category)) {
            return fail("The library file is damaged.");
        }
        state = LIB_READY;
        return state;
    }
    uint32_t n = hdr.boot_bytes - boot_read;
    if (n > BOOT_CHUNK) {
        n = BOOT_CHUNK;
    }
    boot_requested = boot_read + n;
    pf_lib_read(0, boot_read, boot + boot_read, n);
    return state;
}

uint32_t lib_boot_progress(void) {
    if (!hdr.boot_bytes) {
        return 0;
    }
    return (uint32_t)(((uint64_t)boot_read << 8) / hdr.boot_bytes);
}

const char *lib_error(void) { return error; }
uint32_t lib_song_count(void) { return state == LIB_READY ? hdr.song_count : 0; }
uint32_t lib_category_count(void) { return state == LIB_READY ? hdr.category_count : 0; }
uint32_t lib_build_id(void) { return hdr.build_id; }

const uint8_t *lib_section(int index, uint32_t *bytes) {
    if (bytes) {
        *bytes = hdr.section[index].bytes;
    }
    return boot + hdr.section[index].offset;
}

const lib_song *lib_get_song(uint32_t index) {
    return (const lib_song *)(boot + hdr.section[LIB_SEC_SONGS].offset) + index;
}

const lib_category *lib_get_category(uint32_t index) {
    return (const lib_category *)(boot + hdr.section[LIB_SEC_CATEGORIES].offset) + index;
}

const char *lib_string(uint32_t offset) {
    if (offset >= hdr.section[LIB_SEC_STRINGS].bytes) {
        return "";
    }
    return (const char *)boot + hdr.section[LIB_SEC_STRINGS].offset + offset;
}

void lib_read(uint64_t offset, void *dst, uint32_t len) {
    uint32_t volume = 0;
    if (hdr.volume_bytes) {
        volume = (uint32_t)(offset / hdr.volume_bytes);
        offset -= (uint64_t)volume * hdr.volume_bytes;
    }
    pf_lib_read(volume, offset, dst, len);
}
