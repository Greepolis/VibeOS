/* A basic graphical shell for the UEFI framebuffer.
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
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

#define GUI_MAX_W 1920u
#define GUI_MAX_H 1200u

extern void vibeos_x86_64_mouse_state(int32_t *x, int32_t *y, uint32_t *buttons);
extern int vibeos_x86_64_mouse_ready(void);

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

static uint32_t *g_fb;
static uint32_t g_w, g_h;
static int g_active;

/* The back buffer is supplied by the caller: this file has no allocator, and
 * a screen-sized static array would be several megabytes of .bss in every
 * kernel image whether or not a framebuffer exists. */
static uint32_t *g_back;

static int32_t g_last_cx = -1, g_last_cy = -1;
static uint32_t g_frames;

/* The window is a terminal: whatever the system writes to the console is also
 * written here, so the machine shows on screen what it has been saying on the
 * serial line. Text is kept in a small grid and redrawn on change rather than
 * scrolled pixel by pixel, because scrolling a framebuffer means reading it
 * back, and reading write-combining video memory is far slower than
 * recomposing from a buffer that lives in ordinary RAM. */
#define TERM_COLS 72u
#define TERM_ROWS 24u
static char g_term[TERM_ROWS][TERM_COLS];
static uint32_t g_term_col, g_term_row;
static uint32_t g_win_x, g_win_y, g_win_w, g_win_h;
static int g_term_dirty;

extern const uint8_t *vibeos_x86_64_fb_font_row(char c, uint32_t row);

static void fill_rect(uint32_t *dst, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                      uint32_t colour) {
    uint32_t yy, xx;

    if (x >= g_w || y >= g_h) {
        return;
    }
    if (x + w > g_w) {
        w = g_w - x;
    }
    if (y + h > g_h) {
        h = g_h - y;
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

    for (row = 0; row < 8u; row++) {
        const uint8_t *bits = vibeos_x86_64_fb_font_row(c, row);
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

    if (x >= g_w || y >= g_h) {
        return;
    }
    if (x + w > g_w) {
        w = g_w - x;
    }
    if (y + h > g_h) {
        h = g_h - y;
    }
    for (yy = 0; yy < h; yy++) {
        uint64_t off = (uint64_t)(y + yy) * g_w + x;
        uint32_t xx;
        for (xx = 0; xx < w; xx++) {
            g_fb[off + xx] = g_back[off + xx];
        }
    }
}

/* Draw everything that does not move. */
/* Repaint the window's text area from the character grid. */
static void compose_terminal(void) {
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
            char c = g_term[row][col];
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
    if (g_win_x + g_win_w > g_w) {
        g_win_w = g_w - g_win_x;
    }
    if (g_win_y + g_win_h > g_h) {
        g_win_h = g_h - g_win_y;
    }

    fill_rect(g_back, 0, 0, g_w, g_h, COL_DESKTOP);
    fill_rect(g_back, 0, 0, g_w, 24u, COL_PANEL);
    draw_text(g_back, 8u, 8u, "VibeOS", COL_TITLETXT);

    fill_rect(g_back, g_win_x, g_win_y, g_win_w, g_win_h, COL_WINDOW);
    fill_rect(g_back, g_win_x, g_win_y, g_win_w, 20u, COL_TITLE);
    draw_text(g_back, g_win_x + 6u, g_win_y + 6u, "console", COL_TITLETXT);
    compose_terminal();
}

/* One character into the terminal grid. Called from the console write path, so
 * it does the least possible work: it only marks the window dirty, and the
 * repaint happens on the timer where a full recompose is affordable. */
void vibeos_x86_64_gui_putc(char c) {
    if (!g_active) {
        return;
    }
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        g_term_col = 0;
        g_term_row++;
    } else if (c == '\b') {
        if (g_term_col > 0u) {
            g_term_col--;
            g_term[g_term_row][g_term_col] = 0;
        }
    } else {
        if (g_term_col >= TERM_COLS - 1u) {
            g_term_col = 0;
            g_term_row++;
        }
        if (g_term_row < TERM_ROWS) {
            g_term[g_term_row][g_term_col++] = c;
        }
    }
    if (g_term_row >= TERM_ROWS) {
        /* Scroll by moving the grid, not the pixels. */
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
    }
    g_term_dirty = 1;
}

int vibeos_x86_64_gui_init(uint64_t fb_base, uint32_t width, uint32_t height,
                           void *back_buffer) {
    if (fb_base == 0u || width == 0u || height == 0u || !back_buffer) {
        return -1;
    }
    if (width > GUI_MAX_W || height > GUI_MAX_H) {
        return -1;   /* the caller's buffer was sized for at most this */
    }
    g_fb = (uint32_t *)(uintptr_t)fb_base;
    g_back = (uint32_t *)back_buffer;
    g_w = width;
    g_h = height;

    compose_desktop();
    blit_rect(0, 0, g_w, g_h);
    g_active = 1;
    g_last_cx = -1;
    g_last_cy = -1;
    return 0;
}

uint32_t vibeos_x86_64_gui_term_chars(void) {
    uint32_t row, col, n = 0;
    for (row = 0; row < TERM_ROWS; row++) {
        for (col = 0; col < TERM_COLS; col++) {
            if (g_term[row][col]) {
                n++;
            }
        }
    }
    return n;
}

int vibeos_x86_64_gui_active(void) {
    return g_active;
}

uint32_t vibeos_x86_64_gui_frames(void) {
    return g_frames;
}

/* Called from the timer. Repaints only the two small areas that can have
 * changed: where the pointer was, and where it is now. */
void vibeos_x86_64_gui_tick(void) {
    int32_t cx = 0, cy = 0;
    uint32_t buttons = 0;

    if (!g_active || !vibeos_x86_64_mouse_ready()) {
        return;
    }
    /* Repaint the text before the pointer, so the pointer is drawn on top of
     * current content rather than being erased by it. */
    if (g_term_dirty) {
        g_term_dirty = 0;
        compose_terminal();
        blit_rect(g_win_x, g_win_y, g_win_w, g_win_h);
        g_last_cx = -1;   /* whatever was under the pointer is gone */
    }

    vibeos_x86_64_mouse_state(&cx, &cy, &buttons);
    if (cx == g_last_cx && cy == g_last_cy) {
        return;
    }

    if (g_last_cx >= 0) {
        /* Restore what the pointer was covering, from the clean composition. */
        blit_rect((uint32_t)g_last_cx, (uint32_t)g_last_cy, CURSOR_W + 1u, CURSOR_H);
    }
    {
        /* Draw into a scratch copy of the back buffer region, then blit: the
         * cursor must not be baked into the composition, or the next frame
         * restores a smear instead of the desktop. */
        uint32_t saved[CURSOR_H][CURSOR_W + 1u];
        uint32_t row, col;
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
    }
    g_last_cx = cx;
    g_last_cy = cy;
    g_frames++;
}
