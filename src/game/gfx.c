#include "gfx.h"

#include <string.h>

#include "assets.h"
#include "font_builtin.h"
#include "library.h"
#include "prof.h"

#define DIRTY_MAX 48

typedef struct {
    int16_t x0, y0, x1, y1; /* x1, y1 exclusive */
} rect;

typedef struct {
    uint16_t w, h, frames, flags;
    uint32_t table;
} sprite_entry;

typedef struct {
    uint16_t px, line_height;
    int16_t ascent, descent;
    uint32_t glyph_count, glyphs, bitmaps, reserved;
} font_entry;

typedef struct {
    uint32_t cp, bitmap;
    uint8_t w, h;
    int8_t xoff, yoff;
    uint8_t advance, pad[3];
} glyph_entry;

static uint16_t bg[GFX_W * GFX_H];
static uint16_t *fb, *target;
static uint16_t *buffers[2];
static int buf_index;
static struct {
    rect r[DIRTY_MAX];
    int n;
    bool full;
} dirty[2];
static int cx0, cy0, cx1 = GFX_W, cy1 = GFX_H;

/* a rectangle of the background layer that is shown from other pixels (gfx_bg_strip) */
static struct {
    const uint16_t *pixels; /* w * h, NULL: none */
    int x, y, w, h;
} strip;

static const uint8_t *spr_base;
static const sprite_entry *sprites;
static uint32_t sprite_count;
static const uint8_t *font_base;
static const font_entry *fonts;
static uint32_t font_count;

static inline uint16_t blend565(uint32_t fg, uint32_t dst, uint32_t a) {
    dst = (dst | (dst << 16)) & 0x07E0F81F;
    fg = (fg | (fg << 16)) & 0x07E0F81F;
    uint32_t r = ((((fg - dst) * a) >> 5) + dst) & 0x07E0F81F;
    return (uint16_t)(r | (r >> 16));
}

void gfx_init(void) {
    uint32_t bytes;
    spr_base = lib_section(LIB_SEC_SPRITES, &bytes);
    sprite_count = bytes >= 4 ? *(const uint32_t *)spr_base : 0;
    sprites = (const sprite_entry *)(spr_base + 4);
    font_base = lib_section(LIB_SEC_FONTS, &bytes);
    font_count = bytes >= 4 ? *(const uint32_t *)font_base : 0;
    fonts = (const font_entry *)(font_base + 4);
}

/* ---------- frame and dirty rectangles ---------- */

static void mark(int x0, int y0, int x1, int y1) {
    if (target != fb || x1 <= x0 || y1 <= y0 || dirty[buf_index].full) {
        return;
    }
    rect *r = dirty[buf_index].r;
    int n = dirty[buf_index].n;
    for (int i = 0; i < n; i++) {
        /* merge when the union wastes little: overlapping or neighbouring rectangles */
        int ux0 = x0 < r[i].x0 ? x0 : r[i].x0, uy0 = y0 < r[i].y0 ? y0 : r[i].y0;
        int ux1 = x1 > r[i].x1 ? x1 : r[i].x1, uy1 = y1 > r[i].y1 ? y1 : r[i].y1;
        int ua = (ux1 - ux0) * (uy1 - uy0);
        int sum = (x1 - x0) * (y1 - y0) + (r[i].x1 - r[i].x0) * (r[i].y1 - r[i].y0);
        if (ua <= sum + (sum >> 4) + 32) {
            r[i].x0 = (int16_t)ux0;
            r[i].y0 = (int16_t)uy0;
            r[i].x1 = (int16_t)ux1;
            r[i].y1 = (int16_t)uy1;
            return;
        }
    }
    if (n >= DIRTY_MAX) {
        dirty[buf_index].full = true;
        return;
    }
    r[n].x0 = (int16_t)x0;
    r[n].y0 = (int16_t)y0;
    r[n].x1 = (int16_t)x1;
    r[n].y1 = (int16_t)y1;
    dirty[buf_index].n = n + 1;
}

