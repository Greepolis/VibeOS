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
    NEED(seq_is("\x1b[1;31m", VIBEOS_VT_SGR, 0, 0), "a colour is SGR, and prints nothing");
    NEED(seq_is("\x1b[?25l", VIBEOS_VT_CURSOR_HIDE, 0, 0), "the cursor hidden");
    NEED(seq_is("\x1b[?25h", VIBEOS_VT_CURSOR_SHOW, 0, 0), "the cursor shown");
    NEED(seq_is("\x1b[?1049;1006h", VIBEOS_VT_ALT_ON, 0, 0), "the alternate screen, among other modes");
    NEED(seq_is("\x1b[?1049l", VIBEOS_VT_ALT_OFF, 0, 0), "and back");
    NEED(seq_is("\x1b[?2004h", VIBEOS_VT_NONE, 0, 0), "a private mode nothing acts on prints nothing");
    NEED(seq_is("\x1b[?u", VIBEOS_VT_NONE, 0, 0), "a keyboard query is not a restore");
    NEED(seq_is("\x1b[>4;1m", VIBEOS_VT_NONE, 0, 0), "a `>` SGR is a keyboard mode, not a colour");
    NEED(seq_is("\x1b[2 q", VIBEOS_VT_NONE, 0, 0), "an intermediate byte: the cursor's shape");
    NEED(seq_is("\x1b[1J", VIBEOS_VT_ERASE_ABOVE, 0, 0), "erase above");
    NEED(seq_is("\x1b[3J", VIBEOS_VT_ERASE_ALL, 0, 0), "erase the scrollback is erase all here");
    NEED(seq_is("\x1b[1K", VIBEOS_VT_ERASE_LINE_LEFT, 0, 0), "erase to the left");
    NEED(seq_is("\x1b[2K", VIBEOS_VT_ERASE_LINE_ALL, 0, 0), "erase the row");
    NEED(seq_is("\x1b[7G", VIBEOS_VT_COLUMN, 6, 0), "column, one-based");
    NEED(seq_is("\x1b[4d", VIBEOS_VT_ROW, 3, 0), "row, one-based");
    NEED(seq_is("\x1b[5X", VIBEOS_VT_ERASE_CHARS, 5, 0), "erase characters");
    NEED(seq_is("\x1b[@", VIBEOS_VT_INSERT_CHARS, 1, 0), "insert one character");
    NEED(seq_is("\x1b[2P", VIBEOS_VT_DELETE_CHARS, 2, 0), "delete characters");
    NEED(seq_is("\x1b[3L", VIBEOS_VT_INSERT_LINES, 3, 0), "insert lines");
    NEED(seq_is("\x1b[M", VIBEOS_VT_DELETE_LINES, 1, 0), "delete a line");
    NEED(seq_is("\x1b[2S", VIBEOS_VT_SCROLL_UP, 2, 0), "scroll up");
    NEED(seq_is("\x1b[T", VIBEOS_VT_SCROLL_DOWN, 1, 0), "scroll down");
    NEED(seq_is("\x1b[2;10r", VIBEOS_VT_REGION, 2, 10), "a scroll region, as written");
    NEED(seq_is("\x1b[r", VIBEOS_VT_REGION, 0, 0), "and the default one");
    NEED(seq_is("\x1b[s", VIBEOS_VT_SAVE, 0, 0) && seq_is("\x1b[u", VIBEOS_VT_RESTORE, 0, 0),
         "save and restore, CSI");
    NEED(seq_is("\x1b" "7", VIBEOS_VT_SAVE, 0, 0) && seq_is("\x1b" "8", VIBEOS_VT_RESTORE, 0, 0),
         "save and restore, ESC");
    NEED(seq_is("\x1bM", VIBEOS_VT_REVERSE_INDEX, 0, 0) && seq_is("\x1b" "D", VIBEOS_VT_INDEX, 0, 0),
         "index and reverse index");
    NEED(seq_is("\x1b" "c", VIBEOS_VT_RESET, 0, 0), "a full reset");
    NEED(seq_is("\x1b]2;a title\x07", VIBEOS_VT_NONE, 0, 0), "a title, ended by BEL, prints nothing");
    NEED(seq_is("\x1b]0;a title\x1b\\", VIBEOS_VT_NONE, 0, 0), "a title, ended by ESC \\");
    NEED(seq_is("\x1bP+q544e\x1b\\", VIBEOS_VT_NONE, 0, 0), "a DCS prints nothing");
    NEED(seq_is("\x1b(B", VIBEOS_VT_NONE, 0, 0), "a character set designation prints nothing");
    NEED(seq_is("\x1b[99999999A", VIBEOS_VT_UP, 0xFFFF, 0), "a huge number saturates");

    memset(&vt, 0, sizeof(vt));
    {
        /* The SGR's numbers, all of them: a 24-bit foreground and a 256-colour
         * background are eight. */
        const char *sgr = "\x1b[38;2;1;2;3;48;5;196m";
        vibeos_vt_op_t op = VIBEOS_VT_NONE;

        for (; *sgr; sgr++) {
            op = vibeos_vt_feed(&vt, *sgr, &n, &m);
        }
        NEED(op == VIBEOS_VT_SGR && vt.count == 7u && vt.param[0] == 38u && vt.param[4] == 3u &&
                 vt.param[7] == 196u,
             "an SGR keeps every number");
    }
    {
        /* More parameters than are kept: the extra are dropped, the sequence
         * still ends where it ends. */
        const char *many = "\x1b[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20m";
        vibeos_vt_op_t op = VIBEOS_VT_NONE;

        for (; *many; many++) {
            op = vibeos_vt_feed(&vt, *many, &n, &m);
        }
        NEED(op == VIBEOS_VT_SGR && vt.count == VIBEOS_VT_PARAMS - 1u && vt.param[15] == 16u && vt.state == 0u,
             "twenty parameters keep sixteen and end in step");
    }
    /* UTF-8: a character arrives with its last byte, as its code point. */
    NEED(vibeos_vt_feed(&vt, (char)0xE2, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0x95, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0x94, &n, &m) == VIBEOS_VT_PRINT && n == 0x2554u,
         "a three-byte character is one code point");
    NEED(vibeos_vt_feed(&vt, (char)0xC3, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0xA9, &n, &m) == VIBEOS_VT_PRINT && n == 0xE9u,
         "a two-byte character");
    NEED(vibeos_vt_feed(&vt, (char)0xF0, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0x9F, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0x98, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0x80, &n, &m) == VIBEOS_VT_PRINT && n == 0x1F600u,
         "a four-byte character");
    NEED(vibeos_vt_feed(&vt, (char)0x80, &n, &m) == VIBEOS_VT_PRINT && n == VIBEOS_VT_REPLACEMENT,
         "a stray continuation is a replacement");
    NEED(vibeos_vt_feed(&vt, (char)0xE0, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0x80, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0x80, &n, &m) == VIBEOS_VT_PRINT && n == VIBEOS_VT_REPLACEMENT,
         "an overlong form is a replacement");
    NEED(vibeos_vt_feed(&vt, (char)0xED, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0xA0, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, (char)0x80, &n, &m) == VIBEOS_VT_PRINT && n == VIBEOS_VT_REPLACEMENT,
         "a surrogate is a replacement");
    /* Cut short by an escape: the replacement, and the escape is not lost -
     * the sequence after it must not print. */
    NEED(vibeos_vt_feed(&vt, (char)0xE2, &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, 0x1b, &n, &m) == VIBEOS_VT_PRINT && n == VIBEOS_VT_REPLACEMENT &&
             vibeos_vt_feed(&vt, '[', &n, &m) == VIBEOS_VT_NONE &&
             vibeos_vt_feed(&vt, 'K', &n, &m) == VIBEOS_VT_ERASE_LINE,
         "a character cut short by an escape keeps the escape");
    NEED(vibeos_vt_feed(&vt, 'x', &n, &m) == VIBEOS_VT_PRINT && n == 'x', "text is text");
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

