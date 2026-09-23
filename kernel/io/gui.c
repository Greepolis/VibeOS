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

#define TERM_COLS VIBEOS_GUI_TERM_COLS
#define TERM_ROWS VIBEOS_GUI_TERM_ROWS
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

static char g_term[TERM_ROWS][TERM_COLS];
static uint32_t g_term_col, g_term_row;
static int g_term_dirty;
static vibeos_gui_stats_t g_stats;
/* Outside the lock by definition - it counts the writes that could not take it -
 * so it is its own atomic rather than a field the struct copy reads plainly. */
static uint64_t g_reentered;

/* ---- state owned by the repaint (g_ticking) ------------------------------------ */

static volatile int g_ticking;
static char g_snap[TERM_ROWS][TERM_COLS];
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

/* Repaint the window's text area from a copy of the grid. The window is at
 * least the minimum screen's worth of pixels (vibeos_gui_init refuses smaller),
 * so none of these subtractions can go below zero. */
static void compose_terminal(char grid[TERM_ROWS][TERM_COLS]) {
    uint32_t row;

    fill_rect(g_back, g_win_x + 4u, g_win_y + 22u,
              g_win_w - 8u, g_win_h - 26u, COL_WINDOW);
    for (row = 0; row < TERM_ROWS; row++) {
        uint32_t col;
        uint32_t y = g_win_y + 24u + row * 9u;
        if (y + 8u > g_win_y + g_win_h - 2u) {
            break;
        }
        for (col = 0; col < TERM_COLS; col++) {
            char c = grid[row][col];
            if (c == 0) {
                break;
            }
            draw_char(g_back, g_win_x + 8u + col * 8u, y, c, COL_TEXT);
        }
    }
}

static void compose_desktop(void) {
    g_win_x = g_w / 8u;
    g_win_y = g_h / 6u;
    g_win_w = TERM_COLS * 8u + 16u;
    g_win_h = TERM_ROWS * 9u + 32u;
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
    compose_terminal(g_snap);
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

static void term_scroll(void) {
    uint32_t r, cc;

    for (r = 1; r < TERM_ROWS; r++) {
        for (cc = 0; cc < TERM_COLS; cc++) {
            g_term[r - 1u][cc] = g_term[r][cc];
        }
    }
    for (cc = 0; cc < TERM_COLS; cc++) {
        g_term[TERM_ROWS - 1u][cc] = 0;
    }
    g_term_row = TERM_ROWS - 1u;
    g_term_col = 0;
    g_stats.scrolls++;
}

/* Write one cell, refusing one outside the grid. Under the lock this cannot
 * happen, and the count is how that is known rather than assumed: before C7 the
 * same write, from two cores at once, landed past the end of the array. */
static void term_set(uint32_t row, uint32_t col, char c) {
    if (row >= TERM_ROWS || col >= TERM_COLS) {
        g_stats.term_overrun++;
        return;
    }
    g_term[row][col] = c;
}

/* One character into the terminal grid. Called from the console write path, so
 * it does the least possible work: it only marks the window dirty, and the
 * repaint happens on the timer where a full recompose is affordable. */
void vibeos_gui_putc(char c) {
    if (!vibeos_gui_active() || c == '\r') {
        return;
    }
    if (!gui_lock()) {
        __atomic_fetch_add(&g_reentered, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (c == '\n') {
        g_term_col = 0;
        g_term_row++;
    } else if (c == '\b') {
        if (g_term_col > 0u) {
            g_term_col--;
            term_set(g_term_row, g_term_col, 0);
        }
    } else {
        if (g_term_col >= TERM_COLS - 1u) {
            g_term_col = 0;
            g_term_row++;
            if (g_term_row >= TERM_ROWS) {
                term_scroll();
            }
        }
        term_set(g_term_row, g_term_col, c);
        g_term_col++;
        g_stats.chars++;
    }
    if (g_term_row >= TERM_ROWS) {
        /* Scroll by moving the grid, not the pixels. */
        term_scroll();
    }
    g_term_dirty = 1;
    gui_unlock();
}

/* ---- init ---------------------------------------------------------------------- */

int vibeos_gui_init(uint64_t fb_base, uint32_t width, uint32_t height,
                    void *back, uint64_t back_bytes) {
    uint64_t pixels;
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
    pixels = (uint64_t)width * height;
    if (!back || back_bytes < pixels * 4u + VIBEOS_GUI_GUARD_BYTES) {
        g_why = "back buffer missing or too small for the screen and its canary";
        return -1;
    }
    g_fb = (uint32_t *)(uintptr_t)fb_base;
    g_back = (uint32_t *)back;
    g_guard = (uint64_t *)(void *)(g_back + pixels);
    for (i = 0; i < GUARD_WORDS; i++) {
        g_guard[i] = GUI_GUARD;
    }
    g_w = width;
    g_h = height;

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
            for (r = 0; r < TERM_ROWS; r++) {
                for (c = 0; c < TERM_COLS; c++) {
                    g_snap[r][c] = g_term[r][c];
                }
            }
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
        compose_terminal(g_snap);
        blit_rect(g_win_x, g_win_y, g_win_w, g_win_h);
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
            if (g_term[r][c]) {
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
    for (c = 0; c < TERM_COLS && g_term[row][c]; c++) {
        out[c] = g_term[row][c];
        n++;
    }
    out[c] = 0;
    gui_unlock();
    return n;
}

void vibeos_gui_reset(void) {
    uint32_t r, c;

    for (r = 0; r < TERM_ROWS; r++) {
        for (c = 0; c < TERM_COLS; c++) {
            g_term[r][c] = 0;
            g_snap[r][c] = 0;
        }
    }
    g_term_col = g_term_row = 0;
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

static const vibeos_display_ops_t g_gui_ops = {
    .putc = vibeos_gui_putc,
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
