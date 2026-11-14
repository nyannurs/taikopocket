/* Analogue Pocket platform: LiteX SoC registers plus the Taiko gateware.
 *
 * Music is streamed by gateware from a ring in SDRAM, hit sounds are played by
 * gateware straight from button edges, and both the song position and every
 * button edge are counted in output samples by hardware. This file only moves
 * settings and bytes around. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../platform.h"
#include "taiko_regs.h"


static uint32_t ctrl; /* shadow of TK_CTRL */
static int fb_index;
static bool read_busy, read_failed;

/* ---------- video ---------- */
static void video_init(void) {
    /* 400 x 360 at 60 Hz with an 11.424 MHz pixel clock: 476 x 400 total */
    VTG_ENABLE = 0;
    FB_DMA_ENABLE = 0;
    VTG_HRES = PF_SCREEN_W;
    VTG_HSYNC_START = PF_SCREEN_W + 8;
    VTG_HSYNC_END = PF_SCREEN_W + 8 + 32;
    VTG_HSCAN = 476 - 1;
    VTG_VRES = PF_SCREEN_H;
    VTG_VSYNC_START = PF_SCREEN_H + 1;
    VTG_VSYNC_END = PF_SCREEN_H + 1 + 8;
    VTG_VSCAN = 400 - 1;
    memset((void *)MEM_FRAMEBUFFER0, 0, PF_SCREEN_W * PF_SCREEN_H * 2);
    memset((void *)MEM_FRAMEBUFFER1, 0, PF_SCREEN_W * PF_SCREEN_H * 2);
    FB_DMA_BASE = MEM_FRAMEBUFFER0;
    FB_DMA_LENGTH = PF_SCREEN_W * PF_SCREEN_H * 2;
    FB_DMA_LOOP = 1;
    FB_DMA_ENABLE = 1;
    VTG_ENABLE = 1;
    ctrl |= TK_CTRL_VIDEO_ON; /* both buffers are black and the timing is ours: show the picture */
    TAIKO(TK_CTRL) = ctrl;
    fb_index = 1; /* buffer 0 is on screen, draw into 1 */
}

uint16_t *pf_video_buffer(void) { return (uint16_t *)(fb_index ? MEM_FRAMEBUFFER1 : MEM_FRAMEBUFFER0); }

/* ---------- sprite layer ---------- */
static uint32_t ovl_ctrl;    /* what TK_OVL_CTRL holds */
static uint32_t ovl_pending; /* what it should hold from the next frame on */

/* Which sprite layer the bitstream has. One from before the layer ignores its registers
 * and reads them as 0: the game then draws the lane itself. The first layer (version 1)
 * has a 65536 pixel sheet and no pixel doubling; later ones have their version in the
 * top byte. */
static int ovl_version(void) {
    static int known = -1;
    if (known < 0) {
        TAIKO(TK_OVL_CTRL) = 0xA500; /* a count, layer off */
        uint32_t back = TAIKO(TK_OVL_CTRL);
        known = (back & 0xFFFF) != 0xA500 ? 0 : (back >> 24) >= 2 ? (int)(back >> 24) : 1;
        TAIKO(TK_OVL_CTRL) = 0;
    }
    return known;
}

bool pf_ovl_supported(void) { return ovl_version() > 0; }
int pf_ovl_version(void) { return ovl_version(); }
uint32_t pf_ovl_capacity(void) { return ovl_version() >= 2 ? PF_OVL_SHEET : 65536; }
bool pf_ovl_doubling(void) { return ovl_version() >= 2; }

void pf_ovl_sheet(uint32_t addr, const uint8_t *pixels, uint32_t count) {
    TAIKO(TK_OVL_ADDR) = addr & 0xFFFF;
    for (uint32_t i = 0; i < count; i++) {
        TAIKO(TK_OVL_DATA) = pixels[i];
    }
}

void pf_ovl_palette(int bank, int index, uint32_t entry) {
    TAIKO(TK_OVL_ADDR) = 1u << 16 | (uint32_t)(bank * 32 + index);
    TAIKO(TK_OVL_DATA) = entry;
}

