/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A basic graphical shell for the UEFI framebuffer (include/vibeos/gui.h).
 *
 * Not a window system. It draws a desktop, a panel, one window with text in
 * it, and a pointer that follows the mouse - which is enough to make the
 * machine show something a person can look at, and enough to prove the pieces
 * underneath it work: a linear framebuffer, a PS/2 mouse on IRQ12, and the
 * timer driving redraws.
 *
 * Two decisions are worth stating.
 *
 * It composites into a back buffer and copies whole rows to the framebuffer.
 * Drawing straight into video memory is visibly wrong on a moving pointer -
 * the erase and the redraw land in different frames, so the cursor flickers
 * and leaves fragments behind. Framebuffer memory is also uncached write-
 * combining, so the read-modify-write that "erase the old cursor" needs is
 * unusually expensive there.
 *
 * It redraws only what changed. A full-screen copy at every tick would spend
 * the whole machine on a pointer that moved three pixels.
 *
 * ## Who touches what (C7)
 *
 * Until C7 this file had no lock and three callers: the init, the timer's
 * repaint, and putc from the console write path - on any core, and not always
 * under the console lock (kmain's hex printer writes a character at a time with
 * none). Two cores in putc could both pass `row < ROWS` before either scrolled,
 * and the second wrote a row past the end of the grid, into whatever the
 * linker put after it. Nothing would have said so.
 *
 * Now there are two kinds of state, with one rule each:
 *
 * - the terminal grid and the counters: under the GUI's own lock, which masks
 *   interrupts. Every access, including the reads;
 * - the back buffer, the framebuffer and the pointer's last position: owned by
 *   whoever holds `g_ticking`, which only the repaint takes, and only by
 *   exchange. The repaint copies the grid under the lock and composes from the
 *   copy outside it, so a console write never waits for a screen to be drawn -
 *   blitting a window to write-combining memory with interrupts off on every
 *   core that wants to print was the alternative.
 */

#include <stdint.h>

#include "vibeos/device.h"
#include "vibeos/font8x8.h"
#include "vibeos/gui.h"
#include "vibeos/vt.h"

#define TERM_MAX_COLS VIBEOS_GUI_TERM_MAX_COLS
#define TERM_MAX_ROWS VIBEOS_GUI_TERM_MAX_ROWS

/* The grid's size, decided by init from the screen: the window fills it below
 * the panel (S7). The arrays are the largest size; these are the part in use.
 * Set once by init, before g_active, and read-only after. */
static uint32_t g_cols = 2u, g_rows = 2u;
#define TERM_COLS g_cols
#define TERM_ROWS g_rows
#define GUARD_WORDS (VIBEOS_GUI_GUARD_BYTES / 8u)

/* Not the frame layer's poison, and not the value the arch used for this canary
 * before C7 either: if this word turns up somewhere it should not, it has to be
 * unambiguous which detector put it there. This project has already read one
 * detector's output as another's. */
#define GUI_GUARD 0x6D1C0FFEE6D1C0FFull

/* Palette. Flat colours on purpose: gradients need blending and blending needs
 * a pixel format contract this code deliberately does not assume beyond
 * 32-bit XRGB, which is what UEFI hands over in practice. */
#define COL_DESKTOP  0x00202A38u
#define COL_PANEL    0x00121820u
#define COL_WINDOW   0x00E8ECF0u
#define COL_TITLE    0x003A6EA5u
#define COL_TEXT     0x00101418u
#define COL_TITLETXT 0x00FFFFFFu
#define COL_CURSOR   0x00FFFFFFu
#define COL_CURSOR_E 0x00000000u

/* ---- the lock ------------------------------------------------------------------ */

/* Its own: not the console's, whose writers call in here, and not the device
 * registry's, which the report runs under. */
static vibeos_dev_lock_t g_lock = VIBEOS_DEV_LOCK("gui");

/* ---- state under the lock ----------------------------------------------------- */

/* A cell: a code point and its two colours, 0x00RRGGBB, with the underline in
 * the foreground's top byte. Code point 0 is an empty cell - erased, or never
 * written - and draws its background only. Until S7 a cell was a byte and a row
 * was a C string, so everything after an erased cell went undrawn: a
 * full-screen program erases and then writes beyond the gap, and whole pieces
 * of FinalCommander's panels were missing. */
typedef struct {
    uint32_t cp;
    uint32_t fg;
    uint32_t bg;
} term_cell_t;

#define CELL_UNDERLINE 0x01000000u
#define CELL_RGB 0x00FFFFFFu

/* What a written character takes from: the pen, set by SGR. `fg_index` is the
 * palette entry the foreground came from, or -1, so that bold can brighten one
 * of the first eight the way xterm does. */
typedef struct {
    uint32_t fg, bg;
    int32_t fg_index;
    uint8_t bold, reverse, underline;
} term_pen_t;

typedef struct {
    uint32_t row, col;
    term_pen_t pen;
} term_saved_t;

static term_cell_t g_term[TERM_MAX_ROWS][TERM_MAX_COLS];   /* quota: none - the screen, not a pool */
static uint32_t g_term_col, g_term_row;
static vibeos_vt_t g_vt;
#define PEN_DEFAULT ((term_pen_t){ COL_TEXT, COL_WINDOW, -1, 0, 0, 0 })
static term_pen_t g_pen = { COL_TEXT, COL_WINDOW, -1, 0, 0, 0 };
static uint32_t g_top, g_bottom = 1u;   /* the scroll region, rows inclusive */
static term_saved_t g_saved;
static int g_cursor_on = 1;
/* The main screen while a program has the alternate one (?1049h). */
static term_cell_t g_main[TERM_MAX_ROWS][TERM_MAX_COLS];   /* quota: none - the screen, not a pool */
static term_saved_t g_main_cursor;
static int g_alt;
/* Rows changed since the repaint last copied them. The repaint used to compose
 * and copy the whole window on every change; with a cell's background drawn
 * for every cell (S7) that was ten thousand cells and a megabyte of
 * write-combining memory per typed character, on the timer, and the machine
 * crawled. */
static uint8_t g_row_dirty[TERM_MAX_ROWS];   /* quota: none - the screen, not a pool */
static int g_term_dirty;
static vibeos_gui_stats_t g_stats;
/* Outside the lock by definition - it counts the writes that could not take it -
 * so it is its own atomic rather than a field the struct copy reads plainly. */
static uint64_t g_reentered;

/* ---- state owned by the repaint (g_ticking) ------------------------------------ */

static volatile int g_ticking;
static term_cell_t g_snap[TERM_MAX_ROWS][TERM_MAX_COLS];   /* quota: none - the screen, not a pool */
static uint32_t g_snap_col, g_snap_row;
static int g_snap_cursor_on = 1;
/* Rows of the copy the next compose draws; all of them when 0 is passed. */
static uint8_t g_snap_rows[TERM_MAX_ROWS];   /* quota: none - the screen, not a pool */
static int32_t g_last_cx = -1, g_last_cy = -1;

/* ---- set once by init, before g_active; read-only after ------------------------ */

static uint32_t *g_fb;
static uint32_t *g_back;
static uint64_t *g_guard;
static uint32_t g_w, g_h;
static uint32_t g_win_x, g_win_y, g_win_w, g_win_h;
static volatile int g_active;
static const char *g_why = "not initialised";

/* 1 when the lock is now held by the caller; 0 when the caller already held it
 * and must not touch the state - a panic printing from inside the GUI. Who
 * holds a lock is the lock provider's to know (vibeos_dev_lock). */
