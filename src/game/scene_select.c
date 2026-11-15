/* Song select: folders, a paged song list, course choice, preview.
 *
 * Two levels: the list of folders (the library's categories), and inside a folder
 * its songs. A opens a folder, B closes it. The game starts in the folder of the
 * song played last.
 *
 * Everything but the cursor lives in the background layer. On the Pocket a pixel
 * costs about 0.1 us to copy and 0.7 us to blend, so repainting this screen takes
 * several frames. It is therefore drawn in pieces, a few per frame:
 *  - the backdrop with the empty row panels and the footer panel is blended once
 *    and kept (`base`); a piece is repainted by copying its part of `base` back
 *    and drawing its text;
 *  - the list shows one page of the category at a time, so moving the cursor
 *    changes no row, only the cursor frame, which is drawn each frame;
 *  - header and footer follow the cursor, one of them per frame. */
#include <stdio.h>
#include <string.h>

#include "game.h"

#define ROW_H 44
#define ROWS 6
#define LIST_Y 34
#define FOOT_Y (LIST_Y + ROWS * ROW_H + 4)

static const char *const course_names[LIB_COURSES] = {"Easy", "Normal", "Hard", "Oni", "Edit"};
static const uint16_t course_colors[LIB_COURSES] = {RGB(240, 72, 40), RGB(120, 190, 50), RGB(50, 140, 220),
                                                    RGB(220, 50, 140), RGB(130, 80, 230)};

static uint32_t sel;     /* selected song */
static uint32_t pos;     /* its place in the open folder's list */
static uint32_t top;     /* place of the first song of the page shown */
/* the songs of the open folder in the order they are listed (settings: by title, by
 * difficulty) */
#define ORDER_MAX 4096
static uint16_t order[ORDER_MAX];
static uint32_t order_n;
static uint32_t order_folder = 0xFFFFFFFFu;
static int order_kind = -1, order_course = -1;
static int course;       /* course cursor */
static uint32_t folder;     /* folder cursor */
static uint32_t folder_top; /* first folder of the page shown */
static enum { FOLDERS, BROWSE, COURSE, LOADING, MESSAGE } mode;
static uint16_t base[GFX_W * GFX_H]; /* backdrop, row panels, footer panel */
static bool base_ready;
/* pieces of the screen that have to be drawn again */
enum { PIECE_HEADER = 1 << ROWS, PIECE_FOOTER = 2 << ROWS, PIECE_ALL = (4 << ROWS) - 1 };
static uint32_t stale;
static uint32_t detail_sel; /* what header and footer describe: a song, or DETAIL_FOLDER | folder */
#define DETAIL_FOLDER 0x80000000u
#define ROWS_PER_FRAME 1 /* a row is some 5 ms of text on the Pocket */
static const char *message = "";
static uint32_t rng = 0x2545F491u;

static bool has_course(const lib_song *s, int c) { return s->level[c] != LIB_NO_COURSE; }

/* nearest existing course to `want`, preferring easier ones on a tie */
static int pick_course(const lib_song *s, int want) {
    for (int d = 0; d < LIB_COURSES; d++) {
        if (want - d >= 0 && has_course(s, want - d)) {
            return want - d;
        }
        if (want + d < LIB_COURSES && has_course(s, want + d)) {
            return want + d;
        }
    }
    return 3;
}

static int crown_frame(int crown) {
    return crown >= CROWN_FULL_COMBO ? 4 : crown == CROWN_CLEAR ? 2 : crown == CROWN_PLAYED ? 1 : 0;
}

/* copies a rectangle of `base` back into the background layer */
static void restore(int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        gfx_bg_write(x, y + j, w, 1, base + (y + j) * GFX_W + x);
    }
}