void pf_ovl_cell(int cell, uint32_t addr, int w, int h, bool clip, bool twice) {
    TAIKO(TK_OVL_ADDR) = 2u << 16 | (uint32_t)cell;
    if (ovl_version() >= 2) { /* addresses count pairs of pixels, bit 31 doubles */
        TAIKO(TK_OVL_DATA) = (uint32_t)twice << 31 | (uint32_t)clip << 30 | (uint32_t)(h & 127) << 23 |
                             (uint32_t)(w & 127) << 16 | (addr >> 1 & 0xFFFF);
    } else {
        TAIKO(TK_OVL_DATA) = (uint32_t)clip << 30 | (uint32_t)(h & 127) << 23 | (uint32_t)(w & 127) << 16 | (addr & 0xFFFF);
    }
}

void pf_ovl_clip(int x0, int x1) {
    x0 = x0 < 0 ? 0 : x0 > 1023 ? 1023 : x0;
    x1 = x1 < 0 ? 0 : x1 > 1023 ? 1023 : x1;
    TAIKO(TK_OVL_CLIP) = (uint32_t)x1 << 16 | (uint32_t)x0;
}

/* The list goes into the half of the sprite memory that is not on screen; present()
 * switches to it together with the frame buffer. */
void pf_ovl_show(const uint32_t *sprites, int count) {
    if (count <= 0) {
        ovl_pending = 0;
        return;
    }
    if (count > PF_OVL_SPRITES) {
        count = PF_OVL_SPRITES;
    }
    uint32_t page = (ovl_ctrl >> 1 & 1) ^ 1;
    TAIKO(TK_OVL_ADDR) = 3u << 16 | page * PF_OVL_SPRITES;
    for (int i = 0; i < count; i++) {
        TAIKO(TK_OVL_DATA) = sprites[i];
    }
    ovl_pending = (uint32_t)count << 8 | page << 1 | 1;
}

/* show the buffer just drawn, at the next vertical sync */
static void present(void) {
    uint32_t frame = TAIKO(TK_VIDEO) >> 1;
    /* a frame is 16.7 ms; without a picture signal the game goes on unsynchronised rather than stand still */
    for (uint64_t t0 = pf_time_us(); (TAIKO(TK_VIDEO) >> 1) == frame && pf_time_us() - t0 < 100000;) {
    }
    FB_DMA_BASE = fb_index ? MEM_FRAMEBUFFER1 : MEM_FRAMEBUFFER0;
    if (ovl_pending != ovl_ctrl) {
        ovl_ctrl = ovl_pending;
        TAIKO(TK_OVL_CTRL) = ovl_ctrl;
    }
    fb_index ^= 1;
}

/* ---------- input ---------- */
uint32_t pf_buttons(void) { return TAIKO(TK_KEYS) & 0xFFFF; }

bool pf_input_next(pf_input_event *out) {
    uint32_t info = TAIKO(TK_EVENT);
    if (!(info & 0x80000000u)) {
        return false;
    }
    uint32_t time = TAIKO(TK_EVENT_TIME); /* removes the event */
    int64_t counter;
    pf_audio_clock(&counter, NULL);
    /* the event's 32-bit timestamp is at or shortly before the current counter */
    int64_t full = (counter & ~0xFFFFFFFFll) | time;
    if (full > counter + 0x40000000ll) {
        full -= 0x100000000ll;
    }
    out->button = 1u << (info & 15);
    out->pressed = (info >> 4) & 1;
    out->counter = full;
    return true;
}

/* ---------- library ---------- */
uint64_t pf_lib_size(void) {
    BRIDGE_SLOT_ID = 1;
    for (int i = 0; i < 64; i++) { /* the size follows the slot id through two clock domains */
        (void)BRIDGE_SLOT_ID;
    }
    return BRIDGE_FILE_SIZE;
}

