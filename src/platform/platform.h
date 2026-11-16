/* Platform interface. Everything the game needs from the machine it runs on.
 * Implemented by src/platform/sdl (desktop + web) and src/platform/pocket.
 * The platform owns main(); it calls app_init() once, then app_frame() once per
 * displayed frame, and presents the frame buffer after app_frame() returns. */
#ifndef TP_PLATFORM_H
#define TP_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PF_SCREEN_W 400
#define PF_SCREEN_H 360
#define PF_SAMPLE_RATE 48000

/* ---- application entry points (implemented in src/game) ---- */
void app_init(void);
void app_frame(void);

/* ---- video: RGB565, double buffered ----
 * pf_video_buffer() is the buffer to draw this frame. After the platform
 * presents it, the next call returns the other buffer, which still holds the
 * frame drawn two frames ago. */
uint16_t *pf_video_buffer(void);

/* ---- input ----
 * Button bits follow the APF controller word (docs/control.md of openfpga-litex).
 * Bits 16..19 are "direct" drum keys that only exist on keyboards. */
enum {
    PF_BTN_UP = 1 << 0,
    PF_BTN_DOWN = 1 << 1,
    PF_BTN_LEFT = 1 << 2,
    PF_BTN_RIGHT = 1 << 3,
    PF_BTN_A = 1 << 4,
    PF_BTN_B = 1 << 5,
    PF_BTN_X = 1 << 6,
    PF_BTN_Y = 1 << 7,
    PF_BTN_L1 = 1 << 8,
    PF_BTN_R1 = 1 << 9,
    PF_BTN_L2 = 1 << 10,
    PF_BTN_R2 = 1 << 11,
    PF_BTN_L3 = 1 << 12,
    PF_BTN_R3 = 1 << 13,
    PF_BTN_SELECT = 1 << 14,
    PF_BTN_START = 1 << 15,
    PF_KEY_KA_L = 1 << 16,
    PF_KEY_DON_L = 1 << 17,
    PF_KEY_DON_R = 1 << 18,
    PF_KEY_KA_R = 1 << 19,
};

typedef struct {
    uint32_t button; /* exactly one bit */
    uint8_t pressed; /* 1 = press edge, 0 = release edge */
    int64_t counter; /* output-sample counter at the edge (see pf_audio_clock) */
} pf_input_event;

uint32_t pf_buttons(void);                /* current level state */
bool pf_input_next(pf_input_event *out); /* pop the edge FIFO; false when empty */

/* ---- library file (one data slot, or several volumes) ----
 * Reads are asynchronous and one at a time: start, then poll pf_lib_busy(). */
uint64_t pf_lib_size(void); /* size of volume 0; 0 when there is no library */
void pf_lib_read(uint32_t volume, uint64_t offset, void *dst, uint32_t len);
bool pf_lib_busy(void);
bool pf_lib_failed(void);      /* result of the last finished read */

/* ---- audio ----
 * The output-sample counter is a free-running count of samples sent to the
 * speaker. Input edges are stamped with it, and the music position is tied to
 * it, so game time never depends on when the CPU happened to look. */
enum { PF_CODEC_PCM16 = 0, PF_CODEC_IMA_ADPCM = 1 };

typedef struct {
    uint8_t codec;          /* PF_CODEC_* */
    uint8_t channels;       /* 1 or 2 */
    uint16_t block_bytes;   /* ADPCM: bytes per block; PCM: bytes per frame */
    uint16_t block_samples; /* ADPCM: samples per block; PCM: 1 */
} pf_music_fmt;

/* Memory the music is streamed from. The game fills it through pf_lib_read()
 * and treats it as a ring of `ring_bytes` (a multiple of block_bytes). */
void *pf_music_ring(uint32_t *capacity_bytes);

/* Arm the streamer (paused). Ring byte 0 holds song sample `ring_origin`.
 * `start_pos` is the song sample to start from; a negative value plays that
 * much silence first (lead-in). `gain` is 8.8 fixed point (256 = unity). */
void pf_music_begin(const pf_music_fmt *fmt, uint32_t ring_bytes, int64_t ring_origin,
                    int64_t start_pos, uint32_t gain);
void pf_music_feed(uint64_t total_written); /* ring bytes written since begin */
void pf_music_finish(void);                 /* no more data: play out, then silence */
void pf_music_run(bool run);                /* start / pause */
void pf_music_stop(void);
uint64_t pf_music_consumed(void); /* ring bytes the streamer is done with */

/* Latch the output-sample counter and the song position it corresponds to. */
void pf_audio_clock(int64_t *counter, int64_t *music_pos);

/* Hit sounds: short mono samples kept by the platform (block RAM on the Pocket),
 * mixed after the music with their own volume. Four of them: don and ka, which the
 * hardware starts by itself on a button press, and the sounds of a balloon and of a
 * kusudama bursting, which only the game starts. Each has its share of the memory:
 * pf_hit_room() samples (0: this platform cannot hold that sound). */
enum { PF_HIT_DON = 0, PF_HIT_KA = 1, PF_HIT_BALLOON = 2, PF_HIT_KUSUDAMA = 3, PF_HIT_SLOTS = 4 };
#define PF_HIT_MAX_SAMPLES 16384 /* the largest share */

