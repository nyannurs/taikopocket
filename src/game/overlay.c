#include "overlay.h"

#include <string.h>

#include "assets.h"
#include "gfx.h"

/* palette banks */
enum {
    B_NOTE, B_NOTE_BIG, B_ROLL, B_BALLOON, B_LABEL, B_BAR, B_BURST, B_BURST2, B_LOWER, B_JUDGE, B_COUNTER,
    B_DIM,         /* 11..14: B_BURST..B_JUDGE at half strength */
    B_SPLASH = 15, /* the Go-Go splash */
};
#define DIM_BANKS 4

typedef struct {
    int sprite, first, count; /* frames first .. first + count - 1; count 0: all */
    int bank;
    bool clip;     /* only in the lane */
    bool optional; /* left out if the sheet is full */
    bool twice;    /* kept at half the resolution and shown with doubled pixels */
} group;

static const group groups[] = {
    {SPR_NOTES, 1, 2, B_NOTE, true, false, false},
    {SPR_NOTES, 3, 2, B_NOTE_BIG, true, false, false},
    {SPR_NOTES, 5, 6, B_ROLL, true, false, false},
    {SPR_NOTES, 11, 4, B_BALLOON, true, false, false},
    {SPR_SENOTES, 0, 0, B_LABEL, true, false, false},
    {SPR_EXPLOSION, 0, 0, B_BURST, false, false, false},
    {SPR_EXPLOSION_LOWER, 0, 0, B_LOWER, false, false, false},
    {SPR_JUDGE, 0, 0, B_JUDGE, false, false, false},
    /* the kusudama counter lies on the lane, over the notes */
    {SPR_POTATO_BALLOON, 0, 0, B_COUNTER, false, true, false},
    {SPR_NUM_POTATO, 0, 0, B_COUNTER, false, true, false},
    /* soft smoke, taller than a cell can be and too large for the sheet at full resolution */
    {SPR_GOGO_SPLASH, 0, 0, B_SPLASH, false, true, true},
    /* the Go-Go flame: soft too, and four frames of 96 x 96; it shares the palette of the
     * inner hit effect, which has its colours */
    {SPR_FIRE, 0, 0, B_LOWER, false, true, true},
};
#define GROUPS ((int)(sizeof groups / sizeof groups[0]))

static bool on;
static int overlay_cell_of(int sprite, int frame);
static int group_cell[GROUPS], group_frames[GROUPS]; /* first cell of each group, -1: not on the sheet */
static uint8_t cell_bank[PF_OVL_CELLS];
/* a cell holds only the part of its picture that has pixels: where that part sits in the
 * picture, and its size */
static uint8_t cell_dx[PF_OVL_CELLS], cell_dy[PF_OVL_CELLS], cell_w[PF_OVL_CELLS], cell_h[PF_OVL_CELLS];
int overlay_bar = -1, overlay_bar_branch = -1;
int overlay_roll_piece[2][OVERLAY_ROLL_PIECES];

static uint8_t sheet[PF_OVL_SHEET];
static uint32_t sheet_used;
static int cell_count;
static uint16_t px_colour[PF_OVL_MAX_SIZE * PF_OVL_MAX_SIZE];
static uint8_t px_alpha[PF_OVL_MAX_SIZE * PF_OVL_MAX_SIZE];

/* ---- a bank's palette: the colours of its cells, merged down to PF_OVL_COLOURS ---- */
#define COLOURS 512
static struct {
    uint32_t key; /* colour << 8 | alpha 1..16 */
    uint32_t uses;
    int16_t into; /* merged into this entry, -1: kept */
    uint8_t index;
} col[COLOURS];
static int col_n;
static uint16_t coarse; /* colour bits kept while collecting */

static uint32_t key_of(uint16_t colour, uint8_t alpha32) {
    return (uint32_t)(colour & coarse) << 8 | (uint32_t)((alpha32 + 1) / 2);
}

static int find(uint32_t key) {
    static int last;
    if (last < col_n && col[last].key == key) {
        return last;
    }
    for (int i = 0; i < col_n; i++) {
        if (col[i].key == key) {
            return last = i;
        }
    }
    return -1;
}

static int distance(uint32_t a, uint32_t b) {
    int ca = (int)(a >> 8), cb = (int)(b >> 8);
    int dr = ((ca >> 11) - (cb >> 11)) * 8, dg = ((ca >> 5 & 63) - (cb >> 5 & 63)) * 4, db = ((ca & 31) - (cb & 31)) * 8;
    int da = ((int)(a & 255) - (int)(b & 255)) * 16;
    return dr * dr + dg * dg + db * db + da * da;
}