static int gui_lock(void) {
    return vibeos_dev_lock(&g_lock) == 0;
}

static void gui_unlock(void) {
    vibeos_dev_unlock(&g_lock);
}

/* ---- drawing (repaint owner only) --------------------------------------------- */

/* Every clamp compares before it adds. The version before C7 wrote
 * `if (x + w > g_w)`, and a width that had underflowed to 0xFFFFFFFF wrapped
 * `x + w` back below the screen - so the clamp passed a four-billion-pixel
 * rectangle through. */
static int clip(uint32_t x, uint32_t y, uint32_t *w, uint32_t *h) {
    if (x >= g_w || y >= g_h) {
        return 0;
    }
    if (*w > g_w - x) {
        *w = g_w - x;
    }
    if (*h > g_h - y) {
        *h = g_h - y;
    }
    return 1;
}

static void fill_rect(uint32_t *dst, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t colour) {
    uint32_t yy, xx;

    if (!clip(x, y, &w, &h)) {
        return;
    }
    for (yy = 0; yy < h; yy++) {
        uint32_t *row = dst + (uint64_t)(y + yy) * g_w + x;
        for (xx = 0; xx < w; xx++) {
            row[xx] = colour;
        }
    }
}

static void draw_char(uint32_t *dst, uint32_t x, uint32_t y, char c, uint32_t colour) {
    uint32_t row;

    for (row = 0; row < VIBEOS_FONT8X8_HEIGHT; row++) {
        const uint8_t *bits = vibeos_font8x8_row(c, row);
        uint32_t col;
        if (!bits) {
            return;
        }
        for (col = 0; col < 8u; col++) {
            if ((*bits >> col) & 1u) {
                if (x + col < g_w && y + row < g_h) {
                    dst[(uint64_t)(y + row) * g_w + (x + col)] = colour;
                }
            }
        }
    }
}

static void draw_text(uint32_t *dst, uint32_t x, uint32_t y, const char *s, uint32_t colour) {
    while (*s) {
        draw_char(dst, x, y, *s++, colour);
        x += 8u;
    }
}

/* An arrow, as a bitmap. Two colours so it stays visible over both the light
 * window and the dark desktop - a single-colour pointer disappears against
 * whatever it is pointing at, which is the one thing a pointer must not do. */
#define CURSOR_W 8u
#define CURSOR_H 12u
static const uint8_t g_cursor[CURSOR_H] = {
    0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0x0F, 0x1B, 0x19, 0x30, 0x30
};

static void draw_cursor(uint32_t *dst, int32_t cx, int32_t cy) {
    uint32_t row;

    for (row = 0; row < CURSOR_H; row++) {
        uint32_t col;
        for (col = 0; col < CURSOR_W; col++) {
            uint32_t px = (uint32_t)cx + col;
            uint32_t py = (uint32_t)cy + row;
            if (px >= g_w || py >= g_h) {
                continue;
            }
            if ((g_cursor[row] >> col) & 1u) {
                /* Outline first so the arrow reads against any background. */
                dst[(uint64_t)py * g_w + px] = COL_CURSOR;
            } else if (col > 0u && ((g_cursor[row] >> (col - 1u)) & 1u)) {
                dst[(uint64_t)py * g_w + px] = COL_CURSOR_E;
            }
        }
    }
}

static void blit_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t yy;

    if (!clip(x, y, &w, &h)) {
        return;
    }
    for (yy = 0; yy < h; yy++) {
        uint64_t off = (uint64_t)(y + yy) * g_w + x;
        uint32_t xx;
        for (xx = 0; xx < w; xx++) {
            g_fb[off + xx] = g_back[off + xx];
        }
    }
}

/* ---- glyphs beyond the font (repaint owner only) -------------------------------- */

#define CELL_W 8u
#define CELL_H 9u

/* Box drawing, U+2500 to U+257F, as the weight of the line toward each edge of
 * the cell: up, down, left, right, two bits each - none, light, heavy,
 * double. Drawn from lines rather than from a font, which is what makes a
 * panel's frame join from one cell to the next. The dashed forms are drawn
 * solid, the mixed light-and-heavy junctions in one weight, and the diagonals
 * (U+2571-2573) fall back to the font. */
#define BX(u, d, l, r) (uint8_t)(((u) << 6) | ((d) << 4) | ((l) << 2) | (r))
#define LI 1u
#define HV 2u
#define DB 3u
static const uint8_t g_box[0x80] = {
    /* 2500 */ BX(0, 0, LI, LI), BX(0, 0, HV, HV), BX(LI, LI, 0, 0), BX(HV, HV, 0, 0),
    /* 2504 */ BX(0, 0, LI, LI), BX(0, 0, HV, HV), BX(LI, LI, 0, 0), BX(HV, HV, 0, 0),
    /* 2508 */ BX(0, 0, LI, LI), BX(0, 0, HV, HV), BX(LI, LI, 0, 0), BX(HV, HV, 0, 0),
    /* 250C */ BX(0, LI, 0, LI), BX(0, LI, 0, HV), BX(0, HV, 0, LI), BX(0, HV, 0, HV),
    /* 2510 */ BX(0, LI, LI, 0), BX(0, LI, HV, 0), BX(0, HV, LI, 0), BX(0, HV, HV, 0),
    /* 2514 */ BX(LI, 0, 0, LI), BX(LI, 0, 0, HV), BX(HV, 0, 0, LI), BX(HV, 0, 0, HV),
    /* 2518 */ BX(LI, 0, LI, 0), BX(LI, 0, HV, 0), BX(HV, 0, LI, 0), BX(HV, 0, HV, 0),
    /* 251C */ BX(LI, LI, 0, LI), BX(LI, LI, 0, HV), BX(HV, LI, 0, LI), BX(LI, HV, 0, LI),
    /* 2520 */ BX(HV, HV, 0, LI), BX(HV, LI, 0, HV), BX(LI, HV, 0, HV), BX(HV, HV, 0, HV),
    /* 2524 */ BX(LI, LI, LI, 0), BX(LI, LI, HV, 0), BX(HV, LI, LI, 0), BX(LI, HV, LI, 0),
    /* 2528 */ BX(HV, HV, LI, 0), BX(HV, LI, HV, 0), BX(LI, HV, HV, 0), BX(HV, HV, HV, 0),
    /* 252C */ BX(0, LI, LI, LI), BX(0, LI, HV, LI), BX(0, LI, LI, HV), BX(0, LI, HV, HV),
    /* 2530 */ BX(0, HV, LI, LI), BX(0, HV, HV, LI), BX(0, HV, LI, HV), BX(0, HV, HV, HV),
    /* 2534 */ BX(LI, 0, LI, LI), BX(LI, 0, HV, LI), BX(LI, 0, LI, HV), BX(LI, 0, HV, HV),
    /* 2538 */ BX(HV, 0, LI, LI), BX(HV, 0, HV, LI), BX(HV, 0, LI, HV), BX(HV, 0, HV, HV),
    /* 253C */ BX(LI, LI, LI, LI), BX(LI, LI, HV, LI), BX(LI, LI, LI, HV), BX(LI, LI, HV, HV),
    /* 2540 */ BX(HV, LI, LI, LI), BX(LI, HV, LI, LI), BX(HV, HV, LI, LI), BX(HV, LI, HV, LI),
    /* 2544 */ BX(HV, LI, LI, HV), BX(LI, HV, HV, LI), BX(LI, HV, LI, HV), BX(HV, LI, HV, HV),
    /* 2548 */ BX(LI, HV, HV, HV), BX(HV, HV, HV, LI), BX(HV, HV, LI, HV), BX(HV, HV, HV, HV),
    /* 254C */ BX(0, 0, LI, LI), BX(0, 0, HV, HV), BX(LI, LI, 0, 0), BX(HV, HV, 0, 0),
    /* 2550 */ BX(0, 0, DB, DB), BX(DB, DB, 0, 0), BX(0, LI, 0, DB), BX(0, DB, 0, LI),
    /* 2554 */ BX(0, DB, 0, DB), BX(0, LI, DB, 0), BX(0, DB, LI, 0), BX(0, DB, DB, 0),
    /* 2558 */ BX(LI, 0, 0, DB), BX(DB, 0, 0, LI), BX(DB, 0, 0, DB), BX(LI, 0, DB, 0),
    /* 255C */ BX(DB, 0, LI, 0), BX(DB, 0, DB, 0), BX(LI, LI, 0, DB), BX(DB, DB, 0, LI),
    /* 2560 */ BX(DB, DB, 0, DB), BX(LI, LI, DB, 0), BX(DB, DB, LI, 0), BX(DB, DB, DB, 0),
    /* 2564 */ BX(0, LI, DB, DB), BX(0, DB, LI, LI), BX(0, DB, DB, DB), BX(LI, 0, DB, DB),
    /* 2568 */ BX(DB, 0, LI, LI), BX(DB, 0, DB, DB), BX(LI, LI, DB, DB), BX(DB, DB, LI, LI),
    /* 256C */ BX(DB, DB, DB, DB), BX(0, LI, 0, LI), BX(0, LI, LI, 0), BX(LI, 0, LI, 0),
    /* 2570 */ BX(LI, 0, 0, LI), 0, 0, 0,
    /* 2574 */ BX(0, 0, LI, 0), BX(LI, 0, 0, 0), BX(0, 0, 0, LI), BX(0, LI, 0, 0),
    /* 2578 */ BX(0, 0, HV, 0), BX(HV, 0, 0, 0), BX(0, 0, 0, HV), BX(0, HV, 0, 0),
    /* 257C */ BX(0, 0, LI, HV), BX(LI, HV, 0, 0), BX(0, 0, HV, LI), BX(HV, LI, 0, 0),
};

