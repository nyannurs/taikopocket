/* SDL2 platform: desktop (macOS, Windows, Linux) and, with Emscripten, the browser
 * (web/shell.html supplies the library file; see docs/web.md).
 *
 *   taikopocket [library.tpl] [--scale N] [--frames N] [--shot out.bmp]
 *               [--script "60:A 62:-A 200:START"] [--mute] [--set autoplay=1]
 *
 * --script presses (NAME) and releases (-NAME) buttons at the given frame
 * numbers, --frames quits after N frames and --shot saves the last frame, so
 * the whole game can be driven and checked without a person at the keyboard. */
/* fseeko/ftello with 64-bit offsets: libraries are larger than 2 GB */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "../common/softmix.h"
#include "../common/softovl.h"
#include "../platform.h"

#ifdef __EMSCRIPTEN__
#define AUDIO_BUFFER 1024 /* browsers do not go lower reliably */
#define MUSIC_RING_BYTES (24u << 20)
#else
#define AUDIO_BUFFER 256
#define MUSIC_RING_BYTES (64u << 20)
#endif
#define MAX_VOLUMES 10
#define EVENT_FIFO 256

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static SDL_AudioDeviceID audio_dev;
static SDL_GameController *pad;

static uint16_t fb[2][PF_SCREEN_W * PF_SCREEN_H];
static int fb_index;

static uint32_t buttons;
static pf_input_event fifo[EVENT_FIFO];
static unsigned fifo_head, fifo_tail;

static uint32_t hit_mask_don, hit_mask_ka;
static bool hit_auto_on;

/* clock interpolation between audio callbacks */
static int64_t cb_counter;      /* sm_counter() at the start of the last callback */
static int64_t cb_pos;          /* music position at that moment */
static bool cb_advancing;
static uint64_t cb_time;        /* SDL performance counter at that moment */
static int cb_frames = AUDIO_BUFFER;

static char lib_path[1024];
static FILE *lib_file[MAX_VOLUMES];
static bool lib_fail;
static uint32_t lib_done;

static const char *options[16];
static int option_count;

const char *pf_option(const char *name) {
    size_t n = strlen(name);
    for (int i = 0; i < option_count; i++) {
        if (!strncmp(options[i], name, n) && options[i][n] == '=') {
            return options[i] + n + 1;
        }
    }
    return NULL;
}

static bool quit_requested;
static long max_frames = -1, frame_no;
static const char *shot_path;
static const char *script;
static bool mute;

/* ---------- video ---------- */
uint16_t *pf_video_buffer(void) { return fb[fb_index]; }

/* ---------- sprite layer: the gateware's software model ---------- */
static uint16_t shown[PF_SCREEN_W * PF_SCREEN_H]; /* the frame as displayed: frame buffer plus sprites */
static uint32_t ovl_pending[PF_OVL_SPRITES];
static int ovl_pending_count;

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

static int ovl_frames, ovl_frames_over, ovl_worst, ovl_worst_frame, ovl_first_over; /* for --set ovlstat=1 */

/* what the screen shows for the frame just drawn */
static void compose_shown(void) {
    memcpy(shown, fb[fb_index], sizeof shown);
    ovl_sprites(ovl_pending, ovl_pending_count);
    ovl_compose(shown);
    int peak = ovl_peak();
    ovl_frames++;
    ovl_frames_over += peak > OVL_LINE_CYCLES;
    if (peak > OVL_LINE_CYCLES + 300 && !ovl_first_over) {
        ovl_first_over = ovl_frames;
    }
    if (peak > ovl_worst) {
        ovl_worst = peak;
        ovl_worst_frame = ovl_frames;
    }
}

/* ---------- audio ---------- */
static void audio_cb(void *user, Uint8 *stream, int len) {
    (void)user;
    int frames = len / 4;
    cb_counter = sm_counter();
    cb_pos = sm_music_pos();
    cb_advancing = sm_music_advancing();
    cb_time = SDL_GetPerformanceCounter();
    cb_frames = frames;
    sm_render((int16_t *)stream, frames);
    if (mute) {
        memset(stream, 0, (size_t)len);
    }
}

/* Samples the device has been given, extrapolated from the last callback and
 * moved back by one buffer: about what is leaving the speaker now. */