static void compose_base(void) {
    gfx_target(GFX_BG);
    gfx_clip_reset();
    if (gfx_sprite_frames(SPR_SONGSELECT_BG)) {
        gfx_sprite(SPR_SONGSELECT_BG, 0, 0, 0);
    } else {
        gfx_fill(0, 0, GFX_W, GFX_H, RGB(16, 18, 28));
    }
    for (int r = 0; r < ROWS; r++) {
        gfx_blend(6, LIST_Y + r * ROW_H, GFX_W - 12, ROW_H - 4, RGB(16, 16, 20), 22);
    }
    gfx_blend(0, FOOT_Y, GFX_W, GFX_H - FOOT_Y, RGB(0, 0, 0), 22);
    gfx_target(GFX_FRAME);
    gfx_bg_read(0, 0, GFX_W, GFX_H, base);
    base_ready = true;
}

static void draw_row(int r) {
    int y = LIST_Y + r * ROW_H;
    restore(0, y, GFX_W, ROW_H);
    if (mode == FOLDERS) {
        uint32_t f = folder_top + (uint32_t)r;
        if (f < lib_category_count()) {
            const lib_category *cat = lib_get_category(f);
            char buf[32];
            gfx_fill(6, y, 10, ROW_H - 4, RGB24(cat->color));
            gfx_text(FONT_L, 24, y + 10, lib_string(cat->name), RGB(235, 235, 235), ALIGN_LEFT, GFX_W - 24 - 90);
            snprintf(buf, sizeof buf, "%u songs", (unsigned)cat->song_count);
            gfx_text(FONT_S, GFX_W - 16, y + 13, buf, RGB(170, 170, 180), ALIGN_RIGHT, 0);
        }
        gfx_bg_changed_rect(0, y, GFX_W, ROW_H);
        return;
    }
    const lib_category *cat = lib_get_category(lib_get_song(sel)->category);
    if (top + (uint32_t)r < order_n) {
        const lib_song *s = lib_get_song(order[top + (uint32_t)r]);
        gfx_fill(6, y, 4, ROW_H - 4, RGB24(cat->color));
        gfx_text(FONT_L, 18, y + 4, lib_string(s->title), RGB(235, 235, 235), ALIGN_LEFT, GFX_W - 18 - 70);
        gfx_text(FONT_S, 20, y + 24, lib_string(s->subtitle), RGB(150, 150, 160), ALIGN_LEFT, GFX_W - 20 - 70);
        int c = pick_course(s, settings.last_course);
        char buf[16];
        snprintf(buf, sizeof buf, "\xE2\x98\x85%d", s->level[c]); /* a star and the level */
        gfx_text(FONT_L, GFX_W - 16, y + 4, buf, course_colors[c], ALIGN_RIGHT, 0);
        if (s->branch_mask >> c & 1) { /* this course has branches: the two arrows, as on its chip in the footer */
            gfx_text(FONT_S, GFX_W - 36, y + 24, "\xE2\x86\x91\xE2\x86\x93", RGB(255, 200, 80), ALIGN_RIGHT, 0);
        }
        const score_entry *best = score_find(s->chart_crc, c);
        gfx_sprite(SPR_CLEARMARK, crown_frame(best ? best->crown : CROWN_NONE), GFX_W - 28, y + 26);
    }
    gfx_bg_changed_rect(0, y, GFX_W, ROW_H);
}

/* auto-play and the modifiers that are on, in the header left of the position; on a
 * dark patch, since a folder's colour can be anything */
static void header_tags(void) {
    char tags[64];
    snprintf(tags, sizeof tags, "%s%s", settings.auto_play ? "AUTO " : "", modifier_tags());
    int n = (int)strlen(tags);
    if (!n) {
        return;
    }
    if (tags[n - 1] == ' ') {
        tags[--n] = 0;
    }
    int w = n * 7 + 10;
    gfx_fill(GFX_W - 64 - w, 5, w + 4, 18, RGB(24, 16, 16));
    gfx_text(FONT_S, GFX_W - 66, 8, tags, RGB(255, 110, 110), ALIGN_RIGHT, 0);
}