/* restores one row segment of the frame from the background layer, or from the strip
 * where that covers it */
static void repair_row(int y, int x0, int x1) {
    uint16_t *dst = fb + y * GFX_W;
    const uint16_t *src = bg + y * GFX_W;
    if (strip.pixels && y >= strip.y && y < strip.y + strip.h && x1 > strip.x && x0 < strip.x + strip.w) {
        int s0 = x0 < strip.x ? strip.x : x0, s1 = x1 > strip.x + strip.w ? strip.x + strip.w : x1;
        if (s0 > x0) {
            memcpy(dst + x0, src + x0, (size_t)(s0 - x0) * 2);
        }
        memcpy(dst + s0, strip.pixels + (y - strip.y) * strip.w + (s0 - strip.x), (size_t)(s1 - s0) * 2);
        if (x1 > s1) {
            memcpy(dst + s1, src + s1, (size_t)(x1 - s1) * 2);
        }
        return;
    }
    memcpy(dst + x0, src + x0, (size_t)(x1 - x0) * 2);
}

void gfx_begin(void) {
    fb = pf_video_buffer();
    if (!buffers[0]) {
        buffers[0] = fb;
        dirty[0].full = dirty[1].full = true;
    } else if (fb != buffers[0] && !buffers[1]) {
        buffers[1] = fb;
    }
    buf_index = fb == buffers[0] ? 0 : 1;
    if (dirty[buf_index].full) {
        memcpy(fb, bg, sizeof bg);
        if (strip.pixels) {
            for (int y = strip.y; y < strip.y + strip.h; y++) {
                repair_row(y, strip.x, strip.x + strip.w);
            }
        }
        prof_counter[PROF_PX_REPAIR] += GFX_W * GFX_H;
    } else {
        for (int i = 0; i < dirty[buf_index].n; i++) {
            const rect *r = &dirty[buf_index].r[i];
            prof_counter[PROF_PX_REPAIR] += (uint32_t)((r->x1 - r->x0) * (r->y1 - r->y0));
            for (int y = r->y0; y < r->y1; y++) {
                repair_row(y, r->x0, r->x1);
            }
        }
    }
    dirty[buf_index].n = 0;
    dirty[buf_index].full = false;
    target = fb;
    gfx_clip_reset();
}

void gfx_target(int t) {
    if (!fb) {
        fb = pf_video_buffer();
    }
    target = t == GFX_BG ? bg : fb;
}

static uint32_t bg_generation;

void gfx_bg_changed(void) {
    dirty[0].full = dirty[1].full = true;
    bg_generation++;
}

uint32_t gfx_bg_generation(void) { return bg_generation; }
void gfx_bg_touched(void) { bg_generation++; }

void gfx_bg_changed_rect(int x, int y, int w, int h) {
    uint16_t *saved_target = target;
    int saved_index = buf_index;
    target = fb;
    for (buf_index = 0; buf_index < 2; buf_index++) {
        mark(x, y, x + w, y + h);
    }
    buf_index = saved_index;
    target = saved_target;
}

void gfx_bg_strip(int x, int y, int w, int h, const uint16_t *pixels) {
    if (pixels == strip.pixels && (!pixels || (x == strip.x && y == strip.y && w == strip.w && h == strip.h))) {
        return;
    }
    if (strip.pixels) {
        gfx_bg_changed_rect(strip.x, strip.y, strip.w, strip.h);
    }
    strip.pixels = pixels;
    strip.x = x;
    strip.y = y;
    strip.w = w;
    strip.h = h;
    if (pixels) {
        gfx_bg_changed_rect(x, y, w, h);
    }
}

void gfx_bg_read(int x, int y, int w, int h, uint16_t *out) {
    for (int j = 0; j < h; j++) {
        memcpy(out + j * w, bg + (y + j) * GFX_W + x, (size_t)w * 2);
    }
}