static void clock_now(int64_t *counter, int64_t *pos) {
    SDL_LockAudioDevice(audio_dev);
    int64_t c = cb_counter, p = cb_pos;
    bool adv = cb_advancing;
    uint64_t t = cb_time;
    int frames = cb_frames;
    SDL_UnlockAudioDevice(audio_dev);
    int64_t el = 0;
    if (t) {
        el = (int64_t)((SDL_GetPerformanceCounter() - t) * (uint64_t)PF_SAMPLE_RATE /
                       SDL_GetPerformanceFrequency());
        if (el > frames) {
            el = frames;
        }
    }
    el -= frames;
    if (counter) {
        *counter = c + el;
    }
    if (pos) {
        *pos = adv ? p + el : p;
    }
}

void pf_audio_clock(int64_t *counter, int64_t *music_pos) { clock_now(counter, music_pos); }

#define LOCKED(stmt)                   \
    do {                               \
        SDL_LockAudioDevice(audio_dev); \
        stmt;                          \
        SDL_UnlockAudioDevice(audio_dev); \
    } while (0)

static uint8_t *music_ring;

void *pf_music_ring(uint32_t *capacity_bytes) {
    *capacity_bytes = MUSIC_RING_BYTES;
    return music_ring;
}
void pf_music_begin(const pf_music_fmt *fmt, uint32_t ring_bytes, int64_t ring_origin,
                    int64_t start_pos, uint32_t gain) {
    LOCKED(sm_music_begin(fmt, ring_bytes, ring_origin, start_pos, gain); cb_pos = start_pos;
           cb_advancing = false);
}
void pf_music_feed(uint64_t total_written) { LOCKED(sm_music_feed(total_written)); }
void pf_music_finish(void) { LOCKED(sm_music_finish()); }
void pf_music_run(bool run) { LOCKED(sm_music_run(run)); }
void pf_music_stop(void) { LOCKED(sm_music_stop()); }
uint64_t pf_music_consumed(void) {
    uint64_t v;
    LOCKED(v = sm_music_consumed());
    return v;
}
uint32_t pf_hit_room(int slot) { return sm_hit_room(slot); }
void pf_hit_load(int slot, const int16_t *pcm, uint32_t samples, uint32_t rate) {
    LOCKED(sm_hit_load(slot, pcm, samples, rate));
}
void pf_hit_play(int slot) { LOCKED(sm_hit_play(slot)); }
void pf_hit_map(uint32_t don_buttons, uint32_t ka_buttons) {
    hit_mask_don = don_buttons;
    hit_mask_ka = ka_buttons;
}
void pf_hit_auto(bool enable) { hit_auto_on = enable; }
void pf_volume(uint32_t music, uint32_t hits) { LOCKED(sm_volume(music, hits)); }
uint32_t pf_blend_budget(void) { return 1000000; }
void pf_debounce(uint32_t ms) { (void)ms; } /* keyboards and pads here do not bounce */

/* ---------- input ---------- */
uint32_t pf_buttons(void) { return buttons; }

bool pf_input_next(pf_input_event *out) {
    if (fifo_head == fifo_tail) {
        return false;
    }
    *out = fifo[fifo_tail];
    fifo_tail = (fifo_tail + 1) % EVENT_FIFO;
    return true;
}

static void button_edge(uint32_t bit, bool pressed) {
    if (!bit || ((buttons & bit) != 0) == pressed) {
        return;
    }
    buttons = pressed ? buttons | bit : buttons & ~bit;
    /* what the gateware does on the Pocket: sound first, CPU later */
    if (pressed && hit_auto_on) {
        if (bit & hit_mask_don) {
            pf_hit_play(PF_HIT_DON);
        } else if (bit & hit_mask_ka) {
            pf_hit_play(PF_HIT_KA);
        }
    }
    unsigned next = (fifo_head + 1) % EVENT_FIFO;
    if (next == fifo_tail) {
        return; /* full: drop, like the hardware FIFO */
    }
    fifo[fifo_head].button = bit;
    fifo[fifo_head].pressed = pressed;
    clock_now(&fifo[fifo_head].counter, NULL);
    fifo_head = next;
}

