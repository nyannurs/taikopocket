/* Headless, deterministic platform: the library comes from a file, audio goes
 * through the shared software mixer into nothing, and the clock is the mixer's
 * sample counter. See host.h. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "platform/common/softmix.h"
#include "platform/common/softovl.h"
#include "platform/platform.h"

#define SAMPLES_PER_FRAME (PF_SAMPLE_RATE / 60)
#define RING_BYTES (32u << 20)

static FILE *files[10];
static char base[1024];
static bool failed;
static uint32_t done;
static uint16_t fb[2][PF_SCREEN_W * PF_SCREEN_H];
static uint16_t shown[PF_SCREEN_W * PF_SCREEN_H]; /* the frame as displayed: frame buffer plus sprites */
static uint32_t ovl_pending[PF_OVL_SPRITES];
static int ovl_pending_count;
static int fb_index;
static uint8_t *ring;
static uint32_t held;
static pf_input_event fifo[256];
static unsigned fifo_head, fifo_tail;
static uint32_t mask_don, mask_ka;
static bool hit_auto;
static const char *options[16];
static int option_count;
static uint8_t save[1 << 16];
static bool save_valid;
static uint64_t fake_us;

bool host_open_library(const char *path) {
    snprintf(base, sizeof base, "%s", path);
    files[0] = fopen(path, "rb");
    if (!ring) {
        ring = malloc(RING_BYTES);
        sm_init(ring, RING_BYTES);
    }
    return files[0] != NULL;
}

void host_option(const char *name_value) {
    if (option_count < 16) {
        options[option_count++] = name_value;
    }
}

void host_frame(void) {
    static int16_t audio[SAMPLES_PER_FRAME * 2];
    app_frame();
    memcpy(shown, fb[fb_index], sizeof shown);
    ovl_sprites(ovl_pending, ovl_pending_count);
    ovl_compose(shown);
    fb_index ^= 1;
    sm_render(audio, SAMPLES_PER_FRAME);
}

static void push(uint32_t button, bool down, int64_t counter) {
    unsigned next = (fifo_head + 1) % 256;
    if (next == fifo_tail) {
        return;
    }
    if (down && hit_auto) {
        if (button & mask_don) {
            sm_hit_play(PF_HIT_DON);
        } else if (button & mask_ka) {
            sm_hit_play(PF_HIT_KA);
        }
    }
    fifo[fifo_head].button = button;
    fifo[fifo_head].pressed = down;
    fifo[fifo_head].counter = counter;
    fifo_head = next;
}

void host_button(uint32_t button, bool down) {
    held = down ? held | button : held & ~button;
    push(button, down, sm_counter());
}

void host_tap_at(uint32_t button, int64_t counter) {
    push(button, true, counter);
    push(button, false, counter);
}

const uint16_t *host_screen(void) { return shown; }
int64_t host_counter(void) { return sm_counter(); }

/* ---- platform interface ---- */
uint16_t *pf_video_buffer(void) { return fb[fb_index]; }
uint32_t pf_buttons(void) { return held; }
bool pf_input_next(pf_input_event *out) {
    if (fifo_head == fifo_tail) {
        return false;
    }
    *out = fifo[fifo_tail];
    fifo_tail = (fifo_tail + 1) % 256;
    return true;
}

uint64_t pf_lib_size(void) {
    if (!files[0]) {
        return 0;
    }
    fseek(files[0], 0, SEEK_END);
    return (uint64_t)ftell(files[0]);
}

void pf_lib_read(uint32_t volume, uint64_t offset, void *dst, uint32_t len) {
    failed = true;
    done = 0;
    if (volume >= 10) {
        return;
    }
    if (!files[volume]) {
        char path[1024];
        snprintf(path, sizeof path, "%s", base);
        path[strlen(path) - 1] = (char)('0' + volume);
        files[volume] = fopen(path, "rb");
    }
    if (!files[volume] || fseek(files[volume], (long)offset, SEEK_SET)) {
        return;
    }
    done = (uint32_t)fread(dst, 1, len, files[volume]);
    failed = done != len;
}
bool pf_lib_busy(void) { return false; }
bool pf_lib_failed(void) { return failed; }