void gfx_bg_write(int x, int y, int w, int h, const uint16_t *in) {
    for (int j = 0; j < h; j++) {
        memcpy(bg + (y + j) * GFX_W + x, in + j * w, (size_t)w * 2);
    }
}

void gfx_clip(int x, int y, int w, int h) {
    cx0 = x < 0 ? 0 : x;
    cy0 = y < 0 ? 0 : y;
    cx1 = x + w > GFX_W ? GFX_W : x + w;
    cy1 = y + h > GFX_H ? GFX_H : y + h;
    if (cx1 < cx0) { /* an empty clip draws nothing */
        cx1 = cx0;
    }
    if (cy1 < cy0) {
        cy1 = cy0;
    }
}

void gfx_clip_reset(void) {
    cx0 = cy0 = 0;
    cx1 = GFX_W;
    cy1 = GFX_H;
}

/* clips a rectangle; false when nothing is left */
static bool clip_rect(int *x, int *y, int *w, int *h) {
    int x0 = *x < cx0 ? cx0 : *x, y0 = *y < cy0 ? cy0 : *y;
    int x1 = *x + *w > cx1 ? cx1 : *x + *w, y1 = *y + *h > cy1 ? cy1 : *y + *h;
    if (x1 <= x0 || y1 <= y0) {
        return false;
    }
    *x = x0;
    *y = y0;
    *w = x1 - x0;
    *h = y1 - y0;
    return true;
}

void gfx_fill(int x, int y, int w, int h, uint16_t color) {
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    mark(x, y, x + w, y + h);
    prof_counter[PROF_PX_COPY] += (uint32_t)(w * h);
    for (int j = 0; j < h; j++) {
        uint16_t *p = target + (y + j) * GFX_W + x;
        for (int i = 0; i < w; i++) {
            p[i] = color;
        }
    }
}

void gfx_blend(int x, int y, int w, int h, uint16_t color, int alpha) {
    if (alpha <= 0 || !clip_rect(&x, &y, &w, &h)) {
        return;
    }
    if (alpha >= 32) {
        gfx_fill(x, y, w, h, color);
        return;
    }
    mark(x, y, x + w, y + h);
    prof_counter[PROF_PX_BLEND] += (uint32_t)(w * h);
    for (int j = 0; j < h; j++) {
        uint16_t *p = target + (y + j) * GFX_W + x;
        for (int i = 0; i < w; i++) {
            p[i] = blend565(color, p[i], (uint32_t)alpha);
        }
    }
}

void gfx_frame_rect(int x, int y, int w, int h, uint16_t color) {
    gfx_fill(x, y, w, 1, color);
    gfx_fill(x, y + h - 1, w, 1, color);
    gfx_fill(x, y, 1, h, color);
    gfx_fill(x + w - 1, y, 1, h, color);
}

/* ---------- sprites ---------- */

int gfx_sprite_frames(int id) { return (uint32_t)id < sprite_count ? sprites[id].frames : 0; }
int gfx_sprite_w(int id) { return (uint32_t)id < sprite_count ? sprites[id].w : 0; }
int gfx_sprite_h(int id) { return (uint32_t)id < sprite_count ? sprites[id].h : 0; }