void pf_lib_read(uint32_t volume, uint64_t offset, void *dst, uint32_t len) {
    (void)BRIDGE_STATUS; /* clear a stale "finished" */
    TAIKO(TK_BRIDGE_OFFSET_HI) = (uint32_t)(offset >> 32);
    BRIDGE_DATA_OFFSET = (uint32_t)offset;
    BRIDGE_TRANSFER_LENGTH = len;
    BRIDGE_RAM_DATA_ADDRESS = (uint32_t)(uintptr_t)dst;
    BRIDGE_SLOT_ID = 1 + volume;
    read_busy = true;
    read_failed = false;
    BRIDGE_REQUEST_READ = 1;
}

/* The bridge writes file data into SDRAM without the CPU seeing it, so lines of
 * the destination that sit in the data cache are stale afterwards. This is the
 * VexRiscv "flush data cache" instruction (LiteX: flush_cpu_dcache). */
static inline void flush_dcache(void) { __asm__ volatile(".word 0x500F"); }

bool pf_lib_busy(void) {
    if (read_busy && BRIDGE_STATUS == 1) {
        read_busy = false;
        read_failed = (BRIDGE_RESULT_CODE & 7) != 0;
        flush_dcache();
    }
    return read_busy;
}
bool pf_lib_failed(void) { return read_failed; }

/* ---------- audio ---------- */
void *pf_music_ring(uint32_t *capacity_bytes) {
    *capacity_bytes = MEM_MUSIC_RING_BYTES;
    return (void *)MEM_MUSIC_RING;
}

void pf_music_begin(const pf_music_fmt *fmt, uint32_t ring_bytes, int64_t ring_origin, int64_t start_pos,
                    uint32_t gain) {
    ctrl &= TK_CTRL_HIT_AUTO | TK_CTRL_VIDEO_ON;
    TAIKO(TK_CTRL) = ctrl; /* stop: nothing reads the FIFO or the ring now */
    AUDIO_BUFFER_FLUSH = 1;
    TAIKO(TK_FORMAT) = (fmt->codec == PF_CODEC_IMA_ADPCM ? 1u : 0u) | (fmt->channels == 2 ? 2u : 0u) |
                       ((uint32_t)(fmt->block_bytes / 4) << 16);
    TAIKO(TK_RING_BASE) = MEM_MUSIC_RING;
    TAIKO(TK_RING_BYTES) = ring_bytes;
    TAIKO(TK_GAIN) = gain > 0xFFFF ? 0xFFFF : gain;
    TAIKO(TK_START_POS_LO) = (uint32_t)start_pos;
    TAIKO(TK_START_POS_HI) = (uint32_t)((uint64_t)start_pos >> 32) & 0xFFFF;
    TAIKO(TK_LEAD_IN) = start_pos < ring_origin ? (uint32_t)(ring_origin - start_pos) : 0;
    TAIKO(TK_SKIP) = start_pos > ring_origin ? (uint32_t)(start_pos - ring_origin) : 0;
    TAIKO(TK_FED) = 0;
    TAIKO(TK_CMD) = TK_CMD_BEGIN;
    ctrl |= TK_CTRL_STREAM;
    TAIKO(TK_CTRL) = ctrl;
}

void pf_music_feed(uint64_t total_written) { TAIKO(TK_FED) = (uint32_t)total_written; }

void pf_music_finish(void) {
    ctrl |= TK_CTRL_FINISHED;
    TAIKO(TK_CTRL) = ctrl;
}

void pf_music_run(bool run) {
    ctrl = run ? ctrl | TK_CTRL_RUN : ctrl & ~TK_CTRL_RUN;
    TAIKO(TK_CTRL) = ctrl;
}

void pf_music_stop(void) {
    ctrl &= TK_CTRL_HIT_AUTO | TK_CTRL_VIDEO_ON;
    TAIKO(TK_CTRL) = ctrl;
    AUDIO_BUFFER_FLUSH = 1;
}

uint64_t pf_music_consumed(void) { return TAIKO(TK_CONSUMED); }

void pf_audio_clock(int64_t *counter, int64_t *music_pos) {
    TAIKO(TK_CMD) = TK_CMD_LATCH;
    if (counter) {
        *counter = (int64_t)(((uint64_t)TAIKO(TK_COUNTER_HI) << 32) | TAIKO(TK_COUNTER_LO));
    }
    if (music_pos) {
        *music_pos = (int64_t)(((uint64_t)TAIKO(TK_POS_HI) << 32) | TAIKO(TK_POS_LO));
    }
}