/* Frame `f` of group `g` into px_colour / px_alpha; *w and *h are its size there. A
 * `twice` group is halved: each pixel is the average of 2 x 2, weighted by alpha. */
static bool load_frame(const group *g, int f, int *w, int *h) {
    *w = gfx_sprite_w(g->sprite);
    *h = gfx_sprite_h(g->sprite);
    if ((size_t)(*w * *h) > sizeof px_alpha) {
        return false;
    }
    memset(px_alpha, 0, (size_t)(*w * *h));
    if (!gfx_sprite_decode(g->sprite, g->first + f, px_colour, px_alpha)) {
        return false;
    }
    if (g->twice) {
        int hw = *w / 2, hh = *h / 2;
        for (int y = 0; y < hh; y++) {
            for (int x = 0; x < hw; x++) {
                uint32_t a = 0, r = 0, gr = 0, b = 0;
                for (int k = 0; k < 4; k++) {
                    int p = (2 * y + k / 2) * *w + 2 * x + k % 2;
                    uint32_t pa = px_alpha[p], c = px_colour[p];
                    a += pa;
                    r += (c >> 11) * pa;
                    gr += (c >> 5 & 63) * pa;
                    b += (c & 31) * pa;
                }
                /* writes land at or before the pixels just read */
                px_alpha[y * hw + x] = (uint8_t)((a + 2) / 4);
                px_colour[y * hw + x] = a ? (uint16_t)((r / a) << 11 | (gr / a) << 5 | (b / a)) : 0;
            }
        }
        *w = hw;
        *h = hh;
    }
    return true;
}

/* collects the colours of every frame of `g`; false if there are more than the table holds */
static bool collect(const group *g, int frames) {
    for (int f = 0; f < frames; f++) {
        int w, h;
        if (!load_frame(g, f, &w, &h)) {
            continue;
        }
        int n = w * h;
        for (int i = 0; i < n; i++) {
            if (!px_alpha[i]) {
                continue;
            }
            uint32_t key = key_of(px_colour[i], px_alpha[i]);
            int at = find(key);
            if (at < 0) {
                if (col_n >= COLOURS) {
                    return false;
                }
                at = col_n++;
                col[at].key = key;
                col[at].uses = 0;
                col[at].into = -1;
            }
            col[at].uses++;
        }
    }
    return true;
}

/* merges the least used colours into their nearest neighbours until PF_OVL_COLOURS are left, numbers them */
static void reduce(void) {
    int kept = col_n;
    while (kept > PF_OVL_COLOURS) {
        int least = -1;
        for (int i = 0; i < col_n; i++) {
            if (col[i].into < 0 && (least < 0 || col[i].uses < col[least].uses)) {
                least = i;
            }
        }
        int nearest = -1, best = 0;
        for (int i = 0; i < col_n; i++) {
            if (i != least && col[i].into < 0) {
                int d = distance(col[i].key, col[least].key);
                if (nearest < 0 || d < best) {
                    nearest = i;
                    best = d;
                }
            }
        }
        col[least].into = (int16_t)nearest;
        col[nearest].uses += col[least].uses;
        kept--;
    }
    uint8_t next = 1;
    for (int i = 0; i < col_n; i++) {
        if (col[i].into < 0) {
            col[i].index = next++;
        }
    }
}

static uint8_t index_of(uint32_t key) {
    int at = find(key);
    if (at < 0) { /* a colour that was not collected: the nearest one that was */
        int best = 0;
        for (int i = 0; i < col_n; i++) {
            int d = distance(col[i].key, key);
            if (at < 0 || d < best) {
                at = i;
                best = d;
            }
        }
        if (at < 0) {
            return 0;
        }
    }
    while (col[at].into >= 0) {
        at = col[at].into;
    }
    return col[at].index;
}

/* colour times alpha / 16, per channel */
static uint32_t entry(uint32_t colour, uint32_t alpha) {
    uint32_t r = (colour >> 11) * alpha / 16, g = (colour >> 5 & 63) * alpha / 16, b = (colour & 31) * alpha / 16;
    return PF_OVL_ENTRY(alpha, r << 11 | g << 5 | b);
}