static uint32_t key_bit(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP: return PF_BTN_UP;
    case SDLK_DOWN: return PF_BTN_DOWN;
    case SDLK_LEFT: return PF_BTN_LEFT;
    case SDLK_RIGHT: return PF_BTN_RIGHT;
    case SDLK_x: return PF_BTN_A;
    case SDLK_z: return PF_BTN_B;
    case SDLK_s: return PF_BTN_X;
    case SDLK_a: return PF_BTN_Y;
    case SDLK_q: return PF_BTN_L1;
    case SDLK_w: return PF_BTN_R1;
    case SDLK_RETURN: return PF_BTN_START;
    case SDLK_ESCAPE: return PF_BTN_START;
    case SDLK_BACKSPACE: return PF_BTN_SELECT;
    case SDLK_TAB: return PF_BTN_SELECT;
    case SDLK_d: return PF_KEY_KA_L;
    case SDLK_f: return PF_KEY_DON_L;
    case SDLK_j: return PF_KEY_DON_R;
    case SDLK_k: return PF_KEY_KA_R;
    default: return 0;
    }
}

static uint32_t pad_bit(Uint8 b) {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return PF_BTN_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return PF_BTN_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return PF_BTN_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return PF_BTN_RIGHT;
    /* SDL names buttons by Xbox position; the Pocket uses Nintendo names */
    case SDL_CONTROLLER_BUTTON_B: return PF_BTN_A;
    case SDL_CONTROLLER_BUTTON_A: return PF_BTN_B;
    case SDL_CONTROLLER_BUTTON_Y: return PF_BTN_X;
    case SDL_CONTROLLER_BUTTON_X: return PF_BTN_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return PF_BTN_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PF_BTN_R1;
    case SDL_CONTROLLER_BUTTON_BACK: return PF_BTN_SELECT;
    case SDL_CONTROLLER_BUTTON_START: return PF_BTN_START;
    default: return 0;
    }
}

static uint32_t name_bit(const char *name, size_t len) {
    static const struct {
        const char *name;
        uint32_t bit;
    } names[] = {
        {"UP", PF_BTN_UP},       {"DOWN", PF_BTN_DOWN},     {"LEFT", PF_BTN_LEFT},
        {"RIGHT", PF_BTN_RIGHT}, {"A", PF_BTN_A},           {"B", PF_BTN_B},
        {"X", PF_BTN_X},         {"Y", PF_BTN_Y},           {"L1", PF_BTN_L1},
        {"R1", PF_BTN_R1},       {"SELECT", PF_BTN_SELECT}, {"START", PF_BTN_START},
        {"KAL", PF_KEY_KA_L},    {"DONL", PF_KEY_DON_L},    {"DONR", PF_KEY_DON_R},
        {"KAR", PF_KEY_KA_R},
    };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        if (strlen(names[i].name) == len && !strncmp(names[i].name, name, len)) {
            return names[i].bit;
        }
    }
    return 0;
}

/* "60:A 62:-A": run the entries whose frame number is the current frame */
static void run_script(void) {
    const char *p = script;
    while (p && *p) {
        char *end;
        long frame = strtol(p, &end, 10);
        if (*end != ':') {
            break;
        }
        const char *name = end + 1;
        bool press = true;
        if (*name == '-') {
            press = false;
            name++;
        }
        size_t len = strcspn(name, " ");
        if (frame == frame_no) {
            button_edge(name_bit(name, len), press);
        }
        p = name + len;
        while (*p == ' ') {
            p++;
        }
    }
}

static void pump_events(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: quit_requested = true; break;
        case SDL_KEYDOWN:
            if (!e.key.repeat) {
                button_edge(key_bit(e.key.keysym.sym), true);
            }
            break;
        case SDL_KEYUP: button_edge(key_bit(e.key.keysym.sym), false); break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!pad) {
                pad = SDL_GameControllerOpen(e.cdevice.which);
            }
            break;
        case SDL_CONTROLLERBUTTONDOWN: button_edge(pad_bit(e.cbutton.button), true); break;
        case SDL_CONTROLLERBUTTONUP: button_edge(pad_bit(e.cbutton.button), false); break;
        default: break;
        }
    }
    if (script) {
        run_script();
    }
}

/* ---------- library file ---------- */
#ifdef __EMSCRIPTEN__
/* In the browser the library is a File (or a fetched Blob) kept by the page in
 * Module.tpFiles[volume]. Reads are slices of it, delivered asynchronously. */
static bool web_busy;
static uint32_t web_len;

EM_JS(double, js_lib_size, (int volume), {
    var f = Module.tpFiles && Module.tpFiles[volume];
    return f ? f.size : 0;
});