void pf_hit_load(int slot, const int16_t *pcm, uint32_t samples, uint32_t rate) {
    if (slot < 0 || slot >= PF_HIT_SLOTS) {
        return;
    }
    if (samples > PF_HIT_MAX_SAMPLES) {
        samples = PF_HIT_MAX_SAMPLES;
    }
    TAIKO(slot ? TK_HIT_LEN1 : TK_HIT_LEN0) = 0;
    TAIKO(TK_HIT_ADDR) = (uint32_t)slot << 14;
    for (uint32_t i = 0; i < samples; i++) {
        TAIKO(TK_HIT_DATA) = (uint16_t)pcm[i];
    }
    TAIKO(slot ? TK_HIT_STEP1 : TK_HIT_STEP0) = (uint32_t)(((uint64_t)rate << 16) / PF_SAMPLE_RATE);
    TAIKO(slot ? TK_HIT_LEN1 : TK_HIT_LEN0) = samples;
}

void pf_hit_play(int slot) { TAIKO(TK_CMD) = slot == PF_HIT_KA ? TK_CMD_KA : TK_CMD_DON; }

void pf_hit_map(uint32_t don_buttons, uint32_t ka_buttons) {
    TAIKO(TK_DON_MASK) = don_buttons & 0xFFFF;
    TAIKO(TK_KA_MASK) = ka_buttons & 0xFFFF;
}

void pf_hit_auto(bool enable) {
    ctrl = enable ? ctrl | TK_CTRL_HIT_AUTO : ctrl & ~TK_CTRL_HIT_AUTO;
    TAIKO(TK_CTRL) = ctrl;
}

/* measured: about 0.7 us per blended pixel, so this is some 5 ms of a 16.7 ms frame */
uint32_t pf_blend_budget(void) { return 8000; }

void pf_debounce(uint32_t ms) {
    uint32_t samples = ms * (PF_SAMPLE_RATE / 1000);
    TAIKO(TK_DEBOUNCE) = samples > 4095 ? 4095 : samples;
}

void pf_volume(uint32_t music, uint32_t hits) {
    TAIKO(TK_VOL_MUSIC) = music > 0xFFFF ? 0xFFFF : music;
    TAIKO(TK_VOL_HITS) = hits > 0xFFFF ? 0xFFFF : hits;
}

/* ---------- persistence ----------
 * The save is a file of its own in data slot 9, read and written through the
 * bridge like the library. If it is missing or too short the core asks the
 * Pocket to create or resize it (bridge command 0x0192). Anything that fails
 * turns saving off until the next start instead of hanging the game. */
#define SAVE_SLOT 9
#define BRIDGE_TIMEOUT_US 3000000u

static bool save_ok = true;
static struct {
    char path[256];
    uint32_t flags; /* bit 0: create if missing, bit 1: resize to `size` */
    uint32_t size;
} open_request = {"/Assets/taikopocket/common/taikopocket.sav", 0, 0};

/* waits for the bridge to finish; false on a timeout or an error code above `max_code` */
static bool bridge_finish(uint32_t max_code) {
    uint64_t t0 = pf_time_us();
    while (BRIDGE_STATUS != 1) {
        if (pf_time_us() - t0 > BRIDGE_TIMEOUT_US) {
            return false;
        }
    }
    return (BRIDGE_RESULT_CODE & 7) <= max_code;
}

/* a library read may be in flight (song preview): let it finish first */
static void bridge_idle(void) {
    uint64_t t0 = pf_time_us();
    while (pf_lib_busy() && pf_time_us() - t0 < BRIDGE_TIMEOUT_US) {
    }
    (void)BRIDGE_STATUS;
}

static uint32_t save_file_size(void) {
    BRIDGE_SLOT_ID = SAVE_SLOT;
    for (int i = 0; i < 64; i++) { /* the size follows the slot id through two clock domains */
        (void)BRIDGE_SLOT_ID;
    }
    return BRIDGE_FILE_SIZE;
}

