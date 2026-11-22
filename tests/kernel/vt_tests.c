/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The console's two portable halves (vibeos/vt.h), and the GUI's terminal doing
 * what they ask. The expected sequences are xterm's, which is what a line editor
 * anywhere is written against. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vibeos/gui.h"
#include "vibeos/vt.h"

int test_vt(void);

#define NEED(cond, what)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL:vt %s\n", what); \
            return 1;                    \
        }                                \
    } while (0)

static int seq_is(const char *s, vibeos_vt_op_t op, uint32_t a, uint32_t b) {
    vibeos_vt_t vt;
    vibeos_vt_op_t last = VIBEOS_VT_NONE;
    uint32_t n = 0, m = 0;

    memset(&vt, 0, sizeof(vt));
    for (; *s; s++) {
        last = vibeos_vt_feed(&vt, *s, &n, &m);
        if (s[1] && last != VIBEOS_VT_NONE) {
            return 0;       /* acted before the sequence ended */
        }
    }
    return last == op && n == a && m == b && vt.state == 0u;
}

/* What a whole key produces, as a string. */
static int key_is(vibeos_kbd_t *k, const uint8_t *codes, size_t count, const char *want) {
    char got[64], one[VIBEOS_KBD_MAX];
    size_t i, at = 0;

    for (i = 0; i < count; i++) {
        uint32_t n = vibeos_kbd_key(k, codes[i], one);

        memcpy(got + at, one, n);
        at += n;
    }
    got[at] = 0;
    return strcmp(got, want) == 0;
}

#define KEY(want, ...)                                                        \
    do {                                                                      \
        static const uint8_t codes_[] = {__VA_ARGS__};                        \
        NEED(key_is(&k, codes_, sizeof(codes_), want), "key gave: " want);    \
    } while (0)

static int test_output(void) {
    vibeos_vt_t vt;
    uint32_t n, m;

    NEED(seq_is("\x1b[A", VIBEOS_VT_UP, 1, 0), "up");
    NEED(seq_is("\x1b[12B", VIBEOS_VT_DOWN, 12, 0), "down 12");
    NEED(seq_is("\x1b[3C", VIBEOS_VT_RIGHT, 3, 0), "right 3");
    NEED(seq_is("\x1b[0D", VIBEOS_VT_LEFT, 1, 0), "zero counts as one");
    NEED(seq_is("\x1b[H", VIBEOS_VT_GOTO, 0, 0), "home");
    NEED(seq_is("\x1b[5;9H", VIBEOS_VT_GOTO, 4, 8), "goto is one-based");
    NEED(seq_is("\x1b[J", VIBEOS_VT_ERASE_BELOW, 0, 0), "erase below");
    NEED(seq_is("\x1b[2J", VIBEOS_VT_ERASE_ALL, 0, 0), "erase all");
    NEED(seq_is("\x1b[K", VIBEOS_VT_ERASE_LINE, 0, 0), "erase line");
    NEED(seq_is("\x1b[1;31m", VIBEOS_VT_NONE, 0, 0), "a colour prints nothing");
    NEED(seq_is("\x1b[?25l", VIBEOS_VT_NONE, 0, 0), "a private mode prints nothing");
    NEED(seq_is("\x1b[99999999A", VIBEOS_VT_UP, 0xFFFF, 0), "a huge number saturates");

    memset(&vt, 0, sizeof(vt));
    NEED(vibeos_vt_feed(&vt, 'x', &n, &m) == VIBEOS_VT_PRINT, "text is text");
    (void)vibeos_vt_feed(&vt, 0x1b, &n, &m);
    (void)vibeos_vt_feed(&vt, 'c', &n, &m);
    NEED(vibeos_vt_feed(&vt, 'y', &n, &m) == VIBEOS_VT_PRINT, "text after an unknown escape");
    return 0;
}