EM_JS(void, js_lib_read, (int volume, double offset, int dst, int len), {
    var f = Module.tpFiles && Module.tpFiles[volume];
    if (!f) {
        setTimeout(function() { Module._web_read_done(-1); }, 0);
        return;
    }
    f.slice(offset, offset + len).arrayBuffer().then(function(buf) {
        HEAPU8.set(new Uint8Array(buf), dst);
        Module._web_read_done(buf.byteLength);
    }, function() { Module._web_read_done(-1); });
});

EM_JS(int, js_save_read, (int dst, int len), {
    try {
        var s = localStorage.getItem("taikopocket.save");
        if (!s || s.length != len * 2) return 0;
        for (var i = 0; i < len; i++) HEAPU8[dst + i] = parseInt(s.substr(i * 2, 2), 16);
        return 1;
    } catch (e) { return 0; }
});

EM_JS(int, js_save_write, (int src, int len), {
    try {
        var parts = [];
        for (var i = 0; i < len; i++) parts.push((HEAPU8[src + i] + 256).toString(16).substr(1));
        localStorage.setItem("taikopocket.save", parts.join(""));
        return 1;
    } catch (e) { return 0; }
});

/* for measuring clock drift from the page: the game's idea of the song position, and raw mixer state */
EMSCRIPTEN_KEEPALIVE double web_music_pos(void) {
    int64_t counter, pos;
    pf_audio_clock(&counter, &pos);
    return (double)pos;
}
EMSCRIPTEN_KEEPALIVE double web_mixer_counter(void) { return (double)sm_counter(); }
EMSCRIPTEN_KEEPALIVE int web_callback_frames(void) { return cb_frames; }

EMSCRIPTEN_KEEPALIVE void web_read_done(int n) {
    lib_done = n < 0 ? 0 : (uint32_t)n;
    lib_fail = lib_done != web_len;
    web_busy = false;
}

uint64_t pf_lib_size(void) { return (uint64_t)js_lib_size(0); }
void pf_lib_read(uint32_t volume, uint64_t offset, void *dst, uint32_t len) {
    web_busy = true;
    web_len = len;
    lib_done = 0;
    js_lib_read((int)volume, (double)offset, (int)(intptr_t)dst, (int)len);
}
bool pf_lib_busy(void) { return web_busy; }
bool pf_lib_failed(void) { return lib_fail; }

bool pf_save_supported(void) { return true; }
bool pf_save_read(void *dst, uint32_t len) { return js_save_read((int)(intptr_t)dst, (int)len) != 0; }
bool pf_save_write(const void *src, uint32_t len) { return js_save_write((int)(intptr_t)src, (int)len) != 0; }
#else
static FILE *volume_file(uint32_t volume) {
    if (volume >= MAX_VOLUMES) {
        return NULL;
    }
    if (!lib_file[volume] && volume > 0) {
        /* library.tpl, library.tp1, library.tp2, ... */
        char path[1024];
        snprintf(path, sizeof path, "%s", lib_path);
        size_t n = strlen(path);
        if (n > 0) {
            path[n - 1] = (char)('0' + volume);
        }
        lib_file[volume] = fopen(path, "rb");
    }
    return lib_file[volume];
}

uint64_t pf_lib_size(void) {
    FILE *f = lib_file[0];
    if (!f) {
        return 0;
    }
#ifdef _WIN32
    _fseeki64(f, 0, SEEK_END);
    return (uint64_t)_ftelli64(f);
#else
    fseeko(f, 0, SEEK_END);
    return (uint64_t)ftello(f);
#endif
}

void pf_lib_read(uint32_t volume, uint64_t offset, void *dst, uint32_t len) {
    FILE *f = volume_file(volume);
    lib_fail = true;
    lib_done = 0;
    if (!f) {
        return;
    }
#ifdef _WIN32
    if (_fseeki64(f, (long long)offset, SEEK_SET)) {
        return;
    }
#else
    if (fseeko(f, (off_t)offset, SEEK_SET)) {
        return;
    }
#endif
    lib_done = (uint32_t)fread(dst, 1, len, f);
    lib_fail = lib_done != len;
}
bool pf_lib_busy(void) { return false; }
bool pf_lib_failed(void) { return lib_fail; }