uint32_t pf_hit_room(int slot);
void pf_hit_load(int slot, const int16_t *pcm, uint32_t samples, uint32_t rate);
void pf_hit_play(int slot);                                 /* CPU trigger */
void pf_hit_map(uint32_t don_buttons, uint32_t ka_buttons); /* hardware trigger map */
void pf_hit_auto(bool enable);                              /* hardware triggering on/off */
void pf_volume(uint32_t music, uint32_t hits);              /* 8.8 fixed point */

/* Button debounce: a press counts at once; a release only counts when the button has
 * been up for `ms`, so its time stamp is that much late. Chatter in between is not
 * seen. Platforms whose inputs do not bounce (keyboard) may ignore it. */
void pf_debounce(uint32_t ms);

/* ---- sprite layer ----
 * Sprites that are not drawn into the frame buffer but mixed into the picture as it
 * goes to the screen: by the FPGA on the Pocket (gateware/taiko/rtl/taiko_sprites.sv),
 * by its software model on the desktop. For everything that moves every frame: the
 * Pocket's CPU needs milliseconds to draw a screen of notes, the FPGA none.
 *
 * - a sheet of 5-bit pixels (0 = transparent) holds the pictures ("cells");
 * - 16 palette banks of 32 entries turn a pixel into a colour and an alpha;
 * - a cell is a rectangle of the sheet, at most 127 x 127, starting at an even
 *   address; a cell can be shown with every pixel doubled (2 x 2), for large soft
 *   pictures that would not fit the sheet otherwise;
 * - a sprite is a cell at a position, with a bank. Up to 128 per frame; the first
 *   in the list is on top. A translucent pixel is mixed with the frame buffer
 *   under it, not with a sprite under it.
 * Cells with `clip` are only drawn between the two x of pf_ovl_clip().
 * The layer draws a line in a fixed time. It passes over what is drawn already and
 * over the part of a row after PF_OVL_ROW_END, so sprites that lie on top of each
 * other cost little; some 35 notes side by side would be too many, but do not fit. */
#define PF_OVL_SHEET 73728 /* the most a platform has; pf_ovl_capacity() is what this one has */
#define PF_OVL_CELLS 256
#define PF_OVL_SPRITES 128
#define PF_OVL_BANKS 16
#define PF_OVL_COLOURS 30 /* per bank, besides transparent (0) and the end of a row (31) */
#define PF_OVL_ROW_END 31 /* sheet value: nothing right of it in this row of the cell; 0 must follow */
#define PF_OVL_MAX_SIZE 127
/* palette entry: alpha 0..16 and the colour as RGB565 already multiplied by alpha / 16 */
#define PF_OVL_ENTRY(alpha, colour) ((uint32_t)(alpha) << 16 | (uint32_t)(colour))
/* sprite: x and y from -512 to 511 */
#define PF_OVL_SPRITE(x, y, cell, bank) \
    ((uint32_t)(bank) << 28 | (uint32_t)(cell) << 20 | ((uint32_t)(y) & 0x3FF) << 10 | ((uint32_t)(x) & 0x3FF))

bool pf_ovl_supported(void);
/* 0: none. 3 knows PF_OVL_ROW_END and passes over pixels that are drawn already, so
 * that a line has time for a whole list of piled-up notes; 2 runs out at about 35.
 * 5 is the first with the fade (gateware/taiko/README.md, "Versions"): the game
 * calls a bitstream below this old. */
#define PF_OVL_VERSION 5
int pf_ovl_version(void);
void pf_ovl_sheet(uint32_t addr, const uint8_t *pixels, uint32_t count);
void pf_ovl_palette(int bank, int index, uint32_t entry);
uint32_t pf_ovl_capacity(void); /* sheet pixels */
bool pf_ovl_doubling(void);     /* cells can be shown doubled */
void pf_ovl_cell(int cell, uint32_t addr, int w, int h, bool clip, bool twice);
void pf_ovl_clip(int x0, int x1);
/* The sprites of the frame being drawn; they appear together with it. Stays until
 * the next call; count 0 shows none. */
void pf_ovl_show(const uint32_t *sprites, int count);

/* Pixels this device can afford to blend per frame for one decorative effect and
 * still hold 60 fps. Effects that need more are left out when the CPU would have to
 * draw them. Desktop: no limit in practice. */
uint32_t pf_blend_budget(void);

/* ---- fade ----
 * The whole picture, sprites included, is shown at `level` sixteenths of its
 * brightness (PF_FADE_FULL: as drawn, 0: black), from the next frame on and until
 * the next call. The Pocket's FPGA does it on the picture's way to the screen; a
 * bitstream from before it shows every frame as drawn, and pf_fade_supported() says
 * so. */
#define PF_FADE_FULL 16
bool pf_fade_supported(void);
void pf_fade(int level);

/* ---- persistence (settings, scores). May be unsupported. ---- */
bool pf_save_supported(void);
bool pf_save_read(void *dst, uint32_t len);
bool pf_save_write(const void *src, uint32_t len);

/* ---- misc ---- */
/* Test hook: value of a "--set name=value" command-line option, or NULL.
 * The Pocket has none. Used to start in auto-play for automated runs. */
const char *pf_option(const char *name);
uint64_t pf_time_us(void);
void pf_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