/* One cell's code point and colours. */
static int cell_is(uint32_t r, uint32_t c, uint32_t cp, uint32_t fg, uint32_t bg) {
    uint32_t gcp, gfg, gbg;

    return vibeos_gui_term_cell(r, c, &gcp, &gfg, &gbg) == 0 && gcp == cp && gfg == fg && gbg == bg;
}

static uint32_t cell_cp(uint32_t r, uint32_t c) {
    uint32_t cp = 0xFFFFFFFFu, fg, bg;

    (void)vibeos_gui_term_cell(r, c, &cp, &fg, &bg);
    return cp;
}

static uint32_t cell_fg(uint32_t r, uint32_t c) {
    uint32_t cp, fg = 0xFFFFFFFFu, bg;

    (void)vibeos_gui_term_cell(r, c, &cp, &fg, &bg);
    return fg;
}

static uint32_t cell_bg(uint32_t r, uint32_t c) {
    uint32_t cp, fg, bg = 0xFFFFFFFFu;

    (void)vibeos_gui_term_cell(r, c, &cp, &fg, &bg);
    return bg;
}

/* What a full-screen program asks of the grid (S7): colours, the alternate
 * screen, a scroll region, inserting and deleting, UTF-8. */
static int test_screen_full(void) {
    static uint32_t fb[640 * 480];
    static uint64_t back[(640u * 480u * 4u + VIBEOS_GUI_GUARD_BYTES + 16u) / 8u + 1u];
    uint32_t dfg, dbg, cp;
    int cursor_on = -1, alt = -1;

    vibeos_gui_reset();
    NEED(vibeos_gui_init((uint64_t)(uintptr_t)fb, 640, 480, back, VIBEOS_GUI_BACK_BYTES(640, 480)) == 0,
         "no display");
    put("d");
    NEED(vibeos_gui_term_cell(0, 0, &cp, &dfg, &dbg) == 0 && cp == 'd', "a cell");

    /* Colours. */
    put("\r\x1b[38;2;1;2;3;48;5;196mA\x1b[0mB\x1b[1;31mC\x1b[22;7mD\x1b[0;4mE\x1b[m");
    NEED(cell_is(0, 0, 'A', 0x010203u, 0xFF0000u), "a 24-bit foreground on a 256-colour background");
    NEED(cell_is(0, 1, 'B', dfg, dbg), "a reset is the default pen");
    NEED(cell_is(0, 2, 'C', 0xFF0000u, dbg), "bold brightens one of the first eight");
    NEED(cell_is(0, 3, 'D', dbg, 0xCD0000u), "reverse swaps them");
    NEED(cell_is(0, 4, 'E', dfg | 0x01000000u, dbg), "underline");
    put("\x1b[44m\x1b[2K");
    NEED(cell_is(0, 0, 0u, dfg, 0x0000EEu) && cell_bg(0, VIBEOS_GUI_TERM_COLS - 1u) == 0x0000EEu,
         "erasing leaves the pen's background");
    put("\x1b[0m");

    /* An erased cell is not the end of a row: what is written after it stays,
     * and is drawn (compose draws every cell). */
    put("\x1b[2J\x1b[HABCDEF\x1b[1;3H\x1b[2X");
    NEED(cell_cp(0, 2) == 0u && cell_cp(0, 3) == 0u && cell_cp(0, 4) == 'E', "erase characters leaves a gap");
    {
        /* And on the screen: the 'E' beyond the gap is drawn. Until S7 a row
         * was drawn up to its first empty cell, and FinalCommander's panels
         * lost everything a program wrote past an erase. The window's place is
         * compose_desktop's for a 640 by 480 screen: wider than the screen, so
         * at an eighth across; centred below the panel down. Text starts 8 in
         * and 24 down; a cell is 8 by 9. */
        uint32_t win_h = VIBEOS_GUI_TERM_ROWS * 9u + 32u;
        uint32_t x0 = 640u / 8u + 8u, y0 = 24u + (480u - 24u - win_h) / 3u + 24u;
        uint32_t xx, yy, lit_e = 0, lit_gap = 0, lit_box = 0;

        vibeos_gui_tick();
        for (yy = 0; yy < 8u; yy++) {
            for (xx = 0; xx < 8u; xx++) {
                lit_e += fb[(y0 + yy) * 640u + x0 + 4u * 8u + xx] == (dfg & 0xFFFFFFu);
                lit_gap += fb[(y0 + yy) * 640u + x0 + 2u * 8u + xx] == (dfg & 0xFFFFFFu);
            }
        }
        NEED(lit_e > 4u && lit_gap == 0u, "a character past an erased cell is drawn, the erased cell is not");

        /* A double horizontal line joins its neighbours: both of its lines run
         * to the cell's edges. */
        put("\x1b[2J\x1b[H\xe2\x95\x90");
        vibeos_gui_tick();
        for (xx = 0; xx < 8u; xx++) {
            lit_box += fb[(y0 + 3u) * 640u + x0 + xx] == (dfg & 0xFFFFFFu);
            lit_box += fb[(y0 + 6u) * 640u + x0 + xx] == (dfg & 0xFFFFFFu);
        }
        NEED(lit_box == 16u, "a double line is two lines across the whole cell");
        put("\x1b[2J\x1b[H");
    }

    /* Insert and delete. */
    put("\x1b[2;1HABCDEF\x1b[2;2H\x1b[2@");
    NEED(row_is(1, "A  BCDEF") || (cell_cp(1, 1) == 0u && cell_cp(1, 3) == 'B' && cell_cp(1, 7) == 'F'),
         "insert two cells pushes the rest right");
    put("\x1b[2;2H\x1b[3P");
    NEED(cell_cp(1, 0) == 'A' && cell_cp(1, 1) == 'C' && cell_cp(1, 4) == 'F', "delete three pulls it left");

    /* A scroll region: a newline at its bottom scrolls it, and only it. */
    put("\x1b[2J\x1b[1;1Htop\x1b[2;1Hr2\x1b[3;1Hr3\x1b[4;1Hr4\x1b[5;1Hbelow");
    put("\x1b[2;4r\x1b[4;1H\n");
    NEED(row_is(0, "top") && row_is(1, "r3") && row_is(2, "r4") && row_is(3, "") && row_is(4, "below"),
         "a newline at the region's bottom scrolls the region");
    put("\x1b[2;1H\x1bM");
    NEED(row_is(1, "") && row_is(2, "r3") && row_is(4, "below"), "a reverse index at its top scrolls it down");
    put("\x1b[3;1H\x1b[L");
    NEED(row_is(2, "") && row_is(3, "r3") && row_is(4, "below"), "inserting a line stays inside the region");
    put("\x1b[r");

    /* The alternate screen keeps the main one. */
    put("\x1b[2J\x1b[Hmain\x1b[?1049h");
    vibeos_gui_term_modes(&cursor_on, &alt);
    NEED(alt == 1 && row_is(0, ""), "the alternate screen is up and clear");
    /* The cursor stays where it was, as xterm's does: home it first. */
    put("\x1b[Halt\x1b[?25l");
    vibeos_gui_term_modes(&cursor_on, &alt);
    NEED(cursor_on == 0 && row_is(0, "alt"), "the cursor hidden on it");
    put("\x1b[?1049l\x1b[?25h");
    vibeos_gui_term_modes(&cursor_on, &alt);
    NEED(alt == 0 && cursor_on == 1 && row_is(0, "main"), "and the main screen back, as it was");

    /* UTF-8 lands as one cell; the row as text says '?'. */
    put("\x1b[2J\x1b[H\xe2\x95\x94\xe2\x95\x90x");
    NEED(cell_cp(0, 0) == 0x2554u && cell_cp(0, 1) == 0x2550u && cell_cp(0, 2) == 'x' && row_is(0, "??x"),
         "box drawing takes a cell each");

    /* A window title does not reach the screen. */
    put("\x1b[2J\x1b[H\x1b]2;/tmp/x - fcmd\x07ok");
    NEED(row_is(0, "ok"), "a title is not text");

    /* Save and restore carry the pen. */
    put("\x1b[2J\x1b[H\x1b[31m\x1b" "7\x1b[0m\x1b[5;5H\x1b" "8z");
    NEED(cell_is(0, 0, 'z', 0xCD0000u, dbg), "restore puts the cursor and the pen back");
    (void)cell_fg;
    {
        vibeos_gui_stats_t st;

        vibeos_gui_stats(&st);
        NEED(st.term_overrun == 0u, "a sequence wrote outside the grid");
    }
    vibeos_gui_reset();
    return 0;
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
    return test_output() || test_input() || test_screen() || test_screen_full();
}