static void draw_header(void) {
    char buf[48];
    if (mode == FOLDERS) {
        gfx_fill(0, 0, GFX_W, 28, RGB(36, 36, 44));
        gfx_fill(0, 28, GFX_W, 2, RGB(0, 0, 0));
        gfx_text(FONT_L, 10, 5, "Folders", RGB(255, 255, 255), ALIGN_LEFT, GFX_W - 100);
        snprintf(buf, sizeof buf, "%u / %u", (unsigned)(folder + 1), (unsigned)lib_category_count());
        gfx_text(FONT_S, GFX_W - 10, 8, buf, RGB(255, 255, 255), ALIGN_RIGHT, 0);
        header_tags();
        gfx_bg_changed_rect(0, 0, GFX_W, 30);
        return;
    }
    const lib_song *s = lib_get_song(sel);
    const lib_category *cat = lib_get_category(s->category);
    gfx_fill(0, 0, GFX_W, 28, RGB24(cat->color));
    gfx_fill(0, 28, GFX_W, 2, RGB(0, 0, 0));
    gfx_text(FONT_L, 10, 5, lib_string(cat->name), RGB24(cat->text_color), ALIGN_LEFT, GFX_W - 100);
    snprintf(buf, sizeof buf, "%u / %u", (unsigned)(pos + 1), (unsigned)order_n);
    gfx_text(FONT_S, GFX_W - 10, 8, buf, RGB24(cat->text_color), ALIGN_RIGHT, 0);
    header_tags();
    gfx_bg_changed_rect(0, 0, GFX_W, 30);
}

/* footer: the courses of the selected song, the hint line, the best score */
static void draw_footer(void) {
    const lib_song *s = lib_get_song(sel);
    char buf[48];
    restore(0, FOOT_Y, GFX_W, GFX_H - FOOT_Y);
    if (mode == FOLDERS) {
        snprintf(buf, sizeof buf, "%u songs in %u folders", (unsigned)lib_song_count(), (unsigned)lib_category_count());
        gfx_text(FONT_S, 10, FOOT_Y + 12, buf, RGB(200, 200, 210), ALIGN_LEFT, 0);
        gfx_text(FONT_S, 10, FOOT_Y + 38, "A: open   X: random song   SELECT: settings", RGB(170, 170, 180), ALIGN_LEFT,
                 GFX_W - 60);
        gfx_bg_changed_rect(0, FOOT_Y, GFX_W, GFX_H - FOOT_Y);
        gfx_bg_touched();
        return;
    }
    int chip_w = 74, x = 8;
    for (int c = 0; c < LIB_COURSES; c++) {
        bool present = has_course(s, c);
        bool cursor = mode == COURSE && c == course;
        /* solid colours: the course colour, dimmed unless the cursor is on it */
        uint16_t color = !present ? RGB(22, 22, 24) : cursor ? course_colors[c]
                         : RGB((course_colors[c] >> 11) * 8 * 7 / 16, ((course_colors[c] >> 5) & 63) * 4 * 7 / 16,
                               (course_colors[c] & 31) * 8 * 7 / 16);
        gfx_fill(x, FOOT_Y + 4, chip_w, 30, color);
        if (cursor) {
            gfx_frame_rect(x - 1, FOOT_Y + 3, chip_w + 2, 32, RGB(255, 255, 255));
        }
        gfx_text(FONT_S, x + chip_w / 2, FOOT_Y + 6, course_names[c], present ? RGB(255, 255, 255) : RGB(110, 110, 110),
                 ALIGN_CENTER, 0);
        if (present) {
            snprintf(buf, sizeof buf, "\xE2\x98\x85%d%s", s->level[c], (s->branch_mask >> c & 1) ? " \xE2\x86\x91\xE2\x86\x93" : "");
            gfx_text(FONT_S, x + chip_w / 2, FOOT_Y + 20, buf, RGB(255, 255, 255), ALIGN_CENTER, 0);
        }
        x += chip_w + 4;
    }
    const char *hint = mode == COURSE ? "A: play   B: back" : "A: choose   B: folders   X: random";
    if (mode == LOADING) {
        hint = "Loading...";
    } else if (mode == MESSAGE) {
        hint = message;
    }
    gfx_text(FONT_S, 10, FOOT_Y + 38, hint, mode == MESSAGE ? RGB(255, 200, 80) : RGB(170, 170, 180), ALIGN_LEFT,
             GFX_W - 130);
    if (mode != LOADING && mode != MESSAGE) {
        int c = mode == COURSE ? course : pick_course(s, settings.last_course);
        const score_entry *best = score_find(s->chart_crc, c);
        if (best) {
            snprintf(buf, sizeof buf, "Best %u", (unsigned)best->score);
            gfx_text(FONT_S, ui_corner_x(), FOOT_Y + 38, buf, RGB(255, 220, 80), ALIGN_RIGHT, 0);
        }
    }
    gfx_bg_changed_rect(0, FOOT_Y, GFX_W, GFX_H - FOOT_Y);
    gfx_bg_touched(); /* the frame rate box sits in this corner */
}