/* one row of a sprite: `p` is its command stream, `dst` the start of the target row */
static void blit_row(const uint16_t *p, uint16_t *dst, int px, int alpha) {
    for (;;) {
        uint16_t cmd = *p++;
        int kind = cmd >> 14, count = cmd & 0x3FFF;
        if (kind == 3) {
            return;
        }
        if (kind == 0) {
            px += count;
            continue;
        }
        int a = px < cx0 ? cx0 - px : 0;            /* first visible pixel of the run */
        int b = px + count > cx1 ? cx1 - px : count; /* one past the last */
        if (kind == 1) {
            if (b > a) {
                if (alpha >= 32) {
                    memcpy(dst + px + a, p + a, (size_t)(b - a) * 2);
                    prof_counter[PROF_PX_COPY] += (uint32_t)(b - a);
                } else {
                    prof_counter[PROF_PX_BLEND] += (uint32_t)(b - a);
                    for (int i = a; i < b; i++) {
                        dst[px + i] = blend565(p[i], dst[px + i], (uint32_t)alpha);
                    }
                }
            }
            p += count;
        } else {
            if (b > a) {
                prof_counter[PROF_PX_BLEND] += (uint32_t)(b - a);
                const uint16_t *sp = p + a * 2, *end = p + b * 2;
                uint16_t *q = dst + px + a;
                if (alpha >= 32) {
                    for (; sp < end; sp += 2, q++) {
                        *q = blend565(sp[0], *q, sp[1]);
                    }
                } else {
                    for (; sp < end; sp += 2, q++) {
                        *q = blend565(sp[0], *q, (sp[1] * (uint32_t)alpha) >> 5);
                    }
                }
            }
            p += count * 2;
        }
        px += count;
    }
}

bool gfx_sprite_decode(int id, int frame, uint16_t *colour, uint8_t *alpha) {
    if ((uint32_t)id >= sprite_count || frame < 0 || frame >= sprites[id].frames) {
        return false;
    }
    const sprite_entry *s = &sprites[id];
    const uint32_t *table = (const uint32_t *)(spr_base + s->table);
    const uint8_t *data = spr_base + table[frame];
    const uint32_t *rows = (const uint32_t *)data;
    memset(alpha, 0, (size_t)s->w * s->h);
    memset(colour, 0, (size_t)s->w * s->h * 2);
    for (int row = 0; row < s->h; row++) {
        const uint16_t *p = (const uint16_t *)(data + rows[row]);
        uint16_t *c = colour + row * s->w;
        uint8_t *a = alpha + row * s->w;
        for (int px = 0;;) {
            uint16_t cmd = *p++;
            int kind = cmd >> 14, count = cmd & 0x3FFF;
            if (kind == 3) {
                break;
            }
            for (int i = 0; i < count && px + i < s->w; i++) {
                if (kind == 1) {
                    c[px + i] = p[i];
                    a[px + i] = 32;
                } else if (kind == 2) {
                    c[px + i] = p[i * 2];
                    a[px + i] = (uint8_t)(p[i * 2 + 1] > 32 ? 32 : p[i * 2 + 1]);
                }
            }
            p += kind == 1 ? count : kind == 2 ? count * 2 : 0;
            px += count;
        }
    }
    return true;
}

bool gfx_sprite_extent(int id, int frame, int *x0, int *x1) {
    if ((uint32_t)id >= sprite_count || frame < 0 || frame >= sprites[id].frames) {
        return false;
    }
    const sprite_entry *s = &sprites[id];
    const uint32_t *table = (const uint32_t *)(spr_base + s->table);
    const uint8_t *data = spr_base + table[frame];
    const uint32_t *rows = (const uint32_t *)data;
    int lo = s->w, hi = 0;
    for (int row = 0; row < s->h; row++) {
        const uint16_t *p = (const uint16_t *)(data + rows[row]);
        for (int px = 0;;) {
            uint16_t cmd = *p++;
            int kind = cmd >> 14, count = cmd & 0x3FFF;
            if (kind == 3) {
                break;
            }
            if (kind != 0 && count) {
                lo = px < lo ? px : lo;
                hi = px + count > hi ? px + count : hi;
                p += kind == 1 ? count : count * 2;
            }
            px += count;
        }
    }
    *x0 = lo;
    *x1 = hi;
    return hi > lo;
}

/* columns hidden by a sprite drawn later (gfx_sprite_behind); a == NULL: none */
static struct {
    const uint8_t *a, *b;
    int x, rows;
} cover;