static void put_px(uint32_t x, uint32_t y, uint32_t colour) {
    if (x < g_w && y < g_h) {
        g_back[(uint64_t)y * g_w + x] = colour;
    }
}

/* A line from the centre of the cell toward one edge. Light is one pixel,
 * heavy two, double two lines three apart - the cell is eight by nine, so the
 * centre is column 3 and row 4. A double line runs one pixel past the centre
 * so that its corner closes. */
static void box_arm(uint32_t x, uint32_t y, int vertical, int toward_end, uint32_t weight, uint32_t colour) {
    uint32_t from, to, k, i;
    uint32_t centre = vertical ? 4u : 3u, len = vertical ? CELL_H : CELL_W;
    uint32_t at[2], lines;

    if (weight == 0u) {
        return;
    }
    if (weight == DB) {
        at[0] = vertical ? 2u : 3u;
        at[1] = vertical ? 5u : 6u;
        lines = 2;
        if (vertical) {
            from = toward_end ? 3u : 0u;
            to = toward_end ? len - 1u : 6u;
        } else {
            from = toward_end ? 2u : 0u;
            to = toward_end ? len - 1u : 6u;
        }
    } else {
        at[0] = vertical ? 3u : 4u;
        at[1] = at[0] + 1u;
        lines = weight == HV ? 2u : 1u;
        from = toward_end ? centre : 0u;
        to = toward_end ? len - 1u : centre;
    }
    for (i = 0; i < lines; i++) {
        for (k = from; k <= to; k++) {
            if (vertical) {
                put_px(x + at[i], y + k, colour);
            } else {
                put_px(x + k, y + at[i], colour);
            }
        }
    }
}

static void fill_cell_part(uint32_t x, uint32_t y, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                           uint32_t colour) {
    uint32_t xx, yy;

    for (yy = y0; yy < y1; yy++) {
        for (xx = x0; xx < x1; xx++) {
            put_px(x + xx, y + yy, colour);
        }
    }
}

/* What a code point the font does not have is drawn as: its own shape where
 * one is drawn here, or an ASCII stand-in. 0 means the font has nothing and
 * the cell shows '?'. */
static char glyph_fallback(uint32_t cp) {
    switch (cp) {
        case 0x00A0u: return ' ';
        case 0x00B7u: case 0x2022u: case 0x2219u: return '*';
        case 0x2190u: case 0x25C0u: case 0x25C4u: return '<';
        case 0x2192u: case 0x25B6u: case 0x25BAu: return '>';
        case 0x2191u: case 0x25B2u: return '^';
        case 0x2193u: case 0x25BCu: return 'v';
        case 0x2571u: return '/';
        case 0x2572u: return '\\';
        case 0x2573u: return 'X';
        case 0x2013u: case 0x2014u: case 0x2212u: return '-';
        case 0x2018u: case 0x2019u: return '\'';
        case 0x201Cu: case 0x201Du: return '"';
        case 0x2713u: case 0x2714u: return 'v';
        default: return 0;
    }
}

static void draw_glyph(uint32_t x, uint32_t y, uint32_t cp, uint32_t colour) {
    if (cp >= 0x20u && cp < 0x7Fu) {
        draw_char(g_back, x, y, (char)cp, colour);
        return;
    }
    if (cp >= 0x2500u && cp <= 0x257Fu && g_box[cp - 0x2500u]) {
        uint8_t b = g_box[cp - 0x2500u];

        box_arm(x, y, 1, 0, (b >> 6) & 3u, colour);
        box_arm(x, y, 1, 1, (b >> 4) & 3u, colour);
        box_arm(x, y, 0, 0, (b >> 2) & 3u, colour);
        box_arm(x, y, 0, 1, b & 3u, colour);
        return;
    }
    if (cp >= 0x2580u && cp <= 0x2595u) {
        /* Block elements: halves, eighths, and the three shades. */
        if (cp == 0x2580u) {
            fill_cell_part(x, y, 0, 0, CELL_W, CELL_H / 2u, colour);
        } else if (cp <= 0x2588u) {
            uint32_t h = (CELL_H * (cp - 0x2580u) + 4u) / 8u;
            fill_cell_part(x, y, 0, CELL_H - h, CELL_W, CELL_H, colour);
        } else if (cp <= 0x258Fu) {
            fill_cell_part(x, y, 0, 0, CELL_W - (cp - 0x2588u), CELL_H, colour);
        } else if (cp == 0x2590u) {
            fill_cell_part(x, y, CELL_W / 2u, 0, CELL_W, CELL_H, colour);
        } else if (cp <= 0x2593u) {
            uint32_t xx, yy;

            /* Light, medium, dark: one pixel in four, one in two, three in four. */
            for (yy = 0; yy < CELL_H; yy++) {
                for (xx = 0; xx < CELL_W; xx++) {
                    int odd_x = (int)(xx & 1u), odd_y = (int)(yy & 1u);
                    int on = cp == 0x2591u ? (!odd_x && !odd_y)
                             : cp == 0x2592u ? (odd_x == odd_y)
                                             : !(odd_x && odd_y);

                    if (on) {
                        put_px(x + xx, y + yy, colour);
                    }
                }
            }
        } else if (cp == 0x2594u) {
            fill_cell_part(x, y, 0, 0, CELL_W, 1u, colour);
        } else {
            fill_cell_part(x, y, CELL_W - 1u, 0, CELL_W, CELL_H, colour);
        }
        return;
    }
    if (cp == 0x2026u) {
        /* The ellipsis a program truncates a name with: three dots. */
        put_px(x + 1u, y + 6u, colour);
        put_px(x + 4u, y + 6u, colour);
        put_px(x + 7u, y + 6u, colour);
        return;
    }
    {
        char f = glyph_fallback(cp);

        draw_char(g_back, x, y, f ? f : '?', colour);
    }
}