/* ---- personal best popup ----
 * When the cursor has rested on a song with a saved best, a small window over the next
 * row shows that play's counts. With bests on several courses it goes through them,
 * easiest first, fading from one to the next. It is drawn into the background layer
 * when it changes: a fade is the same window in darker colours, four steps, no blending. */
#define POPUP_W 236
#define POPUP_H (ROW_H - 4)
#define POPUP_X (GFX_W - 10 - POPUP_W)
#define POPUP_REST 45  /* frames the cursor has to rest before it appears */
#define POPUP_HOLD 150 /* frames one course is shown */
#define POPUP_STEPS 4  /* brightness steps of a fade */
#define POPUP_STEP_FRAMES 3
static uint32_t rest_sel = 0xFFFFFFFFu, rest_frames;
static int popup_row = -1; /* the row it covers; -1: not shown */
static int popup_course, popup_level, popup_dir;
static uint32_t popup_timer;
static bool popup_only; /* the song has one best: nothing to go through */

static const score_entry *best_of(const lib_song *s, int c) {
    return has_course(s, c) ? score_find(s->chart_crc, c) : NULL;
}

/* the next course after `c` that has a best, going round; -1 if there is none */
static int next_best(const lib_song *s, int c) {
    for (int i = 1; i <= LIB_COURSES; i++) {
        int k = (c + i) % LIB_COURSES;
        if (best_of(s, k)) {
            return k;
        }
    }
    return -1;
}

static uint16_t dim(uint16_t c, int level) {
    return (uint16_t)(((c >> 11) * level / POPUP_STEPS) << 11 | (((c >> 5) & 63) * level / POPUP_STEPS) << 5 |
                      ((c & 31) * level / POPUP_STEPS));
}

static void popup_draw(void) {
    const lib_song *s = lib_get_song(sel);
    const score_entry *e = best_of(s, popup_course);
    int x = POPUP_X, y = LIST_Y + popup_row * ROW_H, l = popup_level;
    char buf[32];
    gfx_target(GFX_BG);
    gfx_clip_reset();
    gfx_fill(x, y, POPUP_W, POPUP_H, RGB(10, 10, 14));
    gfx_frame_rect(x, y, POPUP_W, POPUP_H, dim(course_colors[popup_course], l > 1 ? l : 1));
    if (e && l > 0) {
        snprintf(buf, sizeof buf, "Best  %s \xE2\x98\x85%d", course_names[popup_course], s->level[popup_course]);
        gfx_text(FONT_S, x + 8, y + 5, buf, dim(course_colors[popup_course], l), ALIGN_LEFT, 0);
        snprintf(buf, sizeof buf, "%u", (unsigned)e->score);
        gfx_text(FONT_S, x + POPUP_W - 8, y + 5, buf, dim(RGB(255, 220, 80), l), ALIGN_RIGHT, 0);
        struct {
            const char *label;
            int value, at;
            uint16_t color;
        } cells[4] = {{"Good", e->good, 8, RGB(255, 220, 60)},
                      {"OK", e->ok, 70, RGB(255, 255, 255)},
                      {"Bad", e->bad, 118, RGB(120, 150, 255)},
                      {"Combo", e->max_combo, 166, RGB(255, 255, 255)}};
        for (int i = e->good == SCORE_NO_COUNTS ? 3 : 0; i < 4; i++) { /* an old best has its combo only */
            snprintf(buf, sizeof buf, "%s %d", cells[i].label, cells[i].value);
            gfx_text(FONT_S, x + (e->good == SCORE_NO_COUNTS ? 8 : cells[i].at), y + 22, buf, dim(cells[i].color, l),
                     ALIGN_LEFT, 0);
        }
    }
    gfx_target(GFX_FRAME);
    gfx_bg_changed_rect(x, y, POPUP_W, POPUP_H);
}