void *pf_music_ring(uint32_t *capacity_bytes) {
    *capacity_bytes = RING_BYTES;
    return ring;
}
void pf_music_begin(const pf_music_fmt *fmt, uint32_t ring_bytes, int64_t ring_origin, int64_t start_pos,
                    uint32_t gain) {
    sm_music_begin(fmt, ring_bytes, ring_origin, start_pos, gain);
}
void pf_music_feed(uint64_t total_written) { sm_music_feed(total_written); }
void pf_music_finish(void) { sm_music_finish(); }
void pf_music_run(bool run) { sm_music_run(run); }
void pf_music_stop(void) { sm_music_stop(); }
uint64_t pf_music_consumed(void) { return sm_music_consumed(); }
void pf_audio_clock(int64_t *counter, int64_t *music_pos) {
    if (counter) {
        *counter = sm_counter();
    }
    if (music_pos) {
        *music_pos = sm_music_pos();
    }
}
uint32_t pf_hit_room(int slot) { return sm_hit_room(slot); }
void pf_hit_load(int slot, const int16_t *pcm, uint32_t samples, uint32_t rate) { sm_hit_load(slot, pcm, samples, rate); }
void pf_hit_play(int slot) { sm_hit_play(slot); }
void pf_hit_map(uint32_t don_buttons, uint32_t ka_buttons) {
    mask_don = don_buttons;
    mask_ka = ka_buttons;
}
void pf_hit_auto(bool enable) { hit_auto = enable; }
void pf_volume(uint32_t music, uint32_t hits) { sm_volume(music, hits); }
uint32_t pf_blend_budget(void) { return 1000000; }

bool pf_ovl_supported(void) { return !(pf_option("overlay") && pf_option("overlay")[0] == '0'); }
void pf_ovl_sheet(uint32_t addr, const uint8_t *pixels, uint32_t count) { ovl_sheet(addr, pixels, count); }
void pf_ovl_palette(int bank, int index, uint32_t entry) { ovl_palette(bank, index, entry); }
uint32_t pf_ovl_capacity(void) { return PF_OVL_SHEET; }
bool pf_ovl_doubling(void) { return true; }
int pf_ovl_version(void) { return pf_ovl_supported() ? PF_OVL_VERSION : 0; }
void pf_ovl_cell(int cell, uint32_t addr, int w, int h, bool clip, bool twice) {
    ovl_cell(cell, addr, w, h, clip, twice);
}
void pf_ovl_clip(int x0, int x1) { ovl_clip(x0, x1); }
void pf_ovl_show(const uint32_t *sprites, int count) {
    ovl_pending_count = count > PF_OVL_SPRITES ? PF_OVL_SPRITES : count < 0 ? 0 : count;
    if (ovl_pending_count) {
        memcpy(ovl_pending, sprites, (size_t)ovl_pending_count * sizeof *sprites);
    }
}
void pf_debounce(uint32_t ms) { (void)ms; }

bool pf_save_supported(void) { return true; }
bool pf_save_read(void *dst, uint32_t len) {
    if (!save_valid || len > sizeof save) {
        return false;
    }
    memcpy(dst, save, len);
    return true;
}
bool pf_save_write(const void *src, uint32_t len) {
    if (len > sizeof save) {
        return false;
    }
    memcpy(save, src, len);
    save_valid = true;
    return true;
}

uint64_t pf_time_us(void) { return fake_us += 10; }
void pf_log(const char *fmt, ...) {
    if (getenv("TP_HOST_LOG")) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
}
const char *pf_option(const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < option_count; i++) {
        if (!strncmp(options[i], name, n) && options[i][n] == '=') {
            return options[i] + n + 1;
        }
    }
    return NULL;
}
