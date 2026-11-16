#include "softovl.h"

#include <string.h>

#define SHEET 131072 /* address space; the hardware has 73728 pixels of it */
#define CELLS 256
#define SPRITES 128
#define BANKS 16

static uint8_t sheet[SHEET];
static uint32_t palette[BANKS * 32];
static struct {
    uint32_t addr;
    uint8_t w, h;
    bool clip, twice;
} cells[CELLS];
static uint32_t sprites[SPRITES];
static int sprite_count;
static int clip_x0, clip_x1 = OVL_W;
static int line_cycles = OVL_LINE_CYCLES; /* 0: no limit */
static int peak_cycles;

void ovl_reset(void) {
    memset(sheet, 0, sizeof sheet);
    memset(palette, 0, sizeof palette);
    memset(cells, 0, sizeof cells);
    sprite_count = 0;
    clip_x0 = 0;
    clip_x1 = OVL_W;
}

void ovl_sheet(uint32_t addr, const uint8_t *pixels, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        sheet[(addr + i) & (SHEET - 1)] = pixels[i] & 31;
    }
}

void ovl_palette(int bank, int index, uint32_t entry) { palette[(bank & 15) * 32 + (index & 31)] = entry & 0x1FFFFF; }

void ovl_cell(int cell, uint32_t addr, int w, int h, bool clip, bool twice) {
    cells[cell & 255].twice = twice;
    cells[cell & 255].addr = addr & (SHEET - 1) & ~1u; /* cells start at even addresses */
    cells[cell & 255].w = (uint8_t)(w & 127);
    cells[cell & 255].h = (uint8_t)(h & 127);
    cells[cell & 255].clip = clip;
}

void ovl_clip(int x0, int x1) {
    clip_x0 = x0;
    clip_x1 = x1;
}

void ovl_sprites(const uint32_t *list, int count) {
    sprite_count = count > SPRITES ? SPRITES : count < 0 ? 0 : count;
    memcpy(sprites, list, (size_t)sprite_count * sizeof *list);
}

void ovl_budget(int cycles) { line_cycles = cycles; }

/* The whole state as one block, for carrying a frame of the real game into the gateware
 * test bench: sheet, palette, cells, clip window, sprite list. */
#include <stdio.h>
bool ovl_state_save(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return false;
    }
    int32_t tail[3] = {clip_x0, clip_x1, sprite_count};
    bool ok = fwrite(sheet, 1, sizeof sheet, f) == sizeof sheet && fwrite(palette, 1, sizeof palette, f) == sizeof palette &&
              fwrite(cells, 1, sizeof cells, f) == sizeof cells && fwrite(tail, 1, sizeof tail, f) == sizeof tail &&
              fwrite(sprites, 1, sizeof sprites, f) == sizeof sprites;
    fclose(f);
    return ok;
}

bool ovl_state_load(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    int32_t tail[3] = {0, 0, 0};
    bool ok = fread(sheet, 1, sizeof sheet, f) == sizeof sheet && fread(palette, 1, sizeof palette, f) == sizeof palette &&
              fread(cells, 1, sizeof cells, f) == sizeof cells && fread(tail, 1, sizeof tail, f) == sizeof tail &&
              fread(sprites, 1, sizeof sprites, f) == sizeof sprites;
    fclose(f);
    clip_x0 = tail[0];
    clip_x1 = tail[1];
    sprite_count = ok ? tail[2] : 0;
    return ok;
}

int ovl_state_cell(int cell, uint32_t *addr, int *w, int *h, bool *clip, bool *twice) {
    *addr = cells[cell & 255].addr;
    *w = cells[cell & 255].w;
    *h = cells[cell & 255].h;
    *clip = cells[cell & 255].clip;
    *twice = cells[cell & 255].twice;
    return sprite_count;
}
const uint8_t *ovl_state_sheet(void) { return sheet; }
const uint32_t *ovl_state_palette(void) { return palette; }
const uint32_t *ovl_state_sprites(void) { return sprites; }
void ovl_state_clip(int *x0, int *x1) {
    *x0 = clip_x0;
    *x1 = clip_x1;
}

int ovl_peak(void) {
    int peak = peak_cycles;
    peak_cycles = 0;
    return peak;
}

static int sign10(uint32_t v) { return (int)(v & 0x3FF) - ((v & 0x200) ? 0x400 : 0); }