static void popup_hide(void) {
    if (popup_row >= 0) {
        stale |= 1u << popup_row; /* the row under it is drawn again */
        popup_row = -1;
    }
    rest_frames = 0;
}

/* once per frame, after the pieces */
static void popup_update(void) {
    bool resting = (mode == BROWSE || mode == COURSE) && sel == rest_sel && !in.pressed && !in.repeat;
    rest_sel = sel;
    if (!resting) {
        popup_hide();
        return;
    }
    const lib_song *s = lib_get_song(sel);
    if (popup_row < 0) {
        if (rest_frames > POPUP_REST) {
            return; /* looked already: this song has no best */
        }
        if (++rest_frames < POPUP_REST || stale) {
            if (rest_frames == POPUP_REST) {
                rest_frames--; /* the pieces are still being drawn: look again next frame */
            }
            return;
        }
        int first = next_best(s, LIB_COURSES - 1); /* the easiest one; a walk through the score table, done once */
        if (first < 0) {
            rest_frames = POPUP_REST + 1;
            return;
        }
        int row = (int)(pos - top);
        popup_row = row + 1 < ROWS ? row + 1 : row - 1;
        popup_course = first;
        popup_only = next_best(s, first) == first;
        popup_level = 0;
        popup_dir = 1;
        popup_timer = 0;
    }
    popup_timer++;
    if (popup_dir && popup_timer % POPUP_STEP_FRAMES == 0) { /* fading in or out */
        popup_level += popup_dir;
        if (popup_level <= 0) { /* dark: on to the next course */
            popup_level = 0;
            popup_course = next_best(s, popup_course);
            popup_dir = 1;
        } else if (popup_level >= POPUP_STEPS) {
            popup_level = POPUP_STEPS;
            popup_dir = 0;
            popup_timer = 0;
        }
        popup_draw();
    } else if (!popup_dir && popup_timer >= POPUP_HOLD && !popup_only) {
        popup_dir = -1;
        popup_timer = 0;
    }
}

/* draws some of the stale pieces: rows first, a couple per frame, then header and footer */
static void draw_pieces(void) {
    if (!stale) {
        return;
    }
    if (lib_song_count() == 0) {
        gfx_target(GFX_BG);
        gfx_clip_reset();
        gfx_fill(0, 0, GFX_W, GFX_H, RGB(16, 18, 28));
        gfx_text(FONT_BUILTIN, GFX_W / 2, 170, "The library has no songs.", RGB(255, 255, 255), ALIGN_CENTER, 0);
        gfx_target(GFX_FRAME);
        gfx_bg_changed();
        stale = 0;
        return;
    }
    if (!base_ready) {
        compose_base();
        gfx_bg_changed();
    }
    gfx_target(GFX_BG);
    gfx_clip_reset();
    int rows_done = 0;
    for (int r = 0; r < ROWS && rows_done < ROWS_PER_FRAME; r++) {
        if (stale >> r & 1) {
            draw_row(r);
            stale &= ~(1u << r);
            rows_done++;
        }
    }
    if (!rows_done && (stale & PIECE_HEADER)) {
        draw_header();
        stale &= ~(uint32_t)PIECE_HEADER;
    } else if (!rows_done && (stale & PIECE_FOOTER)) {
        draw_footer();
        stale &= ~(uint32_t)PIECE_FOOTER;
    }
    gfx_target(GFX_FRAME);
}

