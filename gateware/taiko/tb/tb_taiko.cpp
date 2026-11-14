// Test bench for the Taiko gateware (taiko_top and everything below it).
//
// The peripheral is driven the way the firmware drives it, through its
// Wishbone registers, with behavioural models of the SDRAM (for the streamer's
// bus master), the music FIFO and the 48 kHz tick. The audio it produces is
// compared sample for sample with src/platform/common/softmix.c, which is what
// the desktop build plays through.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include "Vtaiko_top.h"
#include "Vtaiko_top___024root.h"
#include "verilated.h"

extern "C" {
#include "platform/common/softmix.h"
#include "platform/common/softovl.h"
}

static const uint64_t SYS_PS = 17507;    // 57.12 MHz
static const uint64_t AUDIO_PS = 81380;  // 12.288 MHz
static const uint64_t VID_PS = 5 * SYS_PS; // 11.424 MHz pixel clock
// the video timing the firmware programs: 400 x 360 in 476 x 400
enum { H_TOTAL = 476, V_TOTAL = 400, HS_START = 408, HS_END = 440, VS_START = 361, VS_END = 370 };
static const uint32_t RING_BASE = 0x42000000;

enum { R_ID, R_CTRL, R_CMD, R_GAIN, R_VOL_MUSIC, R_VOL_HITS, R_POS_LO, R_POS_HI, R_LEAD, R_SKIP, R_RING_BASE,
       R_RING_BYTES, R_FED, R_CONSUMED, R_FORMAT, R_HIT_ADDR, R_HIT_DATA, R_HIT_LEN0, R_HIT_LEN1, R_HIT_STEP0,
       R_HIT_STEP1, R_DON_MASK, R_KA_MASK, R_COUNTER_LO, R_COUNTER_HI, R_CLOCK_POS_LO, R_CLOCK_POS_HI, R_STATUS,
       R_EVENT, R_EVENT_TIME, R_KEYS, R_VIDEO, R_BRIDGE_HI, R_DEBOUNCE, R_OVL_CTRL, R_OVL_ADDR, R_OVL_DATA,
       R_OVL_CLIP };
enum { CTRL_STREAM = 1, CTRL_RUN = 2, CTRL_FINISHED = 4, CTRL_HIT_AUTO = 8 };
enum { CMD_BEGIN = 1, CMD_DON = 2, CMD_KA = 4, CMD_LATCH = 8, CMD_CLEAR = 16 };