/* Repaint the window's text area from a copy of the grid. The window is at
 * least the minimum screen's worth of pixels (vibeos_gui_init refuses smaller),
 * so none of these subtractions can go below zero. Every cell is drawn,
 * background first: an empty one is a background, not the end of the row. */
static void compose_terminal(term_cell_t grid[TERM_MAX_ROWS][TERM_MAX_COLS], const uint8_t *rows) {
    uint32_t row;
    uint32_t right = g_win_x + g_win_w - 4u, bottom = g_win_y + g_win_h - 2u;

    if (!rows) {
        fill_rect(g_back, g_win_x + 4u, g_win_y + 22u,
                  g_win_w - 8u, g_win_h - 26u, COL_WINDOW);
    }
    for (row = 0; row < TERM_ROWS; row++) {
        uint32_t col;
        uint32_t y = g_win_y + 24u + row * CELL_H;
        if (y + CELL_H > bottom) {
            break;
        }
        if (rows && !rows[row]) {
            continue;
        }
        if (rows) {
            fill_rect(g_back, g_win_x + 4u, y, g_win_w - 8u, CELL_H, COL_WINDOW);
        }
        for (col = 0; col < TERM_COLS; col++) {
            const term_cell_t *c = &grid[row][col];
            uint32_t x = g_win_x + 8u + col * CELL_W;

            if (x + CELL_W > right) {
                break;
            }
            if (c->bg != COL_WINDOW) {
                fill_rect(g_back, x, y, CELL_W, CELL_H, c->bg);
            }
            /* A control character is stored and never drawn, as before. */
            if (c->cp >= 0x20u) {
                draw_glyph(x, y, c->cp, c->fg & CELL_RGB);
            }
            if (c->fg & CELL_UNDERLINE) {
                fill_rect(g_back, x, y + CELL_H - 1u, CELL_W, 1u, c->fg & CELL_RGB);
            }
        }
    }
    /* Where typing goes: a line editor is unusable without it. On the row
     * between two lines of text, so it hides no character - in the colour of
     * the text under it, so it shows on whatever background a program set.
     * A program that hides the cursor (?25l) draws its own. */
    if (g_snap_cursor_on && g_snap_row < TERM_ROWS && g_snap_col < TERM_COLS &&
        (!rows || rows[g_snap_row])) {
        uint32_t y = g_win_y + 24u + g_snap_row * CELL_H;
        uint32_t x = g_win_x + 8u + g_snap_col * CELL_W;

        if (y + CELL_H <= bottom && x + CELL_W <= right) {
            fill_rect(g_back, x, y + 8u, CELL_W, 1u, grid[g_snap_row][g_snap_col].fg & CELL_RGB);
        }
    }
}

static void compose_desktop(void) {
    /* The whole screen below the panel (S7): the grid was sized to fit it by
     * init, so the window is the grid's size, at the panel's edge - and on a
     * screen larger than the largest grid, centred. */
    g_win_w = TERM_COLS * CELL_W + 16u;
    g_win_h = TERM_ROWS * CELL_H + 32u;
    g_win_x = g_w > g_win_w ? (g_w - g_win_w) / 2u : 0u;
    g_win_y = g_h > 24u + g_win_h ? 24u + (g_h - 24u - g_win_h) / 2u : 24u;
    if (g_win_w > g_w - g_win_x) {
        g_win_w = g_w - g_win_x;
    }
    if (g_win_h > g_h - g_win_y) {
        g_win_h = g_h - g_win_y;
    }

    fill_rect(g_back, 0, 0, g_w, g_h, COL_DESKTOP);
    fill_rect(g_back, 0, 0, g_w, 24u, COL_PANEL);
    draw_text(g_back, 8u, 8u, "VibeOS", COL_TITLETXT);

    fill_rect(g_back, g_win_x, g_win_y, g_win_w, g_win_h, COL_WINDOW);
    fill_rect(g_back, g_win_x, g_win_y, g_win_w, 20u, COL_TITLE);
    draw_text(g_back, g_win_x + 6u, g_win_y + 6u, "console", COL_TITLETXT);
    compose_terminal(g_snap, 0);
}

/* How many canary words are wrong. Read by the repaint, which owns nothing the
 * canary protects against, so it can be read without the lock: nobody writes
 * it but init. */
static uint64_t guard_count_broken(void) {
    uint64_t n = 0;
    uint32_t i;

    for (i = 0; i < GUARD_WORDS; i++) {
        if (g_guard[i] != GUI_GUARD) {
            n++;
        }
    }
    return n;
}

/* ---- the terminal (under the lock) -------------------------------------------- */

/* xterm's sixteen, which is what a program that names a colour by number
 * expects to get. */
static const uint32_t g_ansi[16] = {
    0x000000u, 0xCD0000u, 0x00CD00u, 0xCDCD00u, 0x0000EEu, 0xCD00CDu, 0x00CDCDu, 0xE5E5E5u,
    0x7F7F7Fu, 0xFF0000u, 0x00FF00u, 0xFFFF00u, 0x5C5CFFu, 0xFF00FFu, 0x00FFFFu, 0xFFFFFFu,
};

/* One of the 256: the sixteen, a six-by-six-by-six cube, and 24 greys. */
static uint32_t colour_256(uint32_t i) {
    static const uint8_t level[6] = { 0, 95, 135, 175, 215, 255 };

    if (i < 16u) {
        return g_ansi[i];
    }
    if (i < 232u) {
        i -= 16u;
        return ((uint32_t)level[i / 36u] << 16) | ((uint32_t)level[(i / 6u) % 6u] << 8) | level[i % 6u];
    }
    if (i < 256u) {
        uint32_t g = 8u + 10u * (i - 232u);
        return (g << 16) | (g << 8) | g;
    }
    return COL_TEXT;
}

/* A cell as the pen writes it now. */
static term_cell_t pen_cell(uint32_t cp) {
    term_cell_t c;
    uint32_t fg = g_pen.fg, bg = g_pen.bg, t;

    if (g_pen.bold && g_pen.fg_index >= 0 && g_pen.fg_index < 8) {
        fg = g_ansi[g_pen.fg_index + 8];
    }
    if (g_pen.reverse) {
        t = fg;
        fg = bg;
        bg = t;
    }
    c.cp = cp;
    c.fg = (fg & CELL_RGB) | (g_pen.underline ? CELL_UNDERLINE : 0u);
    c.bg = bg & CELL_RGB;
    return c;
}

/* An empty cell: what erasing leaves, in the pen's background - xterm's
 * "background colour erase", which a program that clears to a colour relies
 * on. With the default pen it is the cell as it always was. */
static term_cell_t blank_cell(void) {
    term_cell_t c = pen_cell(0u);

    c.fg &= CELL_RGB;
    return c;
}

/* Write one cell, refusing one outside the grid. Under the lock this cannot
 * happen, and the count is how that is known rather than assumed: before C7 the
 * same write, from two cores at once, landed past the end of the array. */
