/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Framebuffer text console (image-only).
 *
 * Renders characters into the UEFI GOP linear framebuffer the bootloader passes
 * in boot_info, with an 8x8 bitmap font, scrolling when the screen fills. This
 * is the output half of a real console: on hardware the machine shows text on
 * the monitor instead of only on the serial line.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"
#include "vibeos/font8x8.h"
#include "vibeos/vt.h"

#define FB_FONT_W VIBEOS_FONT8X8_WIDTH
#define FB_FONT_H VIBEOS_FONT8X8_HEIGHT

static uint32_t *g_fb;
static uint32_t g_w, g_h, g_cols, g_rows, g_cx, g_cy;
static int g_ready;
static vibeos_vt_t g_vt;

#define FB_FG 0x00CFE8FFu
#define FB_BG 0x00101418u

int vibeos_x86_64_fb_init(uint64_t base, uint32_t width, uint32_t height) {
    uint32_t i, n;

    if (base == 0 || width == 0 || height == 0) {
        return -1;
    }
    g_fb = (uint32_t *)(uintptr_t)base;
    g_w = width;
    g_h = height;
    g_cols = width / FB_FONT_W;
    g_rows = height / FB_FONT_H;
    g_cx = 0;
    g_cy = 0;
    n = width * height;
    for (i = 0; i < n; i++) {
        g_fb[i] = FB_BG;
    }
    g_ready = 1;
    return 0;
}

int vibeos_x86_64_fb_ready(void) {
    return g_ready;
}

/* Half a screen at a time, in one bulk move: a line at a time moved nearly the
 * whole framebuffer for every newline once the console was full (#118). The
 * cursor lands mid-screen and the next half-screen of output costs no copy. */
static void fb_scroll(void) {
    uint32_t lines = g_rows > 1u ? g_rows / 2u : 1u;
    uint32_t shift = g_w * FB_FONT_H * lines;
    uint32_t i, last = g_w * g_h - shift;

    __builtin_memmove(g_fb, g_fb + shift, (uint64_t)last * sizeof(*g_fb));
    for (i = last; i < g_w * g_h; i++) {
        g_fb[i] = FB_BG;
    }
    g_cy = g_rows - lines;
}

static void fb_draw_glyph(char c, uint32_t cx, uint32_t cy) {
    const uint8_t *gl;
    uint32_t x, y, px, py;

    if ((unsigned char)c < 0x20u || (unsigned char)c > 0x7Eu) {
        return;
    }
    gl = vibeos_font8x8[(unsigned char)c - 0x20u];
    px = cx * FB_FONT_W;
    py = cy * FB_FONT_H;
    for (y = 0; y < FB_FONT_H; y++) {
        uint8_t bits = gl[y];
        for (x = 0; x < FB_FONT_W; x++) {
            if (px + x < g_w && py + y < g_h) {
                g_fb[(py + y) * g_w + (px + x)] = (bits & (1u << x)) ? FB_FG : FB_BG;
            }
        }
    }
}

static void fb_erase_row(uint32_t row, uint32_t from) {
    uint32_t k;

    for (k = from; k < g_cols; k++) {
        fb_draw_glyph(' ', k, row);
    }
}

/* Numbers come from whatever a program printed: compared before they are used. */
static void fb_op(vibeos_vt_op_t op, uint32_t n, uint32_t m) {
    uint32_t r;

    switch (op) {
        case VIBEOS_VT_UP: g_cy = n < g_cy ? g_cy - n : 0u; break;
        case VIBEOS_VT_DOWN: g_cy = n < g_rows - g_cy ? g_cy + n : g_rows - 1u; break;
        case VIBEOS_VT_RIGHT: g_cx = n < g_cols - g_cx ? g_cx + n : g_cols - 1u; break;
        case VIBEOS_VT_LEFT: g_cx = n < g_cx ? g_cx - n : 0u; break;
        case VIBEOS_VT_GOTO:
            g_cy = n < g_rows ? n : g_rows - 1u;
            g_cx = m < g_cols ? m : g_cols - 1u;
            break;
        case VIBEOS_VT_ERASE_LINE: fb_erase_row(g_cy, g_cx); break;
        case VIBEOS_VT_ERASE_BELOW:
            fb_erase_row(g_cy, g_cx);
            for (r = g_cy + 1u; r < g_rows; r++) {
                fb_erase_row(r, 0u);
            }
            break;
        case VIBEOS_VT_ERASE_ALL:
            for (r = 0; r < g_rows; r++) {
                fb_erase_row(r, 0u);
            }
            break;
        default: break;
    }
}

/* This console wraps on writing the last column, with no held cursor: a width
 * one less keeps a line editor's arithmetic true. */
void vibeos_x86_64_fb_size(uint32_t *rows, uint32_t *cols) {
    *rows = g_rows;
    *cols = g_cols > 1u ? g_cols - 1u : g_cols;
}

void vibeos_x86_64_fb_putc(char c) {
    vibeos_vt_op_t op;
    uint32_t vn, vm;

    if (!g_ready) {
        return;
    }
    op = vibeos_vt_feed(&g_vt, c, &vn, &vm);
    if (op != VIBEOS_VT_PRINT) {
        fb_op(op, vn, vm);
        return;
    }
    /* The character is the parser's: a UTF-8 one arrives with its last byte,
     * which is not the character. This console has only the ASCII font. */
    c = vn < 0x80u ? (char)vn : '?';
    if (c == '\r') {
        g_cx = 0;
        return;
    }
    if (c == '\n') {
        g_cx = 0;
        if (++g_cy >= g_rows) {
            fb_scroll();
        }
        return;
    }
    if (c == '\b') {
        if (g_cx > 0) {
            g_cx--;
            fb_draw_glyph(' ', g_cx, g_cy);
        }
        return;
    }
    fb_draw_glyph(c, g_cx, g_cy);
    if (++g_cx >= g_cols) {
        g_cx = 0;
        if (++g_cy >= g_rows) {
            fb_scroll();
        }
    }
}

void vibeos_x86_64_fb_puts(const char *s) {
    if (!s) {
        return;
    }
    while (*s) {
        vibeos_x86_64_fb_putc(*s++);
    }
}