static void upload_palette(int bank) {
    for (int i = 0; i < col_n; i++) {
        if (col[i].into >= 0) {
            continue;
        }
        uint32_t colour = col[i].key >> 8, alpha = col[i].key & 255;
        pf_ovl_palette(bank, col[i].index, entry(colour, alpha));
        if (bank >= B_BURST && bank < B_BURST + DIM_BANKS) {
            pf_ovl_palette(B_DIM + bank - B_BURST, col[i].index, entry(colour, (alpha + 1) / 2));
        }
    }
}

static uint32_t tile_addr[PF_OVL_CELLS]; /* where each cell's pixels start on the sheet */

/* The burst comes in two tints of one shape. If frame `f` of group `g` has the pixels of
 * its frame 0 in other colours, it becomes a cell on the same sheet pixels with its own
 * palette (bank `bank`), and half the burst's sheet space is free for other things. */
static bool recolour(const group *g, int f, int first_cell, int bank) {
    static uint16_t base_colour[PF_OVL_MAX_SIZE * PF_OVL_MAX_SIZE];
    static uint8_t base_alpha[PF_OVL_MAX_SIZE * PF_OVL_MAX_SIZE];
    int w = gfx_sprite_w(g->sprite), h = gfx_sprite_h(g->sprite);
    if (!gfx_sprite_decode(g->sprite, g->first, base_colour, base_alpha) ||
        !gfx_sprite_decode(g->sprite, g->first + f, px_colour, px_alpha) ||
        memcmp(base_alpha, px_alpha, (size_t)(w * h)) != 0) {
        return false;
    }
    uint32_t colour_of[32];
    bool seen[32] = {false};
    for (int p = 0; p < w * h; p++) {
        if (!px_alpha[p]) {
            continue;
        }
        uint8_t index = index_of(key_of(base_colour[p], base_alpha[p]));
        if (!seen[index]) {
            seen[index] = true;
            colour_of[index] = (uint32_t)px_colour[p] << 8 | (uint32_t)((px_alpha[p] + 1) / 2);
        } else if (colour_of[index] >> 8 != px_colour[p]) {
            return false; /* not the same picture in another colour */
        }
    }
    for (int i = 1; i < 32; i++) {
        if (seen[i]) {
            pf_ovl_palette(bank, i, entry(colour_of[i] >> 8, colour_of[i] & 255));
            pf_ovl_palette(B_DIM + bank - B_BURST, i, entry(colour_of[i] >> 8, ((colour_of[i] & 255) + 1) / 2));
        }
    }
    /* same pixels as the first frame's cell, other bank */
    tile_addr[cell_count] = tile_addr[first_cell];
    cell_dx[cell_count] = cell_dx[first_cell];
    cell_dy[cell_count] = cell_dy[first_cell];
    cell_w[cell_count] = cell_w[first_cell];
    cell_h[cell_count] = cell_h[first_cell];
    pf_ovl_cell(cell_count, tile_addr[first_cell], cell_w[first_cell], cell_h[first_cell], g->clip, false);
    cell_bank[cell_count++] = (uint8_t)bank;
    return true;
}

/* a cell of w x h pixels that are already at the end of the sheet; (dx, dy) is where it
 * sits in its picture */
static int new_cell(int w, int h, int dx, int dy, int bank, bool clip, bool twice) {
    tile_addr[cell_count] = sheet_used;
    cell_dx[cell_count] = (uint8_t)dx;
    cell_dy[cell_count] = (uint8_t)dy;
    cell_w[cell_count] = (uint8_t)w;
    cell_h[cell_count] = (uint8_t)h;
    pf_ovl_cell(cell_count, sheet_used, w, h, clip, twice);
    cell_bank[cell_count] = (uint8_t)bank;
    sheet_used += (uint32_t)(w * h);
    return cell_count++;
}

/* Puts the frame decoded in px_colour / px_alpha (w x h) on the sheet, without its empty
 * margins. False if the sheet or the cell table is full. */
static bool add_frame(int w, int h, int bank, bool clip, bool twice) {
    int x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (px_alpha[y * w + x]) {
                x0 = x < x0 ? x : x0;
                x1 = x + 1 > x1 ? x + 1 : x1;
                y0 = y < y0 ? y : y0;
                y1 = y + 1 > y1 ? y + 1 : y1;
            }
        }
    }
    if (x1 <= x0) { /* an empty frame still gets a cell, of one empty pixel */
        x0 = y0 = 0;
        x1 = y1 = 1;
    }
    int cw = x1 - x0, ch = y1 - y0;
    sheet_used += sheet_used & 1; /* cells start at even addresses */
    if (sheet_used + (uint32_t)(cw * ch) > pf_ovl_capacity() || cell_count >= PF_OVL_CELLS) {
        return false;
    }
    uint8_t *out = sheet + sheet_used;
    for (int y = 0; y < ch; y++) {
        for (int x = 0; x < cw; x++) {
            int p = (y0 + y) * w + x0 + x;
            out[y * cw + x] = px_alpha[p] ? index_of(key_of(px_colour[p], px_alpha[p])) : 0;
        }
    }
    /* a doubled cell is twice as far from the corner of its picture as from that of its sheet image */
    new_cell(cw, ch, twice ? x0 * 2 : x0, twice ? y0 * 2 : y0, bank, clip, twice);
    return true;
}