static int test_input(void) {
    vibeos_kbd_t k;

    memset(&k, 0, sizeof(k));
    KEY("a", 0x1E, 0x9E);
    KEY("A", 0x2A, 0x1E, 0x9E, 0xAA);
    KEY("a", 0x1E, 0x9E);                         /* Shift came back up */
    KEY("$", 0x36, 0x05, 0x85, 0xB6);
    KEY("|", 0x2A, 0x2B, 0xAB, 0xAA);
    KEY("A1", 0x3A, 0xBA, 0x1E, 0x9E, 0x02, 0x82);   /* Caps: letters only */
    KEY("a", 0x2A, 0x1E, 0x9E, 0xAA);             /* Caps and Shift cancel */
    KEY("", 0x3A, 0xBA);

    KEY("\x1b[A", 0xE0, 0x48, 0xE0, 0xC8);
    KEY("\x1b[B", 0xE0, 0x50, 0xE0, 0xD0);
    KEY("\x1b[C", 0xE0, 0x4D, 0xE0, 0xCD);
    KEY("\x1b[D", 0xE0, 0x4B, 0xE0, 0xCB);
    KEY("\x1b[H", 0xE0, 0x47, 0xE0, 0xC7);
    KEY("\x1b[F", 0xE0, 0x4F, 0xE0, 0xCF);
    KEY("\x1b[3~", 0xE0, 0x53, 0xE0, 0xD3);

    /* The left Control, then the right one, which is the same code behind E0. */
    KEY("\x1b[1;5D", 0x1D, 0xE0, 0x4B, 0xE0, 0xCB, 0x9D);
    KEY("\x1b[1;5C", 0xE0, 0x1D, 0xE0, 0x4D, 0xE0, 0xCD, 0xE0, 0x9D);
    KEY("\x1b[C", 0xE0, 0x4D, 0xE0, 0xCD);        /* Control came back up */
    KEY("\x1b[3;5~", 0x1D, 0xE0, 0x53, 0xE0, 0xD3, 0x9D);
    KEY("\x1b[1;2A", 0x2A, 0xE0, 0x48, 0xE0, 0xC8, 0xAA);
    KEY("\x1b[1;3D", 0x38, 0xE0, 0x4B, 0xE0, 0xCB, 0xB8);

    KEY("\x03", 0x1D, 0x2E, 0xAE, 0x9D);
    KEY("\x03", 0x1D, 0x2A, 0x2E, 0xAE, 0xAA, 0x9D);  /* with Shift too */
    KEY("\x1c", 0x1D, 0x2B, 0xAB, 0x9D);
    KEY("\x1b" "b", 0x38, 0x30, 0xB0, 0xB8);

    /* The false Shift of Print Screen does not stick. */
    KEY("", 0xE0, 0x2A, 0xE0, 0x37, 0xE0, 0xB7, 0xE0, 0xAA);
    KEY("a", 0x1E, 0x9E);
    /* Pause says Control and NumLock in passing, and never releases them. */
    KEY("", 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5);
    NEED(!vibeos_kbd_ctrl(&k), "Pause left Control held");
    KEY("a", 0x1E, 0x9E);
    /* Each of those matters to a modifier really held: the false Shift's
     * release is not the real Shift's, Pause's is not Control's, and letting go
     * of one Control leaves the other down. */
    KEY("A", 0x2A, 0xE0, 0x2A, 0xE0, 0x37, 0xE0, 0xB7, 0xE0, 0xAA, 0x1E, 0x9E, 0xAA);
    KEY("\x03", 0x1D, 0xE1, 0x1D, 0x45, 0xE1, 0x9D, 0xC5, 0x2E, 0xAE, 0x9D);
    KEY("\x03", 0x1D, 0xE0, 0x1D, 0xE0, 0x9D, 0x2E, 0xAE, 0x9D);
    KEY("\x03", 0xE0, 0x1D, 0x1D, 0x9D, 0x2E, 0xAE, 0xE0, 0x9D);
    NEED(!vibeos_kbd_ctrl(&k), "Control is still held");
    KEY("\n", 0xE0, 0x1C, 0xE0, 0x9C);
    KEY("/", 0xE0, 0x35, 0xE0, 0xB5);
    return 0;
}

/* ---- the GUI's terminal --------------------------------------------------------- */

static void put(const char *s) {
    for (; *s; s++) {
        vibeos_gui_putc(*s);
    }
}

static int row_is(uint32_t r, const char *want) {
    char b[VIBEOS_GUI_TERM_COLS + 1u];

    return vibeos_gui_term_row(r, b) >= 0 && strcmp(b, want) == 0;
}

static int test_screen(void) {
    static uint32_t fb[320 * 200];
    static uint64_t back[(320u * 200u * 4u + VIBEOS_GUI_GUARD_BYTES + 16u) / 8u + 1u];
    char b[VIBEOS_GUI_TERM_COLS + 1u];
    uint32_t i;

    vibeos_gui_reset();
    NEED(vibeos_gui_init((uint64_t)(uintptr_t)fb, 320, 200, back, VIBEOS_GUI_BACK_BYTES(320, 200)) == 0,
         "no display");

    put("abcdef\r");
    NEED(row_is(0, "abcdef"), "carriage return erased");
    put("XY");
    NEED(row_is(0, "XYcdef"), "carriage return did not return");
    put("\x1b[K");
    NEED(row_is(0, "XY"), "erase to the end of the row");
    put("\x1b[3CZ");
    NEED(row_is(0, "XY   Z"), "moving right leaves spaces, not an end of row");
    put("\r\n\r\nriga3\x1b[2A\rQ");
    NEED(row_is(0, "QY   Z"), "up");
    put("\x1b[2B\rW");
    NEED(row_is(2, "Wiga3"), "down");
    put("\x1b[1;2H\x1b[J");
    NEED(row_is(0, "Q") && row_is(1, "") && row_is(2, ""), "erase below");
    put("\x1b[2;4Hm");
    NEED(row_is(1, "   m"), "goto");
    put("\x1b[2J\x1b[H");
    NEED(row_is(0, "") && row_is(1, ""), "erase all");

    /* Past every edge: refused by clamping, never written outside the grid. */
    put("\x1b[999A\x1b[999D@\x1b[999B\x1b[999C#\x1b[999;999H$\x1b[65535C%");
    for (i = 0; i < VIBEOS_GUI_TERM_ROWS; i++) {
        NEED(vibeos_gui_term_row(i, b) >= 0, "a row");
    }
    {
        vibeos_gui_stats_t st;

        vibeos_gui_stats(&st);
        NEED(st.term_overrun == 0u, "a sequence wrote outside the grid");
    }
    vibeos_gui_reset();
    return 0;
}

int test_vt(void) {
    return test_output() || test_input() || test_screen();
}