/* draws frame `frame` with its `h` rows spread over `dst_h` rows starting at y */
static void blit(int id, int frame, int x, int y, int dst_h, int alpha) {
    if ((uint32_t)id >= sprite_count || alpha <= 0) {
        return;
    }
    const sprite_entry *s = &sprites[id];
    if (frame < 0 || frame >= s->frames || dst_h <= 0) {
        return;
    }
    int w = s->w, h = s->h;
    if (x >= cx1 || y >= cy1 || x + w <= cx0 || y + dst_h <= cy0) {
        return;
    }
    const uint32_t *table = (const uint32_t *)(spr_base + s->table);
    const uint8_t *data = spr_base + table[frame];
    const uint32_t *rows = (const uint32_t *)data;
    int y0 = y < cy0 ? cy0 - y : 0, y1 = y + dst_h > cy1 ? cy1 - y : dst_h;
    mark(x < cx0 ? cx0 : x, y + y0, x + w > cx1 ? cx1 : x + w, y + y1);
    for (int row = y0; row < y1; row++) {
        int src = dst_h == h ? row : row * h / dst_h;
        const uint16_t *cmds = (const uint16_t *)(data + rows[src]);
        uint16_t *line = target + (y + row) * GFX_W;
        if (cover.a && row < cover.rows && cover.b[row] > cover.a[row]) {
            /* draw what lies left and right of the hidden columns */
            int h0 = cover.x + cover.a[row], h1 = cover.x + cover.b[row];
            int keep0 = cx0, keep1 = cx1;
            if (h0 > keep0) {
                cx1 = h0 < keep1 ? h0 : keep1;
                blit_row(cmds, line, x, alpha);
                cx1 = keep1;
            }
            if (h1 < keep1) {
                cx0 = h1 > keep0 ? h1 : keep0;
                blit_row(cmds, line, x, alpha);
                cx0 = keep0;
            }
            continue;
        }
        blit_row(cmds, line, x, alpha);
    }
}

int gfx_sprite_spans(int id, int frame, uint8_t *a, uint8_t *b, int cap) {
    if ((uint32_t)id >= sprite_count || frame < 0 || frame >= sprites[id].frames || sprites[id].w > 255) {
        return 0;
    }
    const sprite_entry *s = &sprites[id];
    const uint32_t *table = (const uint32_t *)(spr_base + s->table);
    const uint8_t *data = spr_base + table[frame];
    const uint32_t *rows = (const uint32_t *)data;
    int n = s->h < cap ? s->h : cap;
    for (int row = 0; row < n; row++) {
        const uint16_t *p = (const uint16_t *)(data + rows[row]);
        int best0 = 0, best1 = 0;
        for (int px = 0;;) {
            uint16_t cmd = *p++;
            int kind = cmd >> 14, count = cmd & 0x3FFF;
            if (kind == 3) {
                break;
            }
            if (kind == 1 && count > best1 - best0) {
                best0 = px;
                best1 = px + count;
            }
            p += kind == 1 ? count : kind == 2 ? count * 2 : 0;
            px += count;
        }
        a[row] = (uint8_t)best0;
        b[row] = (uint8_t)best1;
    }
    return n;
}

void gfx_sprite_behind(int id, int frame, int x, int y, int cover_x, const uint8_t *a, const uint8_t *b, int rows) {
    cover.a = a;
    cover.b = b;
    cover.x = cover_x;
    cover.rows = rows;
    blit(id, frame, x, y, gfx_sprite_h(id), 32);
    cover.a = NULL;
}

void gfx_sprite_alpha(int id, int frame, int x, int y, int alpha) { blit(id, frame, x, y, gfx_sprite_h(id), alpha); }

void gfx_sprite_stretch(int id, int frame, int x, int y, int extra, int alpha) {
    int h = gfx_sprite_h(id);
    if (h + extra < 1) {
        extra = 1 - h;
    }
    blit(id, frame, x, y - extra, h + extra, alpha);
}

void gfx_sprite(int id, int frame, int x, int y) { gfx_sprite_alpha(id, frame, x, y, 32); }