/* titles compared byte by byte, ASCII letters without regard to case */
static int title_cmp(const lib_song *a, const lib_song *b) {
    const unsigned char *x = (const unsigned char *)lib_string(a->title), *y = (const unsigned char *)lib_string(b->title);
    for (;; x++, y++) {
        int cx = *x >= 'a' && *x <= 'z' ? *x - 32 : *x, cy = *y >= 'a' && *y <= 'z' ? *y - 32 : *y;
        if (cx != cy || !cx) {
            return cx - cy;
        }
    }
}

/* true if song a is listed after song b */
static bool listed_after(uint32_t a, uint32_t b) {
    const lib_song *x = lib_get_song(a), *y = lib_get_song(b);
    if (order_kind == SORT_DIFFICULTY) {
        int lx = x->level[pick_course(x, order_course)], ly = y->level[pick_course(y, order_course)];
        if (lx != ly) {
            return lx > ly;
        }
    }
    int c = title_cmp(x, y);
    return c ? c > 0 : a > b;
}

/* the list of folder `f`, sorted as the settings say; kept until something it depends on changes */
static void build_order(uint32_t f) {
    if (f == order_folder && order_kind == settings.song_order && order_course == settings.last_course) {
        return;
    }
    const lib_category *cat = lib_get_category(f);
    order_folder = f;
    order_kind = settings.song_order;
    order_course = settings.last_course;
    order_n = cat->song_count > ORDER_MAX ? ORDER_MAX : cat->song_count;
    for (uint32_t i = 0; i < order_n; i++) {
        order[i] = (uint16_t)(cat->first_song + i);
    }
    for (uint32_t gap = order_n / 2; gap > 0; gap /= 2) { /* Shell sort: a folder has up to a few hundred songs */
        for (uint32_t i = gap; i < order_n; i++) {
            uint16_t v = order[i];
            uint32_t j = i;
            for (; j >= gap && listed_after(order[j - gap], v); j -= gap) {
                order[j] = order[j - gap];
            }
            order[j] = v;
        }
    }
}

/* cursor to a song; its folder becomes the open one */
static void move_to(uint32_t index) {
    uint32_t count = lib_song_count();
    if (count == 0) {
        return;
    }
    sel = index % count;
    uint32_t listed = order_folder;
    build_order(lib_get_song(sel)->category);
    pos = 0;
    for (uint32_t i = 0; i < order_n; i++) {
        if (order[i] == sel) {
            pos = i;
            break;
        }
    }
    sel = order[pos];
    uint32_t page_top = pos / ROWS * ROWS;
    if (page_top != top || listed != order_folder) {
        top = page_top;
        stale |= (1u << ROWS) - 1;
    }
    preview_request(sel);
}

/* cursor to a folder of the folder list */
static void folder_to(uint32_t index) {
    folder = index % lib_category_count();
    uint32_t page_top = folder / ROWS * ROWS;
    if (page_top != folder_top) {
        folder_top = page_top;
        stale |= (1u << ROWS) - 1;
    }
}

/* the folder list, with the cursor on the folder that was open */
static void close_folder(void) {
    preview_stop();
    mode = FOLDERS;
    folder_top = 0xFFFFFFFFu;
    folder_to(lib_get_song(sel)->category);
    stale = PIECE_ALL;
}

/* into a folder, at song `index` */
static void open_folder(uint32_t index) {
    mode = BROWSE;
    top = 0xFFFFFFFFu;
    move_to(index);
    stale = PIECE_ALL;
}

