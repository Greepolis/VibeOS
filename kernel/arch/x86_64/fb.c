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

#define FB_FONT_W VIBEOS_FONT8X8_WIDTH
#define FB_FONT_H VIBEOS_FONT8X8_HEIGHT

static uint32_t *g_fb;
static uint32_t g_w, g_h, g_cols, g_rows, g_cx, g_cy;
static int g_ready;

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

static void fb_scroll(void) {
    uint32_t row_px = g_w * FB_FONT_H;
    uint32_t i, last = g_w * g_h - row_px;

    for (i = 0; i < last; i++) {
        g_fb[i] = g_fb[i + row_px];
    }
    for (i = last; i < g_w * g_h; i++) {
        g_fb[i] = FB_BG;
    }
    g_cy = g_rows - 1u;
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

void vibeos_x86_64_fb_putc(char c) {
    if (!g_ready) {
        return;
    }
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