/* Ends every row of every cell after its last pixel, where the row has room for the
 * mark. The layer then spends no time on the empty right side of a round note, and
 * with notes piled up that is most of what there is to look at. A layer that does not
 * know the mark shows it as nothing: its palette entry stays empty. */
static void mark_row_ends(void) {
    for (int bank = 0; bank < PF_OVL_BANKS; bank++) {
        pf_ovl_palette(bank, PF_OVL_ROW_END, 0);
    }
    for (int c = 0; c < cell_count; c++) {
        uint8_t *at = sheet + tile_addr[c];
        for (int y = 0; y < cell_h[c]; y++, at += cell_w[c]) {
            int end = cell_w[c];
            while (end > 0 && (at[end - 1] == 0 || at[end - 1] == PF_OVL_ROW_END)) {
                end--;
            }
            if (end < cell_w[c]) {
                at[end] = PF_OVL_ROW_END; /* cells that share their pixels get the same mark again */
            }
        }
    }
}

void overlay_init(void) {
    on = false;
    sheet_used = 0;
    cell_count = 0;
    for (int i = 0; i < GROUPS; i++) {
        group_cell[i] = -1;
    }
    overlay_bar = overlay_bar_branch = -1;
    if (!pf_ovl_supported()) {
        return;
    }
    for (int i = 0; i < GROUPS; i++) {
        const group *g = &groups[i];
        int frames = gfx_sprite_frames(g->sprite) - g->first;
        frames = frames < 0 ? 0 : g->count && g->count < frames ? g->count : frames;
        group_frames[i] = frames;
        int w = gfx_sprite_w(g->sprite), h = gfx_sprite_h(g->sprite);
        if (g->twice) {
            w /= 2;
            h /= 2;
        }
        if (frames && (w > PF_OVL_MAX_SIZE || h > PF_OVL_MAX_SIZE || (g->twice && !pf_ovl_doubling()))) {
            if (!g->optional) {
                return;
            }
            group_frames[i] = 0;
        }
    }

    /* bar lines: one column, 48 high; palette index 1 plain, 2 the start of a branch section */
    memset(sheet, 1, 48);
    memset(sheet + 48, 2, 48);
    overlay_bar = new_cell(1, 48, 0, 0, B_BAR, true, false);
    overlay_bar_branch = new_cell(1, 48, 0, 0, B_BAR, true, false);
    pf_ovl_palette(B_BAR, 1, entry((uint32_t)assets_cfg(CFG_BARLINE_COLOR, 0xFFFF), 16));
    pf_ovl_palette(B_BAR, 2, entry(RGB(255, 220, 0), 16));

    for (int bank = 0; bank < PF_OVL_BANKS; bank++) {
        if (bank == B_BAR || (bank >= B_DIM && bank != B_SPLASH)) {
            continue;
        }
        /* the colours of all groups in this bank; coarser colours if there are too many kinds */
        static const uint16_t steps[] = {0xFFFF, 0xF7DE, 0xE79C, 0xC718};
        for (int step = 0; step < 4; step++) {
            col_n = 0;
            coarse = steps[step];
            bool fits = true;
            for (int i = 0; i < GROUPS && fits; i++) {
                if (groups[i].bank == bank) {
                    fits = collect(&groups[i], group_frames[i]);
                }
            }
            if (fits) {
                break;
            }
        }
        reduce();
        upload_palette(bank);
        for (int i = 0; i < GROUPS; i++) {
            const group *g = &groups[i];
            int w = 0, h = 0;
            if (g->bank != bank || !group_frames[i]) {
                continue;
            }
            /* all frames of the group or none: a group that does not fit is taken off again */
            int cells_before = cell_count;
            uint32_t used_before = sheet_used;
            bool fits = true;
            for (int f = 0; f < group_frames[i] && fits; f++) {
                if (bank == B_BURST && f == 1 && group_frames[i] == 2 && recolour(g, f, cells_before, B_BURST2)) {
                    continue;
                }
                fits = load_frame(g, f, &w, &h) && add_frame(w, h, bank, g->clip, g->twice);
            }
            if (fits) {
                group_cell[i] = cells_before;
            } else {
                cell_count = cells_before;
                sheet_used = used_before;
                if (!g->optional) {
                    pf_log("sprite layer: the skin's pictures do not fit its sheet; the game draws them itself");
                    return;
                }
            }
        }
    }
    /* Narrow pieces of the two drumroll bars, for the end of a bar that is not a whole
     * number of 48 px tiles long (a sprite cannot be cut to a width). */
    static const int piece_w[OVERLAY_ROLL_PIECES] = {24, 12, 6, 3};
    for (int big = 0; big < 2; big++) {
        int frame = big ? 9 : 6;
        for (int k = 0; k < OVERLAY_ROLL_PIECES; k++) {
            overlay_roll_piece[big][k] = -1;
        }
        if (overlay_cell_of(SPR_NOTES, frame) < 0) {
            continue;
        }
        /* the roll bank's palette is no longer in `col`: copy indices from the bar's cell,
         * which is already on the sheet */
        int tile = overlay_cell_of(SPR_NOTES, frame);
        const uint8_t *from = sheet + tile_addr[tile];
        int tw = cell_w[tile], th = cell_h[tile];
        for (int k = 0; k < OVERLAY_ROLL_PIECES; k++) {
            sheet_used += sheet_used & 1;
            if (tw < piece_w[k] || sheet_used + (uint32_t)(piece_w[k] * th) > pf_ovl_capacity() ||
                cell_count >= PF_OVL_CELLS) {
                break;
            }
            uint8_t *out = sheet + sheet_used;
            for (int y = 0; y < th; y++) {
                memcpy(out + y * piece_w[k], from + y * tw + (tw - piece_w[k]) / 2, (size_t)piece_w[k]);
            }
            overlay_roll_piece[big][k] = new_cell(piece_w[k], th, 0, cell_dy[tile], B_ROLL, true, false);
        }
    }
    mark_row_ends();
    pf_ovl_sheet(0, sheet, sheet_used);
    pf_ovl_show(NULL, 0);
    on = true;
    pf_log("sprite layer: %d cells, %u of %u sheet pixels", cell_count, (unsigned)sheet_used, (unsigned)pf_ovl_capacity());
}