/* ---------- persistence: <library>.sav next to the library ---------- */
static void save_path(char *out, size_t n) { snprintf(out, n, "%s.sav", lib_path); }

bool pf_save_supported(void) { return lib_path[0] != 0; }
bool pf_save_read(void *dst, uint32_t len) {
    char path[1100];
    save_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    bool ok = fread(dst, 1, len, f) == len;
    fclose(f);
    return ok;
}
bool pf_save_write(const void *src, uint32_t len) {
    char path[1100];
    save_path(path, sizeof path);
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    bool ok = fwrite(src, 1, len, f) == len;
    fclose(f);
    return ok;
}

#endif

/* ---------- misc ---------- */
uint64_t pf_time_us(void) {
    return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency();
}

void pf_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}


static void save_shot(void) {
    /* the buffer that was presented last */
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(shown, PF_SCREEN_W, PF_SCREEN_H,
                                                        16, PF_SCREEN_W * 2, SDL_PIXELFORMAT_RGB565);
    if (s) {
        SDL_SaveBMP(s, shot_path);
        SDL_FreeSurface(s);
    }
}

static void one_frame(void) {
#ifdef __EMSCRIPTEN__
    /* requestAnimationFrame may run at 120 Hz or more: keep the game at 60 */
    static double last_ms;
    double now_ms = emscripten_get_now();
    if (now_ms - last_ms < 15.0) {
        return;
    }
    last_ms = now_ms;
#else
    /* 60 fps even without vsync (headless) or on a 120 Hz display */
    static uint64_t last;
    uint64_t now = pf_time_us();
    if (last && now - last < 16000) {
        SDL_Delay((Uint32)((16000 - (now - last)) / 1000));
    }
    last = pf_time_us();
#endif
    pump_events();
    app_frame();
    compose_shown();
    SDL_UpdateTexture(texture, NULL, shown, PF_SCREEN_W * 2);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    fb_index ^= 1;
    frame_no++;
    if (max_frames >= 0 && frame_no >= max_frames) {
        quit_requested = true;
    }
#ifdef __EMSCRIPTEN__
    if (quit_requested) {
        emscripten_cancel_main_loop();
    }
#endif
}

int main(int argc, char **argv) {
    int scale = 2;
    const char *path = "build/library.tpl";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
            scale = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            max_frames = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--shot") && i + 1 < argc) {
            shot_path = argv[++i];
        } else if (!strcmp(argv[i], "--script") && i + 1 < argc) {
            script = argv[++i];
        } else if (!strcmp(argv[i], "--set") && i + 1 < argc) {
            if (option_count < 16) {
                options[option_count++] = argv[++i];
            }
        } else if (!strcmp(argv[i], "--mute")) {
            mute = true;
        } else {
            path = argv[i];
        }
    }
    snprintf(lib_path, sizeof lib_path, "%s", path);
#ifndef __EMSCRIPTEN__
    lib_file[0] = fopen(lib_path, "rb");
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    window = SDL_CreateWindow("Taiko Pocket", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              PF_SCREEN_W * scale, PF_SCREEN_H * scale, SDL_WINDOW_RESIZABLE);
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_RenderSetLogicalSize(renderer, PF_SCREEN_W, PF_SCREEN_H);
    SDL_RenderSetIntegerScale(renderer, SDL_TRUE);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                PF_SCREEN_W, PF_SCREEN_H);

    music_ring = malloc(MUSIC_RING_BYTES);
    sm_init(music_ring, MUSIC_RING_BYTES);

    SDL_AudioSpec want = {0}, have;
    want.freq = PF_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = AUDIO_BUFFER;
    want.callback = audio_cb;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!audio_dev) {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
    }
    SDL_PauseAudioDevice(audio_dev, 0);

    app_init();
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(one_frame, 0, 1);
#else
    while (!quit_requested) {
        one_frame();
    }
    if (shot_path) {
        save_shot();
    }
    SDL_CloseAudioDevice(audio_dev);
    if (pf_option("ovlstat")) { /* test hook: how close the sprite layer came to its time per line */
        printf("sprite layer: busiest line %d of %d cycles, in frame %d; %d of %d frames had a line over the limit, "
               "the first well over it is frame %d\n",
               ovl_worst, OVL_LINE_CYCLES, ovl_worst_frame, ovl_frames_over, ovl_frames, ovl_first_over);
    }
    SDL_Quit();
#endif
    return 0;
}