static void term_put(uint32_t row, uint32_t col, term_cell_t c) {
    if (row >= TERM_ROWS || col >= TERM_COLS) {
        g_stats.term_overrun++;
        return;
    }
    g_term[row][col] = c;
    g_row_dirty[row] = 1;
}

static void term_erase_cells(uint32_t row, uint32_t from, uint32_t to) {
    uint32_t k;
    term_cell_t b = blank_cell();

    for (k = from; k < to && k < TERM_COLS; k++) {
        term_put(row, k, b);
    }
}

static void term_erase(uint32_t row, uint32_t from) {
    term_erase_cells(row, from, TERM_COLS);
}

static void term_copy_row(uint32_t to, uint32_t from) {
    uint32_t cc;

    for (cc = 0; cc < TERM_COLS; cc++) {
        g_term[to][cc] = g_term[from][cc];
    }
    g_row_dirty[to] = 1;
}

/* The rows `top` to `bottom` moved up by n, the bottom n left blank. Counted
 * once per call, as the scroll of a full screen always was. */
static void term_scroll_up(uint32_t top, uint32_t bottom, uint32_t n) {
    uint32_t r;

    if (top > bottom || bottom >= TERM_ROWS) {
        return;
    }
    if (n > bottom - top + 1u) {
        n = bottom - top + 1u;
    }
    for (r = top; r + n <= bottom; r++) {
        term_copy_row(r, r + n);
    }
    for (r = bottom + 1u - n; r <= bottom; r++) {
        term_erase(r, 0u);
    }
    g_stats.scrolls++;
}

static void term_scroll_down(uint32_t top, uint32_t bottom, uint32_t n) {
    uint32_t r;

    if (top > bottom || bottom >= TERM_ROWS) {
        return;
    }
    if (n > bottom - top + 1u) {
        n = bottom - top + 1u;
    }
    for (r = bottom; r >= top + n; r--) {
        term_copy_row(r, r - n);
    }
    for (r = top; r < top + n; r++) {
        term_erase(r, 0u);
    }
}

/* Down a row: at the bottom of the region the region scrolls, below it the
 * cursor stops at the last row. With no region set that is the whole screen,
 * and the newline it always was. */
static void term_line_down(void) {
    if (g_term_row == g_bottom) {
        term_scroll_up(g_top, g_bottom, 1u);
    } else if (g_term_row + 1u < TERM_ROWS) {
        g_term_row++;
    }
}

/* The cursor moved, by a sequence. A row the cursor is now past the end of gets
 * spaces up to it, so that the row as text (vibeos_gui_term_row) reads on
 * through to what is written there. */
static void term_moved(uint32_t row, uint32_t col) {
    uint32_t k;

    g_term_row = row < TERM_ROWS ? row : TERM_ROWS - 1u;
    /* TERM_COLS - 1 is where the terminal wraps, never where it writes. */
    g_term_col = col < TERM_COLS - 1u ? col : TERM_COLS - 2u;
    for (k = 0; k < g_term_col; k++) {
        if (g_term[g_term_row][k].cp == 0u) {
            g_term[g_term_row][k].cp = ' ';
        }
    }
}

/* SGR: colours and attributes, parameter by parameter. 38 and 48 take their
 * colour from the parameters after them, by palette index (5) or as red,
 * green and blue (2). An SGR with no parameter is a reset. */
static void term_sgr(const vibeos_vt_t *vt) {
    uint32_t i, count = (uint32_t)vt->count + 1u;

    i = 0;
    while (i < count && i < VIBEOS_VT_PARAMS) {
        uint32_t p = vt->param[i], used = 1u;   /* parameters this one takes */

        if (p == 0u) {
            g_pen = PEN_DEFAULT;
        } else if (p == 1u) {
            g_pen.bold = 1;
        } else if (p == 4u) {
            g_pen.underline = 1;
        } else if (p == 7u) {
            g_pen.reverse = 1;
        } else if (p == 22u) {
            g_pen.bold = 0;
        } else if (p == 24u) {
            g_pen.underline = 0;
        } else if (p == 27u) {
            g_pen.reverse = 0;
        } else if (p >= 30u && p <= 37u) {
            g_pen.fg = g_ansi[p - 30u];
            g_pen.fg_index = (int32_t)(p - 30u);
        } else if (p == 39u) {
            g_pen.fg = COL_TEXT;
            g_pen.fg_index = -1;
        } else if (p >= 40u && p <= 47u) {
            g_pen.bg = g_ansi[p - 40u];
        } else if (p == 49u) {
            g_pen.bg = COL_WINDOW;
        } else if (p >= 90u && p <= 97u) {
            g_pen.fg = g_ansi[p - 90u + 8u];
            g_pen.fg_index = -1;
        } else if (p >= 100u && p <= 107u) {
            g_pen.bg = g_ansi[p - 100u + 8u];
        } else if (p == 38u || p == 48u) {
            uint32_t colour;

            if (i + 2u < count && vt->param[i + 1u] == 5u) {
                colour = colour_256(vt->param[i + 2u]);
                used = 3u;
            } else if (i + 4u < count && vt->param[i + 1u] == 2u) {
                colour = ((uint32_t)(vt->param[i + 2u] & 0xFFu) << 16) |
                         ((uint32_t)(vt->param[i + 3u] & 0xFFu) << 8) | (vt->param[i + 4u] & 0xFFu);
                used = 5u;
            } else {
                break;   /* malformed: the rest cannot be read in step */
            }
            if (p == 38u) {
                g_pen.fg = colour;
                g_pen.fg_index = -1;
            } else {
                g_pen.bg = colour;
            }
        }
        /* Italic, blink, strike-through and the rest: nothing to draw them with. */
        i += used;
    }
}

static void term_save(term_saved_t *s) {
    s->row = g_term_row;
    s->col = g_term_col;
    s->pen = g_pen;
}

static void term_restore(const term_saved_t *s) {
    g_pen = s->pen;
    term_moved(s->row, s->col);
}

static void term_clear_all(void) {
    uint32_t r;

    for (r = 0; r < TERM_ROWS; r++) {
        term_erase(r, 0u);
    }
}

/* Insert or delete n cells at the cursor, the rest of the row moving. */
static void term_shift_cells(uint32_t n, int insert) {
    uint32_t row = g_term_row, col = g_term_col, k;
    term_cell_t b = blank_cell();

    if (n > TERM_COLS - col) {
        n = TERM_COLS - col;
    }
    if (insert) {
        for (k = TERM_COLS - 1u; k >= col + n; k--) {
            g_term[row][k] = g_term[row][k - n];
        }
        for (k = col; k < col + n; k++) {
            term_put(row, k, b);
        }
    } else {
        for (k = col; k + n < TERM_COLS; k++) {
            g_term[row][k] = g_term[row][k + n];
        }
        for (k = TERM_COLS - n; k < TERM_COLS; k++) {
            term_put(row, k, b);
        }
    }
}

/* What a sequence asks of the grid. Every number is compared before it is used:
 * they come from whatever a program printed. */