static int failures, checks;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        checks++;                                           \
        if (!(cond)) {                                      \
            failures++;                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

struct Sample {
    int16_t l, r;
    int64_t pos; // song position just after this tick
};

struct Sim {
    Vtaiko_top* top = new Vtaiko_top;
    uint64_t next_sys = SYS_PS / 2, next_audio = AUDIO_PS / 2, next_vid = VID_PS / 2 + 3000;
    // video: a test picture goes in, what comes out is recorded per frame
    bool video = false;
    int vid_h = 0, vid_v = 0;
    uint64_t vid_frames = 0;                // frames whose last active pixel has gone in
    std::deque<uint32_t> vid_expect_xy;     // (y << 16 | x) of pixels on their way through
    std::vector<uint32_t> vid_frame;        // output of the frame being recorded, HRES * VRES
    std::vector<uint32_t> vid_done;         // the last complete one
    static uint32_t picture(int x, int y) { return (uint32_t)((x * 5 + y * 3) & 255) << 16 | (uint32_t)((x ^ y) & 255) << 8 | (uint32_t)((y * 7 + 40) & 255); }

    void vid_edge() {
        bool de = vid_h < OVL_W && vid_v < OVL_H;
        top->vid_de_in = de;
        top->vid_hs_in = vid_h >= HS_START && vid_h < HS_END;
        top->vid_vs_in = vid_v >= VS_START && vid_v < VS_END;
        top->vid_rgb_in = de ? picture(vid_h, vid_v) : 0;
        if (de) {
            vid_expect_xy.push_back((uint32_t)vid_v << 16 | (uint32_t)vid_h);
        }
        top->clk_vid = 1;
        top->eval();
        if (top->vid_de_out && !vid_expect_xy.empty()) {
            uint32_t xy = vid_expect_xy.front();
            vid_expect_xy.pop_front();
            if (vid_frame.empty()) {
                vid_frame.assign(OVL_W * OVL_H, 0);
            }
            vid_frame[(xy >> 16) * OVL_W + (xy & 0xFFFF)] = top->vid_rgb_out;
            if (xy == ((uint32_t)(OVL_H - 1) << 16 | (OVL_W - 1))) {
                vid_done = vid_frame;
                vid_frames++;
            }
        }
        if (++vid_h == H_TOTAL) {
            vid_h = 0;
            if (++vid_v == V_TOTAL) {
                vid_v = 0;
            }
        }
    }
    // run until `n` more frames have come out completely
    void run_frames(uint64_t n) {
        uint64_t target = vid_frames + n;
        while (vid_frames < target) {
            step();
        }
    }

    std::map<uint32_t, uint32_t> memory; // word address -> word
    std::deque<uint32_t> fifo;           // the music FIFO
    int tick_div = 100;                  // cycles until the next tick
    uint64_t audio_cycles = 0;
    std::vector<Sample> out; // one entry per tick
    bool wb_pending = false;

    Sim() {
        top->clk_sys = top->clk_audio = 0;
        top->rst = 0;
        top->wbs_cyc = top->wbs_stb = top->wbs_we = 0;
        top->wbm_ack = 0;
        top->cpu_audio_wr = 0;
        top->cpu_playback_en = 0;
        top->keys_async = 0;
        top->vblank_async = 0;
        top->fifo_rdempty = 1;
        top->fifo_wrfull = 0;
        top->tick = 0;
        top->clk_vid = 0;
        top->vid_de_in = top->vid_hs_in = top->vid_vs_in = 0;
        top->vid_rgb_in = 0;
        top->eval();
    }

    int64_t pos() const {
        int64_t p = (int64_t)top->rootp->taiko_top__DOT__audio__DOT__pos;
        return (p << 16) >> 16; // 48-bit signed
    }
    int cycle_in_period() const { return (256 - tick_div) & 255; }

    void sys_edge() {
        top->eval(); // settle the combinational outputs for the inputs set since the last edge
        // inputs for this edge: SDRAM answers one cycle after the request, the FIFO takes writes
        bool ack = wb_pending;
        top->wbm_ack = ack;
        if (ack) {
            top->wbm_dat_r = memory.count(top->wbm_adr) ? memory[top->wbm_adr] : 0xDEADBEEF;
        }
        bool request = top->wbm_cyc && top->wbm_stb && !ack;
        if (top->fifo_wrreq && fifo.size() < 4096) {
            fifo.push_back(top->fifo_wr_data);
        }
        top->clk_sys = 1;
        top->eval();
        wb_pending = request;
        top->fifo_wrfull = fifo.size() >= 4095;
        top->fifo_rdempty = fifo.empty();
    }

    void audio_edge() {
        // the FIFO's read side: data one cycle after rdreq
        if (top->fifo_rdreq && !fifo.empty()) {
            top->fifo_q = fifo.front();
            fifo.pop_front();
        }
        top->fifo_rdempty = fifo.empty();
        bool tick = tick_div == 0;
        top->tick = tick;
        tick_div = tick ? 255 : tick_div - 1;
        top->clk_audio = 1;
        top->eval();
        audio_cycles++;
        if (tick) {
            out.push_back({(int16_t)top->out_l, (int16_t)top->out_r, pos()});
        }
    }

    // advance to the next clock edge of any domain
    void step() {
        if (video && next_vid <= next_sys && next_vid <= next_audio) {
            if (top->clk_vid) {
                top->clk_vid = 0;
                top->eval();
            } else {
                vid_edge();
            }
            next_vid += VID_PS / 2;
        } else if (next_sys <= next_audio) {
            if (top->clk_sys) {
                top->clk_sys = 0;
                top->eval();
            } else {
                sys_edge();
            }
            next_sys += SYS_PS / 2;
        } else {
            if (top->clk_audio) {
                top->clk_audio = 0;
                top->eval();
            } else {
                audio_edge();
            }
            next_audio += AUDIO_PS / 2;
        }
    }

    void sys_cycles(int n) {
        while (n > 0) {
            bool was = top->clk_sys;
            step();
            if (!was && top->clk_sys) {
                n--;
            }
        }
    }
    void run_ticks(size_t n) {
        size_t target = out.size() + n;
        while (out.size() < target) {
            step();
        }
    }
    // run until `cycle` audio cycles after a tick, so things land safely before the sample is computed
    void run_to_cycle(int cycle) {
        while (cycle_in_period() != cycle || top->clk_audio) {
            step();
        }
    }

    void bus(uint32_t reg, bool we, uint32_t value, uint32_t* read) {
        while (top->clk_sys) {
            step();
        }
        top->wbs_adr = reg;
        top->wbs_dat_w = value;
        top->wbs_we = we;
        top->wbs_cyc = top->wbs_stb = 1;
        int guard = 0;
        do {
            sys_cycles(1);
        } while (!top->wbs_ack && ++guard < 100);
        if (read) {
            *read = top->wbs_dat_r;
        }
        top->wbs_cyc = top->wbs_stb = 0;
        sys_cycles(1);
    }
    void wr(uint32_t reg, uint32_t value) { bus(reg, true, value, nullptr); }
    uint32_t rd(uint32_t reg) {
        uint32_t v = 0;
        bus(reg, false, 0, &v);
        return v;
    }

    void load_hit(int slot, const int16_t* pcm, int n, uint32_t rate) {
        wr(R_HIT_ADDR, slot << 14);
        for (int i = 0; i < n; i++) {
            wr(R_HIT_DATA, (uint16_t)pcm[i]);
        }
        wr(slot ? R_HIT_LEN1 : R_HIT_LEN0, n);
        wr(slot ? R_HIT_STEP1 : R_HIT_STEP0, (uint32_t)(((uint64_t)rate << 16) / 48000));
    }
    void ring_write(uint32_t byte_offset, const uint8_t* data, uint32_t n) {
        for (uint32_t i = 0; i < n; i++) {
            uint32_t a = RING_BASE + byte_offset + i;
            uint32_t& w = memory[a >> 2];
            w = (w & ~(0xFFu << (8 * (a & 3)))) | ((uint32_t)data[i] << (8 * (a & 3)));
        }
    }
    void latch_clock(int64_t* counter, int64_t* position) {
        wr(R_CMD, CMD_LATCH);
        *counter = (int64_t)rd(R_COUNTER_LO) | ((int64_t)rd(R_COUNTER_HI) << 32);
        *position = (int64_t)(((uint64_t)rd(R_CLOCK_POS_LO) | ((uint64_t)rd(R_CLOCK_POS_HI) << 32)) << 16) >> 16;
    }
};

static uint32_t rng_state = 12345;
static uint32_t rng() {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static int16_t don[300], ka[200];
static uint8_t sm_ring[1 << 16];

static void make_hits() {
    for (int i = 0; i < 300; i++) {
        don[i] = (int16_t)(20000 - i * 60);
    }
    for (int i = 0; i < 200; i++) {
        ka[i] = (int16_t)((i & 1) ? -9000 : 9000);
    }
}

// ---- 1. hit sounds: latency, polyphony, restart, saturation, volume, against softmix ----------
static void test_hits() {
    printf("hit sounds\n");
    Sim s;
    sm_init(sm_ring, sizeof sm_ring);
    s.load_hit(0, don, 300, 48000);
    s.load_hit(1, ka, 200, 24000); // half rate: every stored sample plays twice
    sm_hit_load(PF_HIT_DON, don, 300, 48000);
    sm_hit_load(PF_HIT_KA, ka, 200, 24000);
    s.wr(R_DON_MASK, 0x0030);
    s.wr(R_KA_MASK, 0x0300);
    s.wr(R_VOL_HITS, 256);
    s.wr(R_CTRL, CTRL_HIT_AUTO);
    s.run_ticks(4);

    // a button press: when is the first sample of the hit sound at the output?
    s.run_to_cycle(20);
    uint64_t press_cycle = s.audio_cycles;
    size_t press_tick = s.out.size();
    s.top->keys_async = 0x0010;
    s.run_ticks(3);
    size_t first = press_tick;
    while (first < s.out.size() && s.out[first].l == 0) {
        first++;
    }
    CHECK(first == press_tick, "first audible sample at tick +%zu, expected the very next tick", first - press_tick);
    CHECK(s.out[first].l == don[0], "first sample %d, expected %d", s.out[first].l, don[0]);
    int cycles_to_output = 256 - 20; // the press came 20 cycles into the period; its sample is latched at the next tick
    printf("  button edge to the sample at the serialiser: %d audio clock cycles (%.1f us), less than one sample period\n",
           cycles_to_output, cycles_to_output * 1e6 / 12288000.0);
    (void)press_cycle;
    s.top->keys_async = 0;
    s.run_ticks(400);

    // a scripted sequence of triggers, mirrored in softmix
    struct Trig {
        int tick;
        int kind; // 0 don key, 1 ka key, 2 don cpu, 3 ka cpu, 4 both keys at once
    };
    std::vector<Trig> script = {{10, 0}, {12, 1}, {13, 2}, {14, 3}, {15, 0}, {16, 0}, {17, 0}, {90, 4},
                                {91, 2}, {300, 1}, {301, 1}, {302, 1}, {303, 1}, {304, 1}, {700, 0}};
    size_t base = s.out.size();
    std::vector<int16_t> expect;
    size_t next = 0;
    int vol = 256;
    for (int t = 0; t < 1200; t++) {
        s.run_to_cycle(30);
        if (t == 500) { // quieter hits from here on
            vol = 100;
            s.wr(R_VOL_HITS, vol);
            sm_volume(256, vol);
        }
        s.top->keys_async = 0;
        s.run_to_cycle(60);
        while (next < script.size() && script[next].tick == t) {
            int k = script[next++].kind;
            if (k == 0) {
                s.top->keys_async |= 0x0020;
                sm_hit_play(PF_HIT_DON);
            } else if (k == 1) {
                s.top->keys_async |= 0x0100;
                sm_hit_play(PF_HIT_KA);
            } else if (k == 2) {
                s.wr(R_CMD, CMD_DON);
                sm_hit_play(PF_HIT_DON);
            } else if (k == 3) {
                s.wr(R_CMD, CMD_KA);
                sm_hit_play(PF_HIT_KA);
            } else {
                s.top->keys_async |= 0x0120; // don wins when both arrive in the same cycle
                sm_hit_play(PF_HIT_DON);
            }
        }
        int16_t frame[2];
        sm_render(frame, 1);
        expect.push_back(frame[0]);
        s.run_ticks(1);
    }
    int mismatches = 0, peak = 0;
    for (size_t i = 0; i < expect.size(); i++) {
        // the sample computed in period i is latched for the serialiser at the tick that ends it
        int16_t got = s.out[base + i].l;
        if (abs(got) > peak) {
            peak = abs(got);
        }
        if (got != expect[i]) {
            if (mismatches++ < 5) {
                printf("  sample %zu: gateware %d, softmix %d\n", i, got, expect[i]);
            }
        }
    }
    CHECK(mismatches == 0, "%d of %zu samples differ from softmix", mismatches, expect.size());
    CHECK(peak == 32767, "the mix should saturate at 32767 with stacked voices, peak was %d", peak);
    delete s.top;
}

// ---- 2. input events ----------------------------------------------------------------------------
static void test_input() {
    printf("input timestamps\n");
    Sim s;
    s.run_ticks(50);
    struct Edge {
        int tick;
        uint16_t keys;
    };
    std::vector<Edge> edges = {{60, 0x0001}, {65, 0x0003}, {70, 0x0002}, {70, 0x8002}, {200, 0x0000}, {201, 0x0F00}};
    std::vector<std::pair<int64_t, uint16_t>> applied; // counter value, key word
    size_t start = s.out.size();
    size_t e = 0;
    for (int t = 50; t < 260; t++) {
        s.run_to_cycle(100);
        while (e < edges.size() && edges[e].tick == t) {
            s.top->keys_async = edges[e].keys;
            applied.push_back({(int64_t)s.out.size(), edges[e].keys});
            e++;
            s.sys_cycles(30);
        }
        s.run_ticks(1);
    }
    (void)start;
    // expected events: every changed bit of each step, with the counter at that moment
    uint16_t prev = 0;
    struct Ev {
        int button, pressed;
        int64_t time;
    };
    std::vector<Ev> expect;
    for (auto& a : applied) {
        std::vector<Ev> group;
        for (int b = 0; b < 16; b++) {
            if ((a.second ^ prev) >> b & 1) {
                group.push_back({b, (a.second >> b) & 1, a.first});
            }
        }
        expect.insert(expect.end(), group.begin(), group.end());
        prev = a.second;
    }
    size_t got = 0;
    int wrong = 0;
    while (s.rd(R_STATUS) & 2) {
        uint32_t info = s.rd(R_EVENT);
        uint32_t time = s.rd(R_EVENT_TIME);
        if (got < expect.size()) {
            const Ev& x = expect[got];
            bool ok = (int)(info & 15) == x.button && (int)((info >> 4) & 1) == x.pressed && (int64_t)time == x.time;
            if (!ok && wrong++ < 5) {
                printf("  event %zu: button %u pressed %u time %u, expected %d %d %lld\n", got, info & 15,
                       (info >> 4) & 1, time, x.button, x.pressed, (long long)x.time);
            }
        }
        got++;
    }
    CHECK(got == expect.size(), "%zu events read, %zu expected", got, expect.size());
    CHECK(wrong == 0, "%d events with a wrong button, direction or timestamp", wrong);
    CHECK((s.rd(R_STATUS) & 4) == 0, "event FIFO overflow flag set");
    CHECK(s.rd(R_KEYS) == 0x0F00, "key word read back %x", s.rd(R_KEYS));

    // more edges than the FIFO holds: the overflow flag is raised and cleared on request
    for (int i = 0; i < 80; i++) {
        s.top->keys_async ^= 0x0001;
        s.sys_cycles(40);
    }
    CHECK((s.rd(R_STATUS) & 4) != 0, "overflow flag not set after 80 unread events");
    while (s.rd(R_STATUS) & 2) {
        s.rd(R_EVENT_TIME);
    }
    s.wr(R_CMD, CMD_CLEAR);
    CHECK((s.rd(R_STATUS) & 4) == 0, "overflow flag not cleared");
    delete s.top;
}

// ---- 2b. debounce -----------------------------------------------------------------------------
// A press counts at once; a release only after the button has been up for R_DEBOUNCE samples.
static void test_debounce() {
    printf("debounce\n");
    Sim s;
    s.wr(R_DEBOUNCE, 240); // 5 ms
    CHECK(s.rd(R_DEBOUNCE) == 240, "debounce register reads back %u", s.rd(R_DEBOUNCE));
    s.run_ticks(20);
    struct Step {
        int ticks;
        uint16_t keys;
    };
    // press, chatter, a 100-sample drop-out while held, a real release, a new press, release
    const Step steps[] = {{0, 1}, {3, 0}, {2, 1}, {400, 0}, {100, 1}, {300, 0}, {600, 1}, {50, 0}, {400, 0}};
    int64_t t_press1 = -1, t_release1 = -1, t_press2 = -1, t_release2 = -1;
    int index = 0;
    for (const Step& st : steps) {
        s.run_ticks((size_t)st.ticks);
        s.run_to_cycle(100);
        s.top->keys_async = st.keys;
        int64_t at = (int64_t)s.out.size();
        if (index == 0) t_press1 = at;
        if (index == 5) t_release1 = at;
        if (index == 6) t_press2 = at;
        if (index == 7) t_release2 = at;
        index++;
        s.sys_cycles(30);
    }
    struct Ev {
        int pressed;
        int64_t time;
    };
    std::vector<Ev> got;
    while (s.rd(R_STATUS) & 2) {
        uint32_t info = s.rd(R_EVENT);
        uint32_t time = s.rd(R_EVENT_TIME);
        CHECK((info & 15) == 0, "event for button %u", info & 15);
        got.push_back({(int)((info >> 4) & 1), (int64_t)time});
    }
    CHECK(got.size() == 4, "%zu events, expected press, release, press, release", got.size());
    if (got.size() == 4) {
        CHECK(got[0].pressed == 1 && got[0].time == t_press1, "first press at %lld, expected %lld",
              (long long)got[0].time, (long long)t_press1);
        CHECK(got[1].pressed == 0 && got[1].time >= t_release1 + 239 && got[1].time <= t_release1 + 241,
              "release at %lld, expected %lld + 240", (long long)got[1].time, (long long)t_release1);
        CHECK(got[2].pressed == 1 && got[2].time == t_press2, "second press at %lld, expected %lld",
              (long long)got[2].time, (long long)t_press2);
        CHECK(got[3].pressed == 0 && got[3].time >= t_release2 + 239 && got[3].time <= t_release2 + 241,
              "second release at %lld, expected %lld + 240", (long long)got[3].time, (long long)t_release2);
    }
    CHECK(s.rd(R_KEYS) == 0, "key word %x after the release", s.rd(R_KEYS));

    // the video enable bit is plain storage in the control register
    CHECK(s.top->video_on == 0, "video on after reset");
    s.wr(R_CTRL, 0x10);
    CHECK(s.top->video_on == 1 && (s.rd(R_CTRL) & 0x10), "video enable bit");
    delete s.top;
}

// ---- 2c. sprite layer against softovl ----------------------------------------------------------
static uint32_t rnd_state = 12345;
static uint32_t rnd() {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state >> 8;
}

static void ovl_upload_list(Sim& s, int page, const std::vector<uint32_t>& list) {
    s.wr(R_OVL_ADDR, 3u << 16 | (uint32_t)(page * 128));
    for (uint32_t e : list) {
        s.wr(R_OVL_DATA, e);
    }
}

// what the layer should put out for the picture, with the model's current sprites
static int ovl_mismatches(const Sim& s, const char* what) {
    static uint32_t line[OVL_W];
    int wrong = 0;
    for (int y = 0; y < OVL_H; y++) {
        ovl_line(y, line);
        for (int x = 0; x < OVL_W; x++) {
            uint32_t expect = ovl_mix888(line[x], Sim::picture(x, y));
            uint32_t got = s.vid_done[y * OVL_W + x];
            if (got != expect && wrong++ < 5) {
                printf("  %s: pixel %d,%d is %06x, expected %06x (layer entry %06x)\n", what, x, y, got, expect, line[x]);
            }
        }
    }
    return wrong;
}

static void test_sprites() {
    printf("sprite layer\n");
    Sim s;
    s.video = true;
    ovl_reset();
    ovl_budget(0); // the model draws everything; the hardware must, in the time it has

    // cells of the sizes the game uses and some odd ones, laid out back to back in the sheet
    struct Cell {
        int w, h;
        bool clip, twice;
        bool disc = false; // a solid round note whose rows end with the row end mark
    };
    // the last three are shown with every pixel doubled, the largest as tall as a Go-Go splash
    const Cell shapes[] = {{48, 48, true, false}, {48, 48, true, false}, {96, 96, false, false}, {48, 16, true, false},
                           {1, 48, true, false},  {32, 16, false, false}, {127, 3, false, false}, {5, 127, false, false},
                           {16, 24, false, false}, {48, 48, false, false}, {24, 96, false, true}, {7, 5, true, true},
                           {127, 2, false, true}, {48, 48, true, false, true}, {64, 60, true, false, true}};
    const int ncells = sizeof shapes / sizeof shapes[0];
    const int first_disc = ncells - 2;
    uint32_t addr = 0;
    std::vector<uint32_t> cell_addr;
    s.wr(R_OVL_ADDR, 2u << 16);
    for (int c = 0; c < ncells; c++) {
        addr += addr & 1; // cells start at even sheet addresses
        cell_addr.push_back(addr);
        ovl_cell(c, addr, shapes[c].w, shapes[c].h, shapes[c].clip, shapes[c].twice);
        s.wr(R_OVL_DATA, (uint32_t)shapes[c].twice << 31 | (uint32_t)shapes[c].clip << 30 | (uint32_t)shapes[c].h << 23 |
                             (uint32_t)shapes[c].w << 16 | addr >> 1);
        addr += (uint32_t)(shapes[c].w * shapes[c].h);
    }
    std::vector<uint8_t> pixels(addr);
    for (auto& p : pixels) {
        uint32_t r = rnd();
        p = (r & 3) == 0 ? 0 : (uint8_t)((r >> 4) % 31); // a quarter transparent; 31 would end the row
    }
    for (int c = first_disc; c < ncells; c++) { // an ellipse, every row: nothing, pixels, the end mark, nothing
        int w = shapes[c].w, h = shapes[c].h;
        for (int y = 0; y < h; y++) {
            double v = (y + 0.5 - h / 2.0) / (h / 2.0);
            int half = (int)(sqrt(1.0 - v * v) * w / 2.0);
            for (int x = 0; x < w; x++) {
                bool in = x >= w / 2 - half && x < w / 2 + half;
                uint8_t colour = (uint8_t)(1 + rnd() % 30);
                pixels[cell_addr[c] + (uint32_t)(y * w + x)] =
                    in ? (colour == 7 ? 8 : colour) : x == w / 2 + half ? OVL_ROW_END : 0; // 7 is the hole in every bank
            }
        }
    }
    ovl_sheet(0, pixels.data(), addr);
    s.wr(R_OVL_ADDR, 0u << 16);
    for (uint8_t p : pixels) {
        s.wr(R_OVL_DATA, p);
    }
    s.wr(R_OVL_ADDR, 1u << 16);
    for (int i = 0; i < 16 * 32; i++) {
        uint32_t alpha = (i % 32 == 7) ? 0 : (i / 32 < 8) ? 16 : 1 + rnd() % 16; // opaque banks, translucent banks, a hole
        uint32_t entry = alpha << 16 | (rnd() & 0xFFFF);
        ovl_palette(i / 32, i % 32, entry);
        s.wr(R_OVL_DATA, entry);
    }
    ovl_clip(56, 400);
    s.wr(R_OVL_CLIP, 400u << 16 | 56u);

    auto make_list = [&](int n, bool lane) {
        std::vector<uint32_t> list;
        for (int i = 0; i < n; i++) {
            int cell = lane ? (int)(rnd() % 2) : (int)(rnd() % ncells);
            int x = lane ? 40 + (int)(rnd() % 380) : (int)(rnd() % 560) - 120;
            int y = lane ? 152 : (int)(rnd() % 460) - 60;
            list.push_back((rnd() % 16) << 28 | (uint32_t)cell << 20 | ((uint32_t)y & 0x3FF) << 10 | ((uint32_t)x & 0x3FF));
        }
        return list;
    };

    // 1. sprites all over the screen, partly off it, overlapping
    std::vector<uint32_t> a = make_list(60, false);
    ovl_upload_list(s, 0, a);
    ovl_sprites(a.data(), (int)a.size());
    s.wr(R_OVL_CTRL, (uint32_t)a.size() << 8 | 1u);
    s.run_frames(3);
    CHECK(ovl_mismatches(s, "scattered") == 0, "sprites scattered over the screen differ from the model");
    int covered = 0, mixed = 0;
    for (int y = 0; y < OVL_H; y++) {
        for (int x = 0; x < OVL_W; x++) {
            uint32_t got = s.vid_done[y * OVL_W + x];
            covered += got != Sim::picture(x, y);
        }
    }
    {
        static uint32_t line[OVL_W];
        for (int y = 0; y < OVL_H; y++) {
            ovl_line(y, line);
            for (int x = 0; x < OVL_W; x++) {
                mixed += line[x] && (line[x] >> 16) < 16;
            }
        }
    }
    printf("  %d pixels changed by sprites, %d of them translucent\n", covered, mixed);
    CHECK(covered > 20000 && mixed > 3000, "the test picture has too few sprite pixels to mean anything");
    CHECK((s.rd(R_OVL_CTRL) >> 24) == 3, "sprite layer version reads %u", s.rd(R_OVL_CTRL) >> 24);

    // 1b. the end of the sheet: a cell in its last pixels (beyond 16 address bits)
    {
        const uint32_t end = 73728 - 48 * 48;
        std::vector<uint8_t> tail(48 * 48);
        for (auto& p : tail) {
            p = (uint8_t)(1 + rnd() % 31);
        }
        ovl_sheet(end, tail.data(), (uint32_t)tail.size());
        s.wr(R_OVL_ADDR, 0u << 16);
        // the address register takes 16 bits and counts on: skip forward by writing what is there
        std::vector<uint8_t> all(end);
        for (uint32_t i = 0; i < end; i++) {
            all[i] = i < pixels.size() ? pixels[i] : 0;
        }
        for (uint8_t p : all) {
            s.wr(R_OVL_DATA, p);
        }
        for (uint8_t p : tail) {
            s.wr(R_OVL_DATA, p);
        }
        ovl_cell(ncells, end, 48, 48, false, false);
        s.wr(R_OVL_ADDR, 2u << 16 | (uint32_t)ncells);
        s.wr(R_OVL_DATA, 48u << 23 | 48u << 16 | end >> 1);
        std::vector<uint32_t> one = {(uint32_t)ncells << 20 | 100u << 10 | 150u};
        ovl_upload_list(s, 1, one);
        ovl_sprites(one.data(), 1);
        s.wr(R_OVL_CTRL, 1u << 8 | 3u);
        s.run_frames(3);
        CHECK(ovl_mismatches(s, "end of sheet") == 0, "a cell at the end of the sheet differs from the model");
        ovl_upload_list(s, 0, a);
        ovl_sprites(a.data(), (int)a.size());
        s.wr(R_OVL_CTRL, (uint32_t)a.size() << 8 | 1u);
        s.run_frames(2);
    }

    // 2. a new list in the other page takes effect at the next frame, not in the middle of this one
    std::vector<uint32_t> b = make_list(34, true); // a full lane: 34 notes on the same 48 rows
    ovl_upload_list(s, 1, b);
    while (s.vid_v != 100) {
        s.step();
    }
    s.wr(R_OVL_CTRL, (uint32_t)b.size() << 8 | 3u);
    s.run_frames(1);
    CHECK(ovl_mismatches(s, "before the switch") == 0, "the frame in which the list was switched changed");
    ovl_sprites(b.data(), (int)b.size());
    s.run_frames(2);
    CHECK(ovl_mismatches(s, "full lane") == 0, "a lane full of notes differs from the model");
    covered = 0;
    for (int x = 0; x < OVL_W; x++) {
        covered += s.vid_done[170 * OVL_W + x] != Sim::picture(x, 170);
    }
    printf("  full lane: %d of 400 pixels of a lane row are sprite pixels\n", covered);
    CHECK(covered > 200, "the full lane has too few sprite pixels");

    // 2b. far more notes than a line has time to draw pixel by pixel: piled up a few pixels
    // apart, with bar lines between them. Each costs its visible sliver. The last two
    // cases: so far apart that the narrow top and bottom rows do not touch; and a pile
    // that is leaving through the left edge of the clip window.
    struct Pile {
        int count, first_x, spacing;
        bool many;
    };
    const Pile piles[] = {{124, 30, 2, true}, {120, 40, 3, true}, {41, 30, 9, false}, {110, -60, 2, true}};
    for (const Pile& pl : piles) {
        std::vector<uint32_t> pile;
        for (int i = 0; i < pl.count; i++) {
            int big = rnd() % 5 == 0;
            int x = pl.first_x + i * pl.spacing + (int)(rnd() % 2), y = big ? 146 : 152;
            pile.push_back((rnd() % 16) << 28 | (uint32_t)(first_disc + big) << 20 | (uint32_t)y << 10 | ((uint32_t)x & 0x3FF));
            if (i % 31 == 30) {
                pile.push_back(4u << 20 | 152u << 10 | ((uint32_t)(x + 20) & 0x3FF)); // the 1 x 48 cell
            }
        }
        ovl_upload_list(s, 0, pile);
        ovl_sprites(pile.data(), (int)pile.size());
        s.wr(R_OVL_CTRL, (uint32_t)pile.size() << 8 | 1u);
        s.run_frames(3);
        CHECK(ovl_mismatches(s, "pile") == 0, "%zu notes piled up %d px apart from x = %d differ from the model",
              pile.size(), pl.spacing, pl.first_x);
        static uint32_t line[OVL_W];
        ovl_peak();
        int plain = 0;
        for (int y = 140; y < 210; y++) {
            ovl_line(y, line);
        }
        for (uint32_t e : pile) {
            plain += 3 + shapes[(e >> 20) & 255].w;
        }
        int needs = ovl_peak();
        printf("  pile from x = %d, %d px apart: %zu sprites, the busiest line needs %d of %d cycles (pixel by pixel: %d)\n",
               pl.first_x, pl.spacing, pile.size(), needs, OVL_LINE_CYCLES, plain);
        CHECK(needs < OVL_LINE_CYCLES && (!pl.many || plain > 2 * OVL_LINE_CYCLES), "the pile does not test the short cuts");
    }

    // 3. switched off: the picture passes unchanged
    ovl_sprites(nullptr, 0);
    s.wr(R_OVL_CTRL, 0);
    s.run_frames(2);
    CHECK(ovl_mismatches(s, "off") == 0, "the picture is changed with the layer off");
    delete s.top;
}

// ---- 2d. frames of the real game ---------------------------------------------------------------
// TAIKO_OVL_STATES names a directory of states saved with ovl_state_save() (the game's own
// sheet, palettes, cells and a frame's sprite list). Each is shown by the hardware and
// compared with the model. Not part of the plain run: it needs a library with a skin.
static void test_real_frames(const char* dir) {
    printf("sprite layer, frames of the game from %s\n", dir);
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (e.path().extension() == ".ovl") {
            files.push_back(e.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    Sim s;
    s.video = true;
    ovl_budget(0);
    int page = 0, shown = 0;
    for (const std::string& file : files) {
        if (!ovl_state_load(file.c_str())) {
            printf("  cannot read %s\n", file.c_str());
            continue;
        }
        if (shown == 0) { // the same in every state of one run of the game
            const uint8_t* sheet = ovl_state_sheet();
            s.wr(R_OVL_ADDR, 0u << 16);
            for (uint32_t i = 0; i < 73728; i++) {
                s.wr(R_OVL_DATA, sheet[i]);
            }
            s.wr(R_OVL_ADDR, 1u << 16);
            for (int i = 0; i < 16 * 32; i++) {
                s.wr(R_OVL_DATA, ovl_state_palette()[i]);
            }
            s.wr(R_OVL_ADDR, 2u << 16);
            for (int c = 0; c < 256; c++) {
                uint32_t addr;
                int w, h;
                bool clip, twice;
                ovl_state_cell(c, &addr, &w, &h, &clip, &twice);
                s.wr(R_OVL_DATA, (uint32_t)twice << 31 | (uint32_t)clip << 30 | (uint32_t)h << 23 | (uint32_t)w << 16 | addr >> 1);
            }
            int x0, x1;
            ovl_state_clip(&x0, &x1);
            s.wr(R_OVL_CLIP, (uint32_t)x1 << 16 | (uint32_t)x0);
        }
        uint32_t addr;
        int w, h;
        bool clip, twice;
        int count = ovl_state_cell(0, &addr, &w, &h, &clip, &twice);
        std::vector<uint32_t> list(ovl_state_sprites(), ovl_state_sprites() + count);
        ovl_upload_list(s, page, list);
        s.wr(R_OVL_CTRL, (uint32_t)count << 8 | (uint32_t)page << 1 | 1u);
        s.run_frames(3);
        int wrong = ovl_mismatches(s, file.c_str());
        CHECK(wrong == 0, "%s: %d pixels differ from the model (%d sprites)", file.c_str(), wrong, count);
        page ^= 1;
        shown++;
    }
    printf("  %d frames compared\n", shown);
    delete s.top;
}

// ---- 3. music streaming against softmix ---------------------------------------------------------
struct MusicCase {
    const char* name;
    int codec, channels, block_bytes, block_samples;
    int blocks;       // length of the song in blocks
    int ring_blocks;  // ring size in blocks (smaller than the song: it wraps)
    int64_t origin;   // song sample at ring byte 0
    int64_t start;    // song sample to start from (negative: lead-in)
    int gain, vol;
    int starve_at;    // stop feeding at this tick for a while (0 = never)
};

static void test_music(const MusicCase& c) {
    printf("music: %s\n", c.name);
    Sim s;
    std::vector<uint8_t> song((size_t)c.blocks * c.block_bytes);
    for (auto& b : song) {
        b = (uint8_t)rng();
    }
    if (c.codec == PF_CODEC_IMA_ADPCM) {
        for (int b = 0; b < c.blocks; b++) {
            for (int ch = 0; ch < c.channels; ch++) { // valid headers: step index 0..88, reserved byte 0
                song[(size_t)b * c.block_bytes + ch * 4 + 2] = (uint8_t)(rng() % 89);
                song[(size_t)b * c.block_bytes + ch * 4 + 3] = 0;
            }
        }
    }
    uint32_t ring_bytes = (uint32_t)c.ring_blocks * c.block_bytes;
    pf_music_fmt fmt = {(uint8_t)c.codec, (uint8_t)c.channels, (uint16_t)c.block_bytes, (uint16_t)c.block_samples};

    // gateware
    s.wr(R_CTRL, 0);
    s.wr(R_RING_BASE, RING_BASE);
    s.wr(R_RING_BYTES, ring_bytes);
    s.wr(R_FORMAT, (c.codec == PF_CODEC_IMA_ADPCM ? 1 : 0) | (c.channels == 2 ? 2 : 0) | ((c.block_bytes / 4) << 16));
    s.wr(R_GAIN, c.gain);
    s.wr(R_VOL_MUSIC, c.vol);
    s.wr(R_POS_LO, (uint32_t)c.start);
    s.wr(R_POS_HI, (uint32_t)(c.start >> 32) & 0xFFFF);
    s.wr(R_LEAD, c.start < c.origin ? (uint32_t)(c.origin - c.start) : 0);
    s.wr(R_SKIP, c.start > c.origin ? (uint32_t)(c.start - c.origin) : 0);
    s.wr(R_FED, 0);
    s.wr(R_CMD, CMD_BEGIN);
    s.wr(R_CTRL, CTRL_STREAM);
    // software model
    sm_init(sm_ring, sizeof sm_ring);
    sm_volume(c.vol, 256);
    sm_music_begin(&fmt, ring_bytes, c.origin, c.start, c.gain);

    uint32_t hw_fed = 0, sm_fed = 0;
    std::map<int64_t, Sample> hw, sw;
    bool hw_finished = false, sm_finished = false;
    size_t base = s.out.size();
    int total_ticks = c.blocks * c.block_samples + 900;
    int64_t sm_last_pos = c.start;
    for (int t = 0; t < total_ticks; t++) {
        bool feeding = !(c.starve_at && t >= c.starve_at && t < c.starve_at + 300);
        // each model is fed whenever its own ring has room, as the game does
        if (feeding && hw_fed < song.size() && hw_fed - s.rd(R_CONSUMED) + c.block_bytes <= ring_bytes) {
            s.ring_write(hw_fed % ring_bytes, &song[hw_fed], c.block_bytes);
            hw_fed += c.block_bytes;
            s.wr(R_FED, hw_fed);
        }
        if (!hw_finished && hw_fed == song.size()) {
            s.wr(R_CTRL, CTRL_STREAM | CTRL_RUN | CTRL_FINISHED);
            hw_finished = true;
        }
        if (feeding && sm_fed < song.size() && sm_fed - sm_music_consumed() + c.block_bytes <= ring_bytes) {
            memcpy(sm_ring + sm_fed % ring_bytes, &song[sm_fed], c.block_bytes);
            sm_fed += c.block_bytes;
            sm_music_feed(sm_fed);
        }
        if (!sm_finished && sm_fed == song.size()) {
            sm_music_finish();
            sm_finished = true;
        }
        if (t == 3) {
            s.wr(R_CTRL, CTRL_STREAM | CTRL_RUN | (hw_finished ? CTRL_FINISHED : 0));
            sm_music_run(true);
        }
        s.run_ticks(1);
        int16_t frame[2];
        int64_t before = sm_music_pos();
        sm_render(frame, 1);
        if (sm_music_pos() == before + 1) {
            sw[before] = {frame[0], frame[1], before};
        }
        sm_last_pos = sm_music_pos();
    }
    // gateware: the sample at tick n+1 is the one computed in period n, for the position at tick n
    for (size_t n = base; n + 1 < s.out.size(); n++) {
        if (s.out[n + 1].pos == s.out[n].pos + 1) {
            hw[s.out[n].pos] = s.out[n + 1];
        } else {
            CHECK(s.out[n + 1].pos == s.out[n].pos, "position jumped from %lld to %lld", (long long)s.out[n].pos,
                  (long long)s.out[n + 1].pos);
        }
    }
    int differ = 0, compared = 0;
    for (auto& [p, expect] : sw) {
        auto it = hw.find(p);
        if (it == hw.end()) {
            continue;
        }
        compared++;
        if (it->second.l != expect.l || it->second.r != expect.r) {
            if (differ++ < 5) {
                printf("  position %lld: gateware %d,%d softmix %d,%d\n", (long long)p, it->second.l, it->second.r,
                       expect.l, expect.r);
            }
        }
    }
    int64_t song_samples = c.origin + (int64_t)c.blocks * c.block_samples;
    CHECK(differ == 0, "%d of %d positions differ from softmix", differ, compared);
    CHECK(compared > (int)(song_samples - c.start) - 50, "only %d positions compared, song has %lld", compared,
          (long long)(song_samples - c.start));
    CHECK(hw.rbegin()->first > song_samples, "position stopped at %lld, before the end of the song (%lld)",
          (long long)hw.rbegin()->first, (long long)song_samples);
    if (c.starve_at) {
        bool held = false;
        for (size_t n = base; n + 1 < s.out.size(); n++) {
            held = held || s.out[n + 1].pos == s.out[n].pos;
        }
        CHECK(held, "the position never waited although the ring ran dry");
    }
    // the clock registers: counter and position latched together, at most a sample or two behind
    int64_t counter, position;
    s.latch_clock(&counter, &position);
    CHECK(llabs(counter - (int64_t)s.out.size()) <= 2, "latched counter %lld, ticks so far %zu", (long long)counter,
          s.out.size());
    CHECK(llabs(position - s.out.back().pos) <= 2, "latched position %lld, actual %lld", (long long)position,
          (long long)s.out.back().pos);
    (void)sm_last_pos;
    delete s.top;
}

// ---- 4. the stock path: the CPU writes samples, playback_en plays them ---------------------------
static void test_cpu_path() {
    printf("CPU-fed music (stock audio path)\n");
    Sim s;
    for (int i = 0; i < 100; i++) {
        while (s.top->clk_sys) {
            s.step();
        }
        s.top->cpu_audio_data = ((uint32_t)(uint16_t)(1000 + i) << 16) | (uint16_t)(-1000 - i);
        s.top->cpu_audio_wr = 1;
        s.sys_cycles(1);
        s.top->cpu_audio_wr = 0;
        s.sys_cycles(1);
    }
    s.run_ticks(5);
    CHECK(s.out.back().l == 0, "sound before playback was enabled");
    s.top->cpu_playback_en = 1;
    s.run_ticks(110);
    int found = -1;
    for (size_t n = 0; n < s.out.size(); n++) {
        if (s.out[n].l == 1000) {
            found = (int)n;
            break;
        }
    }
    CHECK(found >= 0, "the first CPU sample never came out");
    bool ok = found >= 0;
    for (int i = 0; ok && i < 100; i++) {
        ok = s.out[found + i].l == 1000 + i && s.out[found + i].r == -1000 - i;
    }
    CHECK(ok, "CPU samples not passed through unchanged, in order");
    CHECK(s.out.back().l == 0, "not silent after the FIFO emptied");
    CHECK(s.rd(R_ID) == 0x54504B31, "id register");
    s.wr(R_BRIDGE_HI, 0x1234);
    CHECK(s.rd(R_BRIDGE_HI) == 0x1234 && s.top->bridge_offset_hi == 0x1234, "bridge offset register");
    // frame counter from the vertical blank
    uint32_t v0 = s.rd(R_VIDEO) >> 1;
    for (int i = 0; i < 3; i++) {
        s.top->vblank_async = 1;
        s.sys_cycles(20);
        CHECK((s.rd(R_VIDEO) & 1) == 1, "vblank bit");
        s.top->vblank_async = 0;
        s.sys_cycles(20);
    }
    CHECK((s.rd(R_VIDEO) >> 1) == v0 + 3, "frame counter");
    delete s.top;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    make_hits();
    test_hits();
    test_input();
    test_debounce();
    test_sprites();
    if (getenv("TAIKO_OVL_STATES")) {
        test_real_frames(getenv("TAIKO_OVL_STATES"));
        printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }
    test_music({"ADPCM stereo, lead-in, ring wraps", PF_CODEC_IMA_ADPCM, 2, 2048, 2041, 6, 3, 0, -700, 256, 256, 0});
    test_music({"ADPCM mono, starts mid-block (preview), gain and volume", PF_CODEC_IMA_ADPCM, 1, 1024, 2041, 5, 2,
                2041 * 7, 2041 * 7 + 1234, 180, 300, 0});
    test_music({"ADPCM stereo, ring runs dry mid-song", PF_CODEC_IMA_ADPCM, 2, 2048, 2041, 5, 2, 0, 0, 256, 256, 3000});
    test_music({"PCM stereo", PF_CODEC_PCM16, 2, 4, 1, 3000, 1024, 0, -100, 256, 200, 0});
    test_music({"PCM mono", PF_CODEC_PCM16, 1, 2, 1, 4000, 2048, 500, 600, 256, 256, 0});
    test_cpu_path();
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