/* into folder `f` at the top of its list */
static void open_folder_top(uint32_t f) {
    build_order(f);
    open_folder(order_n ? order[0] : lib_get_category(f)->first_song);
}

/* Where the cursor was when the settings were opened: coming back, it is there again,
 * not on the song played last. */
static bool place_kept, place_in_folders;
static uint32_t place_sel, place_folder;

static void to_settings(void) {
    place_kept = true;
    place_in_folders = mode == FOLDERS;
    place_sel = sel;
    place_folder = folder;
    preview_stop();
    app_goto(&scene_settings);
}

static void select_enter(void) {
    song_unload();
    pf_hit_auto(false);
    settings_apply_audio();
    uint32_t count = lib_song_count();
    uint32_t start = 0;
    if (settings.library_id == lib_build_id() && settings.last_song < count) {
        start = settings.last_song;
    }
    bool back = place_kept && place_sel < count; /* back from the settings */
    place_kept = false;
    if (back) {
        start = place_sel;
    }
    if (pf_option("song")) { /* test hook: start on this song */
        uint32_t wanted = 0;
        for (const char *d = pf_option("song"); *d >= '0' && *d <= '9'; d++) {
            wanted = wanted * 10 + (uint32_t)(*d - '0');
        }
        start = wanted < count ? wanted : start;
    }
    mode = BROWSE;
    top = 0xFFFFFFFFu;
    popup_row = -1;
    rest_sel = 0xFFFFFFFFu;
    order_folder = 0xFFFFFFFFu; /* the order setting or the scores may have changed */
    move_to(start);
    if (back && place_in_folders && place_folder < lib_category_count()) { /* it was the folder list */
        preview_stop();
        mode = FOLDERS;
        folder_top = 0xFFFFFFFFu;
        folder_to(place_folder);
    }
    stale = PIECE_ALL;
    gfx_target(GFX_BG);
    gfx_clip_reset();
    if (base_ready) { /* the backdrop at once; the pieces follow over the next frames */
        gfx_bg_write(0, 0, GFX_W, GFX_H, base);
    } else {
        gfx_fill(0, 0, GFX_W, GFX_H, RGB(0, 0, 0));
    }
    gfx_target(GFX_FRAME);
    gfx_bg_changed();
}

static void remember(void) {
    settings.library_id = lib_build_id();
    settings.last_song = sel;
    settings.last_course = (uint8_t)course;
    settings_save();
}