void ovl_line(int y, uint32_t out[OVL_W]) {
    memset(out, 0, OVL_W * sizeof *out);
    /* The hardware has a fixed time per line: three cycles for every sprite of the list
     * and one for every pixel it looks at. When the time is up the sprite being drawn is
     * cut off and the ones behind it are missing. It does not look at pixels right of a
     * row's end mark or of the screen's edge, and it steps over `run` in one cycle: a
     * stretch of the line that is drawn without a gap. (The hardware learns of a drawn
     * pixel two cycles later than this model, so its count can differ by a few cycles;
     * the picture is the same as long as the time is not used up.) */
    int used = 0, run_lo = 0, run_hi = 0;
    for (int i = 0; i < sprite_count; i++) { /* front to back: the first pixel drawn stays */
        uint32_t s = sprites[i];
        int x = sign10(s), sy = sign10(s >> 10), cell = (s >> 20) & 255, bank = s >> 28;
        int dy = y - sy;
        int twice = cells[cell].twice ? 1 : 0;
        used += 3;
        if (dy < 0 || dy >= cells[cell].h << twice || cells[cell].w == 0) {
            continue;
        }
        uint32_t row = cells[cell].addr + (uint32_t)(dy >> twice) * cells[cell].w;
        int width = cells[cell].w << twice;
        int from = cells[cell].clip && clip_x0 > 0 ? clip_x0 : 0;
        int to = cells[cell].clip && clip_x1 < OVL_W ? clip_x1 : OVL_W;
        for (int k = 0; k < width; k++) {
            int px = x + k;
            used++;
            if (px >= to) {
                break; /* cannot be seen from here on */
            }
            if (!twice && px < from) {
                k += from - px - 1; /* nor up to here: over it in one cycle */
                continue;
            }
            if (!twice && px >= run_lo && px < run_hi) {
                k += run_hi - px - 1; /* drawn already: the same */
                continue;
            }
            if (line_cycles && used > line_cycles) {
                break;
            }
            uint8_t index = sheet[(row + (uint32_t)(k >> twice)) & (SHEET - 1)];
            if (index == OVL_ROW_END) {
                used++;
                break;
            }
            if (px < 0) {
                continue;
            }
            if (index && !out[px] && !(cells[cell].clip && (px < clip_x0 || px >= clip_x1))) {
                uint32_t entry = palette[bank * 32 + index];
                if (entry >> 16) { /* alpha 0 draws nothing and leaves the pixel free */
                    out[px] = entry;
                }
            }
            if (out[px]) {
                if (px == run_hi) {
                    run_hi = px + 1;
                } else if (px < run_lo || px > run_hi) {
                    run_lo = px;
                    run_hi = px + 1;
                }
            }
        }
    }
    if (used > peak_cycles) {
        peak_cycles = used;
    }
}

uint32_t ovl_mix888(uint32_t entry, uint32_t rgb) {
    uint32_t alpha = entry >> 16;
    if (!alpha) {
        return rgb;
    }
    uint32_t keep = 16 - (alpha > 16 ? 16 : alpha);
    uint32_t c = entry & 0xFFFF;
    /* RGB565 to 8 bits as the hardware does for sprites and picture alike */
    uint32_t s[3] = {((c >> 11) * 527 + 23) >> 6, (((c >> 5) & 63) * 259 + 33) >> 6, ((c & 31) * 527 + 23) >> 6};
    uint32_t out = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t p = (rgb >> (16 - 8 * i)) & 255;
        uint32_t v = s[i] + ((p * keep) >> 4);
        out = out << 8 | (v > 255 ? 255 : v);
    }
    return out;
}

void ovl_compose(uint16_t *frame) {
    static uint32_t line[OVL_W];
    if (!sprite_count) {
        return;
    }
    for (int y = 0; y < OVL_H; y++) {
        ovl_line(y, line);
        uint16_t *row = frame + y * OVL_W;
        for (int x = 0; x < OVL_W; x++) {
            if (line[x]) {
                uint32_t c = row[x];
                uint32_t rgb = (((c >> 11) * 527 + 23) >> 6) << 16 | ((((c >> 5) & 63) * 259 + 33) >> 6) << 8 |
                               (((c & 31) * 527 + 23) >> 6);
                uint32_t m = ovl_mix888(line[x], rgb);
                row[x] = (uint16_t)(((m >> 19) & 31) << 11 | ((m >> 10) & 63) << 5 | ((m >> 3) & 31));
            }
        }
    }
}

void ovl_fade(uint16_t *frame, int level) {
    if (level >= 16) {
        return;
    }
    level = level < 0 ? 0 : level;
    for (int i = 0; i < OVL_W * OVL_H; i++) {
        uint32_t c = frame[i];
        frame[i] = (uint16_t)(((c >> 11) * (uint32_t)level >> 4) << 11 | (((c >> 5) & 63) * (uint32_t)level >> 4) << 5 |
                              ((c & 31) * (uint32_t)level >> 4));
    }
}