void gfx_sprite_c(int id, int frame, int cx, int cy) {
    gfx_sprite_alpha(id, frame, cx - gfx_sprite_w(id) / 2, cy - gfx_sprite_h(id) / 2, 32);
}

int gfx_number_fx(int id, int value, int x, int y, int align, int spacing, int stretch, int alpha) {
    int digits[12], n = 0;
    if (value < 0) {
        value = 0;
    }
    do {
        digits[n++] = value % 10;
        value /= 10;
    } while (value && n < 12);
    int dw = gfx_sprite_w(id) + spacing;
    int total = n * dw - spacing;
    if (align == ALIGN_CENTER) {
        x -= total / 2;
    } else if (align == ALIGN_RIGHT) {
        x -= total;
    }
    for (int i = 0; i < n; i++) {
        gfx_sprite_stretch(id, digits[n - 1 - i], x + i * dw, y, stretch, alpha);
    }
    return total;
}

int gfx_number(int id, int value, int x, int y, int align, int spacing) {
    return gfx_number_fx(id, value, x, y, align, spacing, 0, 32);
}

/* ---------- text ---------- */

static uint32_t utf8_next(const char **s, const char *end) {
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    if (extra) {
        c &= 0x3F >> extra;
        while (extra-- && (const char *)p < end && (*p & 0xC0) == 0x80) {
            c = (c << 6) | (*p++ & 0x3F);
        }
    }
    *s = (const char *)p;
    return c;
}

static const glyph_entry *find_glyph(const font_entry *f, uint32_t cp) {
    const glyph_entry *g = (const glyph_entry *)(font_base + f->glyphs);
    int lo = 0, hi = (int)f->glyph_count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g[mid].cp == cp) {
            return &g[mid];
        }
        if (g[mid].cp < cp) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static const font_entry *font_of(int font) { return (uint32_t)font < font_count ? &fonts[font] : NULL; }

/* glyph tops sit this far below the line top; trimmed so `y` is about the top of the letters */
static int font_trim(const font_entry *f) { return f->ascent - f->px + (f->px >> 3); }

int gfx_font_height(int font) {
    const font_entry *f = font_of(font);
    return f ? f->px + 2 : FONT_BUILTIN_H;
}

static int advance_of(const font_entry *f, uint32_t cp) {
    if (!f) {
        return cp >= 0x20 && cp < 0x7F ? font_builtin_width[cp - 0x20] : 6;
    }
    const glyph_entry *g = find_glyph(f, cp);
    return g ? g->advance : f->px / 2;
}

int gfx_text_width(int font, const char *s, int bytes) {
    const font_entry *f = font_of(font);
    const char *end = s + (bytes < 0 ? (int)strlen(s) : bytes);
    int w = 0;
    while (s < end) {
        w += advance_of(f, utf8_next(&s, end));
    }
    return w;
}

static void draw_glyph(const font_entry *f, uint32_t cp, int x, int y, uint16_t color) {
    if (!f) { /* built-in 1-bit font */
        if (cp < 0x20 || cp >= 0x7F) {
            return;
        }
        const unsigned char *bits = font_builtin_bits[cp - 0x20];
        for (int j = 0; j < FONT_BUILTIN_H; j++) {
            int py = y + j;
            if (py < cy0 || py >= cy1) {
                continue;
            }
            for (int i = 0; i < 8; i++) {
                int px = x + i;
                if ((bits[j] & (0x80 >> i)) && px >= cx0 && px < cx1) {
                    target[py * GFX_W + px] = color;
                }
            }
        }
        return;
    }
    const glyph_entry *g = find_glyph(f, cp);
    if (!g || !g->w) {
        return;
    }
    const uint8_t *bmp = font_base + f->bitmaps + g->bitmap;
    prof_counter[PROF_PX_BLEND] += (uint32_t)(g->w * g->h);
    int stride = (g->w + 1) / 2;
    int gx = x + g->xoff, gy = y + g->yoff - font_trim(f);
    for (int j = 0; j < g->h; j++) {
        int py = gy + j;
        if (py < cy0 || py >= cy1) {
            continue;
        }
        const uint8_t *row = bmp + j * stride;
        uint16_t *dst = target + py * GFX_W;
        for (int i = 0; i < g->w; i++) {
            int px = gx + i;
            uint32_t v = (i & 1) ? row[i >> 1] & 15 : row[i >> 1] >> 4;
            if (!v || px < cx0 || px >= cx1) {
                continue;
            }
            dst[px] = v == 15 ? color : blend565(color, dst[px], v * 2 + 1);
        }
    }
}