static void select_update(void) {
    uint32_t count = lib_song_count();
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    if (count == 0) {
        draw_pieces();
        return;
    }
    const lib_song *s = lib_get_song(sel);
    switch (mode) {
    case FOLDERS:
        if (in.repeat & PF_BTN_DOWN) {
            folder_to(folder + 1);
            ui_sound_move();
        } else if (in.repeat & PF_BTN_UP) {
            folder_to(folder + lib_category_count() - 1);
            ui_sound_move();
        } else if (in.pressed & PF_BTN_R1) {
            folder_to(folder + ROWS < lib_category_count() ? folder + ROWS : lib_category_count() - 1);
        } else if (in.pressed & PF_BTN_L1) {
            folder_to(folder >= ROWS ? folder - ROWS : 0);
        } else if (in.pressed & (PF_BTN_A | PF_BTN_RIGHT | PF_KEY_DON_L | PF_KEY_DON_R)) {
            /* back at the song it was left at if this is the folder that was open */
            if (s->category == folder) {
                open_folder(sel);
            } else {
                open_folder_top(folder);
            }
            ui_sound_ok();
        } else if (in.pressed & PF_BTN_X) {
            open_folder(rng % count);
            ui_sound_move();
        } else if (in.pressed & PF_BTN_SELECT) {
            to_settings();
        }
        break;
    case BROWSE:
        preview_poll();
        if (in.repeat & PF_BTN_DOWN) { /* up and down stay inside the folder */
            move_to(order[(pos + 1) % order_n]);
            ui_sound_move();
        } else if (in.repeat & PF_BTN_UP) {
            move_to(order[(pos + order_n - 1) % order_n]);
            ui_sound_move();
        } else if (in.pressed & PF_BTN_B) {
            close_folder();
            ui_sound_move();
        } else if (in.repeat & (PF_BTN_RIGHT | PF_BTN_LEFT)) {
            uint32_t cats = lib_category_count();
            uint32_t c = s->category;
            if (in.repeat & PF_BTN_RIGHT) {
                c = (c + 1) % cats;
            } else if (pos == 0) {
                c = (c + cats - 1) % cats; /* at the top of a folder: go to the one before */
            }
            open_folder_top(c);
            ui_sound_move();
        } else if (in.pressed & PF_BTN_R1) { /* a page down or up, inside the folder */
            move_to(order[pos + ROWS < order_n ? pos + ROWS : order_n - 1]);
        } else if (in.pressed & PF_BTN_L1) {
            move_to(order[pos >= ROWS ? pos - ROWS : 0]);
        } else if (in.pressed & PF_BTN_X) {
            move_to(rng % count);
            ui_sound_move();
        } else if (in.pressed & (PF_BTN_A | PF_KEY_DON_L | PF_KEY_DON_R)) {
            course = pick_course(s, settings.last_course);
            mode = COURSE;
            stale |= PIECE_FOOTER;
            ui_sound_ok();
        } else if (in.pressed & PF_BTN_SELECT) {
            to_settings();
        }
        break;
    case COURSE:
        preview_poll();
        if (in.repeat & (PF_BTN_RIGHT | PF_BTN_LEFT | PF_KEY_KA_L | PF_KEY_KA_R)) {
            int dir = (in.repeat & (PF_BTN_RIGHT | PF_KEY_KA_R)) ? 1 : -1;
            for (int c = course + dir; c >= 0 && c < LIB_COURSES; c += dir) {
                if (has_course(s, c)) {
                    course = c;
                    stale |= PIECE_FOOTER;
                    ui_sound_move();
                    break;
                }
            }
        } else if (in.pressed & PF_BTN_B) {
            mode = BROWSE;
            stale |= PIECE_FOOTER;
        } else if (in.pressed & (PF_BTN_A | PF_KEY_DON_L | PF_KEY_DON_R)) {
            ui_sound_ok();
            remember();
            song_load(sel, course);
            mode = LOADING;
            stale |= PIECE_FOOTER;
        }
        break;
    case LOADING: {
        song_state st = song_poll();
        if (st == SONG_READY) {
            app_goto(&scene_game);
        } else if (st == SONG_ERROR) {
            message = song_error();
            mode = MESSAGE;
            stale |= PIECE_FOOTER;
        }
        break;
    }
    case MESSAGE:
        if (in.pressed) {
            mode = BROWSE;
            stale |= PIECE_FOOTER;
        }
        break;
    }
    /* header and footer follow the cursor: one of them per frame, after any rows */
    if (detail_sel != (mode == FOLDERS ? DETAIL_FOLDER | folder : sel)) {
        stale |= PIECE_HEADER | PIECE_FOOTER;
        detail_sel = mode == FOLDERS ? DETAIL_FOLDER | folder : sel;
    }
    draw_pieces();
    popup_update();
}

/* the cursor: a frame around the selected row, drawn every frame */
static void select_draw(void) {
    uint32_t at = mode == FOLDERS ? folder - folder_top : pos - top;
    if (lib_song_count() == 0 || at >= ROWS) {
        return;
    }
    int y = LIST_Y + (int)at * ROW_H;
    uint16_t c = RGB(255, 220, 80);
    gfx_frame_rect(6, y, GFX_W - 12, ROW_H - 4, c);
    gfx_frame_rect(7, y + 1, GFX_W - 14, ROW_H - 6, c);
}

const scene scene_select = {select_enter, select_update, select_draw};