bool overlay_on(void) { return on; }

int overlay_cell(int sprite, int frame) { return on ? overlay_cell_of(sprite, frame) : -1; }

static int overlay_cell_of(int sprite, int frame) {
    for (int i = 0; i < GROUPS; i++) {
        if (groups[i].sprite == sprite && group_cell[i] >= 0 && frame >= groups[i].first &&
            frame < groups[i].first + group_frames[i]) {
            return group_cell[i] + frame - groups[i].first;
        }
    }
    return -1;
}

/* ---- a frame's sprites ---- */
#define LIST_MAX 512
static uint32_t list[LIST_MAX];
static int list_n;

void overlay_begin(void) { list_n = 0; }

void overlay_add(int cell, int x, int y, bool dim) {
    if (!on || cell < 0 || cell >= cell_count || x <= -2 * PF_OVL_MAX_SIZE || x >= GFX_W ||
        y <= -2 * PF_OVL_MAX_SIZE || y >= GFX_H) {
        return;
    }
    if (list_n == LIST_MAX) { /* too many: the ones at the very back go */
        memmove(list, list + 1, (LIST_MAX - 1) * sizeof list[0]);
        list_n--;
    }
    int bank = cell_bank[cell];
    if (dim && bank >= B_BURST && bank < B_BURST + DIM_BANKS) {
        bank = B_DIM + bank - B_BURST;
    }
    list[list_n++] = PF_OVL_SPRITE(x + cell_dx[cell], y + cell_dy[cell], cell, bank);
}

/* the layer wants the front sprite first, and takes 128 */
void overlay_end(void) {
    static uint32_t out[PF_OVL_SPRITES];
    int n = list_n > PF_OVL_SPRITES ? PF_OVL_SPRITES : list_n;
    for (int i = 0; i < n; i++) {
        out[i] = list[list_n - 1 - i];
    }
    pf_ovl_show(out, n);
}

void overlay_clear(void) {
    list_n = 0;
    if (on) {
        pf_ovl_show(NULL, 0);
    }
}