int gfx_text_n(int font, int x, int y, const char *s, int bytes, uint16_t color, int align, int max_w) {
    const font_entry *f = font_of(font);
    const char *end = s + (bytes < 0 ? (int)strlen(s) : bytes);
    int width = gfx_text_width(font, s, (int)(end - s));
    bool cut = max_w > 0 && width > max_w;
    int dots = cut ? 3 * advance_of(f, '.') : 0;
    if (cut) {
        /* how much fits in front of the "..." */
        const char *p = s;
        int w = 0;
        while (p < end) {
            const char *q = p;
            int a = advance_of(f, utf8_next(&q, end));
            if (w + a + dots > max_w) {
                break;
            }
            w += a;
            p = q;
        }
        end = p;
        width = w + dots;
    }
    if (align == ALIGN_CENTER) {
        x -= width / 2;
    } else if (align == ALIGN_RIGHT) {
        x -= width;
    }
    int h = f ? f->px + 6 : FONT_BUILTIN_H;
    mark(x < cx0 ? cx0 : x, y - 2 < cy0 ? cy0 : y - 2, x + width + 2 > cx1 ? cx1 : x + width + 2,
         y + h > cy1 ? cy1 : y + h);
    if (x >= cx1 || y >= cy1 || x + width <= cx0 || y + h <= cy0) {
        return width;
    }
    int px = x;
    while (s < end) {
        uint32_t cp = utf8_next(&s, end);
        draw_glyph(f, cp, px, y, color);
        px += advance_of(f, cp);
    }
    if (cut) {
        for (int i = 0; i < 3; i++) {
            draw_glyph(f, '.', px, y, color);
            px += advance_of(f, '.');
        }
    }
    return width;
}

int gfx_text(int font, int x, int y, const char *s, uint16_t color, int align, int max_w) {
    return gfx_text_n(font, x, y, s, -1, color, align, max_w);
}

int gfx_text_outline(int font, int x, int y, const char *s, int bytes, uint16_t color, uint16_t outline, int align,
                     int max_w) {
    static const int8_t off[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for (int i = 0; i < 4; i++) {
        gfx_text_n(font, x + off[i][0], y + off[i][1], s, bytes, outline, align, max_w);
    }
    return gfx_text_n(font, x, y, s, bytes, color, align, max_w);
}

/* ---------- other assets ---------- */

int assets_cfg(int id, int fallback) {
    uint32_t bytes;
    const uint8_t *sec = lib_section(LIB_SEC_SKINCFG, &bytes);
    if (bytes < 4 || (uint32_t)id >= *(const uint32_t *)sec) {
        return fallback;
    }
    return ((const int32_t *)(sec + 4))[id];
}

const int16_t *assets_sound(int id, uint32_t *samples, uint32_t *rate) {
    uint32_t bytes;
    const uint8_t *sec = lib_section(LIB_SEC_SOUNDS, &bytes);
    if (bytes < 4 || (uint32_t)id >= *(const uint32_t *)sec) {
        return NULL;
    }
    const uint32_t *e = (const uint32_t *)(sec + 4 + 16 * (uint32_t)id);
    if (!e[1]) {
        return NULL;
    }
    *rate = e[0];
    *samples = e[1];
    return (const int16_t *)(sec + e[2]);
}