static void term_op(vibeos_vt_op_t op, uint32_t n, uint32_t m) {
    uint32_t r;

    switch (op) {
        case VIBEOS_VT_UP:
            term_moved(n < g_term_row ? g_term_row - n : 0u, g_term_col);
            break;
        case VIBEOS_VT_DOWN:
            term_moved(n < TERM_ROWS - g_term_row ? g_term_row + n : TERM_ROWS - 1u, g_term_col);
            break;
        case VIBEOS_VT_RIGHT:
            term_moved(g_term_row, n < TERM_COLS ? g_term_col + n : TERM_COLS);
            break;
        case VIBEOS_VT_LEFT:
            term_moved(g_term_row, n < g_term_col ? g_term_col - n : 0u);
            break;
        case VIBEOS_VT_GOTO:
            term_moved(n, m);
            break;
        case VIBEOS_VT_COLUMN:
            term_moved(g_term_row, n);
            break;
        case VIBEOS_VT_ROW:
            term_moved(n, g_term_col);
            break;
        case VIBEOS_VT_ERASE_LINE:
            term_erase(g_term_row, g_term_col);
            break;
        case VIBEOS_VT_ERASE_LINE_LEFT:
            term_erase_cells(g_term_row, 0u, g_term_col + 1u);
            break;
        case VIBEOS_VT_ERASE_LINE_ALL:
            term_erase(g_term_row, 0u);
            break;
        case VIBEOS_VT_ERASE_BELOW:
            term_erase(g_term_row, g_term_col);
            for (r = g_term_row + 1u; r < TERM_ROWS; r++) {
                term_erase(r, 0u);
            }
            break;
        case VIBEOS_VT_ERASE_ABOVE:
            for (r = 0; r < g_term_row; r++) {
                term_erase(r, 0u);
            }
            term_erase_cells(g_term_row, 0u, g_term_col + 1u);
            break;
        case VIBEOS_VT_ERASE_ALL:
            term_clear_all();
            break;
        case VIBEOS_VT_ERASE_CHARS:
            term_erase_cells(g_term_row, g_term_col, n < TERM_COLS ? g_term_col + n : TERM_COLS);
            break;
        case VIBEOS_VT_INSERT_CHARS:
            term_shift_cells(n, 1);
            break;
        case VIBEOS_VT_DELETE_CHARS:
            term_shift_cells(n, 0);
            break;
        case VIBEOS_VT_INSERT_LINES:
            /* Only inside the region, from the cursor's row down. */
            if (g_term_row >= g_top && g_term_row <= g_bottom) {
                term_scroll_down(g_term_row, g_bottom, n);
                g_term_col = 0;
            }
            break;
        case VIBEOS_VT_DELETE_LINES:
            if (g_term_row >= g_top && g_term_row <= g_bottom) {
                term_scroll_up(g_term_row, g_bottom, n);
                g_term_col = 0;
            }
            break;
        case VIBEOS_VT_SCROLL_UP:
            term_scroll_up(g_top, g_bottom, n);
            break;
        case VIBEOS_VT_SCROLL_DOWN:
            term_scroll_down(g_top, g_bottom, n);
            break;
        case VIBEOS_VT_REGION: {
            /* One-based as written; 0 for either end is its default. A region
             * of one row or less, or past the screen, is refused as xterm
             * refuses it, and the cursor goes home either way. */
            uint32_t top = n ? n - 1u : 0u, bottom = m ? m - 1u : TERM_ROWS - 1u;

            if (bottom >= TERM_ROWS) {
                bottom = TERM_ROWS - 1u;
            }
            if (top < bottom) {
                g_top = top;
                g_bottom = bottom;
            }
            term_moved(0u, 0u);
            break;
        }
        case VIBEOS_VT_SAVE:
            term_save(&g_saved);
            break;
        case VIBEOS_VT_RESTORE:
            term_restore(&g_saved);
            break;
        case VIBEOS_VT_INDEX:
            term_line_down();
            break;
        case VIBEOS_VT_NEXT_LINE:
            term_line_down();
            g_term_col = 0;
            break;
        case VIBEOS_VT_REVERSE_INDEX:
            if (g_term_row == g_top) {
                term_scroll_down(g_top, g_bottom, 1u);
            } else if (g_term_row > 0u) {
                g_term_row--;
            }
            break;
        case VIBEOS_VT_CURSOR_SHOW:
            g_cursor_on = 1;
            break;
        case VIBEOS_VT_CURSOR_HIDE:
            g_cursor_on = 0;
            break;
        case VIBEOS_VT_ALT_ON:
            if (!g_alt) {
                for (r = 0; r < TERM_ROWS; r++) {
                    uint32_t cc;
                    for (cc = 0; cc < TERM_COLS; cc++) {
                        g_main[r][cc] = g_term[r][cc];
                    }
                }
                term_save(&g_main_cursor);
                g_alt = 1;
                term_clear_all();
            }
            break;
        case VIBEOS_VT_ALT_OFF:
            if (g_alt) {
                for (r = 0; r < TERM_ROWS; r++) {
                    uint32_t cc;
                    for (cc = 0; cc < TERM_COLS; cc++) {
                        g_term[r][cc] = g_main[r][cc];
                    }
                    g_row_dirty[r] = 1;
                }
                g_alt = 0;
                g_top = 0;
                g_bottom = TERM_ROWS - 1u;
                term_restore(&g_main_cursor);
            }
            break;
        case VIBEOS_VT_RESET:
            g_pen = PEN_DEFAULT;
            g_top = 0;
            g_bottom = TERM_ROWS - 1u;
            g_cursor_on = 1;
            term_clear_all();
            term_moved(0u, 0u);
            break;
        case VIBEOS_VT_SGR:
            term_sgr(&g_vt);
            break;
        default:
            break;
    }
}

/* One character into the terminal grid. Called from the console write path, so
 * it does the least possible work: it only marks the window dirty, and the
 * repaint happens on the timer where a full recompose is affordable. */
void vibeos_gui_putc(char c) {
    vibeos_vt_op_t op;
    uint32_t vn, vm;

    if (!vibeos_gui_active()) {
        return;
    }
    if (!gui_lock()) {
        __atomic_fetch_add(&g_reentered, 1u, __ATOMIC_RELAXED);
        return;
    }
    op = vibeos_vt_feed(&g_vt, c, &vn, &vm);
    if (op != VIBEOS_VT_PRINT) {
        if (op != VIBEOS_VT_NONE) {
            term_op(op, vn, vm);
            g_term_dirty = 1;
        }
        gui_unlock();
        return;
    }
    /* The parser's code point, not the byte: a UTF-8 character arrives with
     * its last byte. */
    if (vn == '\r') {
        g_term_col = 0;
    } else if (vn == '\n') {
        g_term_col = 0;
        term_line_down();
    } else if (vn == '\b') {
        if (g_term_col > 0u) {
            g_term_col--;
            term_put(g_term_row, g_term_col, blank_cell());
        }
    } else {
        if (g_term_col >= TERM_COLS - 1u) {
            g_term_col = 0;
            term_line_down();
        }
        term_put(g_term_row, g_term_col, pen_cell(vn));
        g_term_col++;
        g_stats.chars++;
    }
    g_term_dirty = 1;
    gui_unlock();
}

/* ---- init ---------------------------------------------------------------------- */