static uint32_t swap32(uint32_t v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }

/* creates the save file, or grows it to `len` bytes */
static bool save_file_prepare(uint32_t len) {
    for (int attempt = 0; attempt < 2; attempt++) {
        /* the Pocket reads this structure out of memory; if it rejects the two numbers as
         * written, try them the other way round */
        open_request.flags = attempt ? swap32(3) : 3;
        open_request.size = attempt ? swap32(len) : len;
        BRIDGE_SLOT_ID = SAVE_SLOT;
        BRIDGE_RAM_DATA_ADDRESS = (uint32_t)(uintptr_t)&open_request;
        BRIDGE_REQUEST_OPENFILE = 1;
        if (bridge_finish(1)) { /* 0: opened, 1: created */
            BRIDGE_SLOT_ID = SAVE_SLOT;
            BRIDGE_FILE_SIZE = len; /* the core's own record of the slot's size */
            return true;
        }
    }
    return false;
}

bool pf_save_supported(void) { return save_ok; }

bool pf_save_read(void *dst, uint32_t len) {
    if (!save_ok) {
        return false;
    }
    bridge_idle();
    if (save_file_size() < len) {
        return false; /* no save yet, or an older and shorter one */
    }
    TAIKO(TK_BRIDGE_OFFSET_HI) = 0;
    BRIDGE_SLOT_ID = SAVE_SLOT;
    BRIDGE_DATA_OFFSET = 0;
    BRIDGE_TRANSFER_LENGTH = len;
    BRIDGE_RAM_DATA_ADDRESS = (uint32_t)(uintptr_t)dst;
    BRIDGE_REQUEST_READ = 1;
    bool ok = bridge_finish(0);
    flush_dcache();
    return ok;
}

bool pf_save_write(const void *src, uint32_t len) {
    if (!save_ok) {
        return false;
    }
    bridge_idle();
    if (save_file_size() < len && !save_file_prepare(len)) {
        save_ok = false;
        pf_log("save: the file could not be created");
        return false;
    }
    TAIKO(TK_BRIDGE_OFFSET_HI) = 0;
    BRIDGE_SLOT_ID = SAVE_SLOT;
    BRIDGE_DATA_OFFSET = 0;
    BRIDGE_TRANSFER_LENGTH = len;
    BRIDGE_RAM_DATA_ADDRESS = (uint32_t)(uintptr_t)src;
    BRIDGE_REQUEST_WRITE = 1;
    if (!bridge_finish(0)) {
        save_ok = false;
        pf_log("save: writing failed");
        return false;
    }
    return true;
}

/* ---------- misc ---------- */
/* Microseconds since the first call. The up-time counter runs at 57.12 MHz; only its
 * low word is read and the time is added up in 32-bit steps, because a 64-bit division
 * is a software loop here and this is called many times per frame. */
uint64_t pf_time_us(void) {
    static uint32_t last, rem;
    static uint64_t us;
    static bool started;
    TIMER_UPTIME_LATCH = 1;
    uint32_t t = TIMER_UPTIME_LO;
    if (!started) {
        started = true;
        last = t;
    }
    uint32_t d = t - last; /* cycles since the last call; a call at least every 75 s */
    last = t;
    /* 57.12 cycles per microsecond = 1428 / 25 */
    uint32_t whole = d / 1428, part = d % 1428 * 25 + rem;
    us += (uint64_t)whole * 25 + part / 1428;
    rem = part % 1428;
    return us;
}

void pf_log(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) {
        n = sizeof buf - 1;
    }
    buf[n++] = '\n';
    for (int i = 0; i < n; i++) {
        /* drop output rather than stall when no terminal is draining the UART */
        for (int wait = 0; UART_TXFULL && wait < 2000; wait++) {
        }
        if (!UART_TXFULL) {
            UART_RXTX = (uint8_t)buf[i];
        }
    }
}

const char *pf_option(const char *name) {
    (void)name;
    return NULL;
}

int main(void) {
    video_init();
    app_init();
    for (;;) {
        app_frame();
        present();
    }
}