int vibeos_gui_init(uint64_t fb_base, uint32_t width, uint32_t height,
                    void *back, uint64_t back_bytes) {
    uint32_t i;

    if (vibeos_gui_active()) {
        g_why = "already active";
        return -1;
    }
    if (fb_base == 0u || width == 0u || height == 0u) {
        g_why = "no framebuffer";
        return -1;
    }
    if (width > VIBEOS_GUI_MAX_W || height > VIBEOS_GUI_MAX_H) {
        g_why = "screen larger than the back buffer is sized for";
        return -1;
    }
    if (width < VIBEOS_GUI_MIN_W || height < VIBEOS_GUI_MIN_H) {
        g_why = "screen smaller than the desktop can be laid out on";
        return -1;
    }
    if (!back || ((uintptr_t)back & 7u) != 0u) {
        g_why = "back buffer missing or not 8-byte aligned";
        return -1;
    }
    if (back_bytes < VIBEOS_GUI_BACK_BYTES(width, height)) {
        g_why = "back buffer too small for the screen and its canary";
        return -1;
    }
    g_fb = (uint32_t *)(uintptr_t)fb_base;
    g_back = (uint32_t *)back;
    g_guard = (uint64_t *)(void *)(g_back + VIBEOS_GUI_GUARD_PIXEL(width, height));
    for (i = 0; i < GUARD_WORDS; i++) {
        g_guard[i] = GUI_GUARD;
    }
    g_w = width;
    g_h = height;
    /* As many cells as fit below the panel, inside the window's border and
     * title, and no more than the arrays hold. The smallest screen init
     * accepts still gives several of each. */
    g_cols = (width - 16u) / CELL_W;
    _Static_assert(VIBEOS_GUI_MIN_H > 24u + 32u + CELL_H, "init's minimum leaves a row below the panel");
    g_rows = (height - 24u - 32u) / CELL_H;
    if (g_cols > TERM_MAX_COLS) {
        g_cols = TERM_MAX_COLS;
    }
    if (g_rows > TERM_MAX_ROWS) {
        g_rows = TERM_MAX_ROWS;
    }
    if (g_cols < 2u) {
        g_cols = 2u;
    }
    if (g_rows < 2u) {
        g_rows = 2u;
    }
    g_top = 0;
    g_bottom = g_rows - 1u;

    /* A cell of zeros is black on black now that a cell has colours: the grid
     * starts as empty cells in the window's own, which is what it showed. */
    {
        uint32_t r, c;

        for (r = 0; r < TERM_ROWS; r++) {
            for (c = 0; c < TERM_COLS; c++) {
                g_term[r][c] = (term_cell_t){ 0, COL_TEXT, COL_WINDOW };
                g_snap[r][c] = g_term[r][c];
            }
        }
    }
    compose_desktop();
    blit_rect(0, 0, g_w, g_h);
    g_last_cx = -1;
    g_last_cy = -1;
    g_why = "ok";
    /* Published last: putc and the repaint read nothing of the above until
     * this says it is there. */
    __atomic_store_n(&g_active, 1, __ATOMIC_RELEASE);
    return 0;
}

const char *vibeos_gui_why(void) {
    return g_why;
}

int vibeos_gui_active(void) {
    return __atomic_load_n(&g_active, __ATOMIC_ACQUIRE);
}

/* ---- the repaint ---------------------------------------------------------------- */

/* Called from the timer. Repaints only the two small areas that can have
 * changed: where the pointer was, and where it is now - and the window, when
 * the text in it did. */
void vibeos_gui_tick(void) {
    int32_t cx = 0, cy = 0;
    uint32_t buttons = 0;
    int took = 0, moved = 0;
    uint64_t broken;

    if (!vibeos_gui_active()) {
        return;
    }
    if (__atomic_exchange_n(&g_ticking, 1, __ATOMIC_ACQUIRE)) {
        if (gui_lock()) {
            g_stats.ticks_overlapped++;
            gui_unlock();
        }
        return;
    }

    /* The grid, copied under the lock; drawn from the copy outside it. */
    if (gui_lock()) {
        if (g_term_dirty) {
            uint32_t r, c;

            /* Only the rows that changed, and the rows the cursor left and
             * reached: its underline is drawn with the row. */
            for (r = 0; r < TERM_ROWS; r++) {
                if (!g_row_dirty[r]) {
                    continue;
                }
                for (c = 0; c < TERM_COLS; c++) {
                    g_snap[r][c] = g_term[r][c];
                }
                g_row_dirty[r] = 0;
                g_snap_rows[r] = 1;
            }
            if (g_snap_row != g_term_row || g_snap_col != g_term_col || g_snap_cursor_on != g_cursor_on) {
                if (g_snap_row < TERM_ROWS) {
                    g_snap_rows[g_snap_row] = 1;
                }
                if (g_term_row < TERM_ROWS) {
                    g_snap_rows[g_term_row] = 1;
                }
            }
            g_snap_col = g_term_col;
            g_snap_row = g_term_row;
            g_snap_cursor_on = g_cursor_on;
            g_term_dirty = 0;
            took = 1;
        }
        gui_unlock();
    }
    /* Text before the pointer, so the pointer is drawn on top of current
     * content rather than being erased by it. Repainted whether or not there is
     * a pointer: before C7 a machine with no mouse returned first and never
     * showed any console text at all. */
    if (took) {
        uint32_t r;

        compose_terminal(g_snap, g_snap_rows);
        for (r = 0; r < TERM_ROWS; r++) {
            if (g_snap_rows[r]) {
                blit_rect(g_win_x, g_win_y + 24u + r * CELL_H, g_win_w, CELL_H);
                g_snap_rows[r] = 0;
            }
        }
    }

    /* Whichever input device provides a pointer; the display does not know or
     * care that it is a PS/2 mouse.
     *
     * Redrawn when it moved, and after the text was repainted even if it did
     * not: the window's blit may have covered it. This used to forget the old
     * position instead ("whatever was under the pointer is gone"), which is
     * true only for a pointer inside the window - one out on the desktop was
     * never restored and left a cursor-shaped fragment on the screen. The
     * torture's first run found it, comparing the screen to the composition. */
    if (vibeos_input_pointer(&cx, &cy, &buttons) == 0 &&
        (took || cx != g_last_cx || cy != g_last_cy)) {
        uint32_t saved[CURSOR_H][CURSOR_W + 1u];
        uint32_t row, col;

        if (g_last_cx >= 0) {
            /* Restore what the pointer was covering, from the clean
             * composition. */
            blit_rect((uint32_t)g_last_cx, (uint32_t)g_last_cy, CURSOR_W + 1u, CURSOR_H);
        }
        /* Draw into the back buffer, blit, and put the back buffer back: the
         * cursor must not be baked into the composition, or the next frame
         * restores a smear instead of the desktop. */
        for (row = 0; row < CURSOR_H; row++) {
            for (col = 0; col <= CURSOR_W; col++) {
                uint32_t px = (uint32_t)cx + col, py = (uint32_t)cy + row;
                saved[row][col] = (px < g_w && py < g_h)
                                  ? g_back[(uint64_t)py * g_w + px] : 0u;
            }
        }
        draw_cursor(g_back, cx, cy);
        blit_rect((uint32_t)cx, (uint32_t)cy, CURSOR_W + 1u, CURSOR_H);
        for (row = 0; row < CURSOR_H; row++) {
            for (col = 0; col <= CURSOR_W; col++) {
                uint32_t px = (uint32_t)cx + col, py = (uint32_t)cy + row;
                if (px < g_w && py < g_h) {
                    g_back[(uint64_t)py * g_w + px] = saved[row][col];
                }
            }
        }
        g_last_cx = cx;
        g_last_cy = cy;
        moved = 1;
    }

    /* The canary, on every repaint rather than once at the end of the boot:
     * the repaint is what writes near it, and a detector that reports at the
     * crash says more than one that reports at shutdown. The count kept is the
     * worst seen, so a canary that was broken and is broken still is one
     * finding, not a number that grows with uptime. */
    broken = guard_count_broken();
    if (gui_lock()) {
        g_stats.guard_checks++;
        if (broken > g_stats.guard_broken) {
            g_stats.guard_broken = broken;
        }
        if (moved) {
            g_stats.frames++;
        }
        gui_unlock();
    }
    __atomic_store_n(&g_ticking, 0, __ATOMIC_RELEASE);
}

/* ---- observing ------------------------------------------------------------------ */

void vibeos_gui_stats(vibeos_gui_stats_t *out) {
    uint32_t r, c;
    uint64_t n = 0;

    if (!out) {
        return;
    }
    if (!gui_lock()) {
        /* Inside the GUI already: a copy without the lock is the best there
         * is, and a panic dump is the only caller that can get here. */
        *out = g_stats;
        out->reentered = __atomic_load_n(&g_reentered, __ATOMIC_RELAXED);
        return;
    }
    for (r = 0; r < TERM_ROWS; r++) {
        for (c = 0; c < TERM_COLS; c++) {
            if (g_term[r][c].cp) {
                n++;
            }
        }
    }
    g_stats.term_chars = n;
    *out = g_stats;
    gui_unlock();
    out->reentered = __atomic_load_n(&g_reentered, __ATOMIC_RELAXED);
}

int vibeos_gui_term_row(uint32_t row, char *out) {
    uint32_t c;
    int n = 0;

    if (row >= TERM_ROWS || !out) {
        return -1;
    }
    if (!gui_lock()) {
        return -1;
    }
    /* As text: a row up to its first empty cell, and a character outside
     * ASCII as '?' - this is for the torture's model and the host tests, which
     * compare characters, not glyphs. */
    for (c = 0; c < TERM_COLS && g_term[row][c].cp; c++) {
        uint32_t cp = g_term[row][c].cp;

        out[c] = cp < 0x80u ? (char)cp : '?';
        n++;
    }
    out[c] = 0;
    gui_unlock();
    return n;
}

int vibeos_gui_term_cell(uint32_t row, uint32_t col, uint32_t *cp, uint32_t *fg, uint32_t *bg) {
    if (row >= TERM_ROWS || col >= TERM_COLS || !cp || !fg || !bg) {
        return -1;
    }
    if (!gui_lock()) {
        return -1;
    }
    *cp = g_term[row][col].cp;
    *fg = g_term[row][col].fg;
    *bg = g_term[row][col].bg;
    gui_unlock();
    return 0;
}

void vibeos_gui_term_size(uint32_t *rows, uint32_t *cols) {
    if (rows) {
        *rows = g_rows;
    }
    if (cols) {
        *cols = g_cols;
    }
}

void vibeos_gui_term_modes(int *cursor_on, int *alt) {
    if (!gui_lock()) {
        return;
    }
    if (cursor_on) {
        *cursor_on = g_cursor_on;
    }
    if (alt) {
        *alt = g_alt;
    }
    gui_unlock();
}

void vibeos_gui_reset(void) {
    uint32_t r, c;

    for (r = 0; r < TERM_MAX_ROWS; r++) {
        for (c = 0; c < TERM_MAX_COLS; c++) {
            g_term[r][c] = (term_cell_t){ 0, COL_TEXT, COL_WINDOW };
            g_snap[r][c] = g_term[r][c];
            g_main[r][c] = g_term[r][c];
        }
    }
    g_term_col = g_term_row = 0;
    g_snap_col = g_snap_row = 0;
    g_vt = (vibeos_vt_t){ 0 };
    g_pen = PEN_DEFAULT;
    g_cols = g_rows = 2u;
    g_top = 0;
    g_bottom = 1u;
    g_saved = (term_saved_t){ 0, 0, PEN_DEFAULT };
    g_main_cursor = g_saved;
    g_cursor_on = 1;
    g_snap_cursor_on = 1;
    g_alt = 0;
    {
        uint32_t k;

        for (k = 0; k < TERM_MAX_ROWS; k++) {
            g_row_dirty[k] = 0;
            g_snap_rows[k] = 0;
        }
    }
    g_term_dirty = 0;
    g_stats = (vibeos_gui_stats_t){ 0 };
    g_reentered = 0;
    g_ticking = 0;
    g_last_cx = g_last_cy = -1;
    g_fb = 0;
    g_back = 0;
    g_guard = 0;
    g_w = g_h = 0;
    g_win_x = g_win_y = g_win_w = g_win_h = 0;
    g_active = 0;
    g_why = "not initialised";
}

/* ---- the registered device -------------------------------------------------------- */

/* The report is taken under the lock and printed outside it: the line goes to
 * the console, and the console writes into this terminal. Printing under the
 * GUI's lock would be a putc from the holder - dropped as reentered on the
 * machine, a deadlock without a `self`. */
static void gui_report(vibeos_dev_write_fn out, void *ctx) {
    vibeos_gui_stats_t s;
    vibeos_devline_t l;

    vibeos_gui_stats(&s);
    vibeos_devline_start(&l, "[GUI] GUI_STATS frames=");
    vibeos_devline_hex(&l, s.frames);
    vibeos_devline_str(&l, " termchars=");
    vibeos_devline_hex(&l, s.term_chars);
    vibeos_devline_str(&l, " chars=");
    vibeos_devline_hex(&l, s.chars);
    vibeos_devline_str(&l, " scrolls=");
    vibeos_devline_hex(&l, s.scrolls);
    vibeos_devline_str(&l, " active=");
    vibeos_devline_hex(&l, (uint64_t)vibeos_gui_active());
    vibeos_devline_str(&l, " init=");
    vibeos_devline_str(&l, vibeos_gui_why());
    vibeos_devline_end(&l, out, ctx);

    vibeos_devline_start(&l, "[GUI] MUSTBEZERO guard_broken=");
    vibeos_devline_hex(&l, s.guard_broken);
    vibeos_devline_str(&l, " MUSTBEZERO term_overrun=");
    vibeos_devline_hex(&l, s.term_overrun);
    vibeos_devline_str(&l, " guard_checks=");
    vibeos_devline_hex(&l, s.guard_checks);
    vibeos_devline_str(&l, " reentered=");
    vibeos_devline_hex(&l, s.reentered);
    vibeos_devline_str(&l, " ticks_overlapped=");
    vibeos_devline_hex(&l, s.ticks_overlapped);
    vibeos_devline_end(&l, out, ctx);
}

/* The probe is the init, on the framebuffer and the back buffer the machine
 * describes. No framebuffer is an absent display, not a failure: the reason is
 * in the report either way. */
static int gui_probe(const vibeos_dev_env_t *env) {
    return vibeos_gui_init(env->fb_base, env->fb_width, env->fb_height,
                           env->fb_back, env->fb_back_bytes);
}

/* The last column is where the grid wraps and never where it writes. */
static void gui_size(uint32_t *rows, uint32_t *cols) {
    *rows = TERM_ROWS;
    *cols = TERM_COLS - 1u;
}

static const vibeos_display_ops_t g_gui_ops = {
    .putc = vibeos_gui_putc,
    .size = gui_size,
    .tick = vibeos_gui_tick,
};

static const vibeos_device_t g_gui_device = {
    .name = "gui",
    .cls = VIBEOS_DEV_DISPLAY,
    .isa_irq = VIBEOS_DEVICE_NO_IRQ,
    .probe = gui_probe,
    .irq = 0,
    .selftest = 0,
    .report = gui_report,
    .ops = &g_gui_ops,
};
VIBEOS_DEVICE(g_gui_device);
