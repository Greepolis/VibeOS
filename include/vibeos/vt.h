/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_VT_H
#define VIBEOS_VT_H

/* The two halves of a text console that are the same on every screen and every
 * keyboard, and have no hardware in them, so the host can test them:
 *
 * - what a stream of output bytes asks the screen to do: the ANSI sequences a
 *   line editor needs - moving the cursor and erasing - and, since S7, what a
 *   full-screen program needs besides: colours, inserting and deleting, a
 *   scroll region, the alternate screen, and UTF-8;
 * - what a stream of PS/2 set-1 scan codes means as terminal input. */

#include <stdint.h>

typedef enum {
    VIBEOS_VT_NONE,        /* part of a sequence, or one nothing acts on */
    VIBEOS_VT_PRINT,       /* a character: *n is its code point (a control
                            * character below 0x20 included, for the screen's
                            * own handling of \r, \n, \b and \t) */
    VIBEOS_VT_UP,          /* by n */
    VIBEOS_VT_DOWN,
    VIBEOS_VT_RIGHT,
    VIBEOS_VT_LEFT,
    VIBEOS_VT_GOTO,        /* row n, column m, from zero */
    VIBEOS_VT_ERASE_BELOW, /* from the cursor to the end of the screen */
    VIBEOS_VT_ERASE_ALL,
    VIBEOS_VT_ERASE_LINE,  /* from the cursor to the end of the row */
    /* Since S7 (docs/shell/s7.md). */
    VIBEOS_VT_SGR,         /* colours and attributes: read vt->param[0..vt->count] */
    VIBEOS_VT_COLUMN,      /* to column n, from zero, on this row */
    VIBEOS_VT_ROW,         /* to row n, from zero, in this column */
    VIBEOS_VT_ERASE_ABOVE, /* from the start of the screen to the cursor */
    VIBEOS_VT_ERASE_LINE_LEFT, /* from the start of the row to the cursor */
    VIBEOS_VT_ERASE_LINE_ALL,
    VIBEOS_VT_ERASE_CHARS, /* n cells from the cursor, which stays */
    VIBEOS_VT_INSERT_CHARS,/* n blank cells at the cursor, the rest pushed right */
    VIBEOS_VT_DELETE_CHARS,/* n cells at the cursor, the rest pulled left */
    VIBEOS_VT_INSERT_LINES,/* n blank rows at the cursor's, inside the region */
    VIBEOS_VT_DELETE_LINES,
    VIBEOS_VT_SCROLL_UP,   /* the region by n */
    VIBEOS_VT_SCROLL_DOWN,
    VIBEOS_VT_REGION,      /* rows n to m, one-based as written; 0 is the default */
    VIBEOS_VT_SAVE,        /* the cursor and the pen */
    VIBEOS_VT_RESTORE,
    VIBEOS_VT_INDEX,       /* down one, scrolling the region at its bottom */
    VIBEOS_VT_REVERSE_INDEX, /* up one, scrolling the region at its top */
    VIBEOS_VT_NEXT_LINE,   /* an index and a carriage return */
    VIBEOS_VT_CURSOR_SHOW,
    VIBEOS_VT_CURSOR_HIDE,
    VIBEOS_VT_ALT_ON,      /* the alternate screen, cleared, the cursor saved */
    VIBEOS_VT_ALT_OFF,
    VIBEOS_VT_RESET
} vibeos_vt_op_t;

/* What a sequence may carry. Sixteen is more than any SGR a program writes:
 * a foreground and a background in 24-bit colour with two attributes is
 * twelve. */
#define VIBEOS_VT_PARAMS 16u

/* The code point a broken UTF-8 sequence prints as. */
#define VIBEOS_VT_REPLACEMENT 0xFFFDu

typedef struct {
    uint8_t state;
    uint8_t count;        /* the index of the parameter being read */
    uint8_t priv;         /* `?`, `>`, `<` or `=` after the `[`, or 0 */
    uint8_t inter;        /* an intermediate byte was seen: nothing here acts on one */
    uint8_t utf_need;     /* continuation bytes still to come */
    uint8_t utf_len;      /* and how many the sequence has in all */
    uint16_t param[VIBEOS_VT_PARAMS];
    uint32_t cp;          /* the code point being assembled */
} vibeos_vt_t;

/* One byte. *n and *m are the operation's numbers. A sequence nobody here acts
 * on is consumed and is VIBEOS_VT_NONE: it must not print. */
vibeos_vt_op_t vibeos_vt_feed(vibeos_vt_t *vt, char c, uint32_t *n, uint32_t *m);

/* The longest a key can be as input: ESC [ 1 ; 8 D, and a little. */
#define VIBEOS_KBD_MAX 8u

typedef struct {
    uint8_t e0;           /* the last byte was the extended prefix */
    uint8_t skip;         /* bytes left of Pause, which is six long */
    uint8_t shift_l, shift_r, ctrl_l, ctrl_r, alt, caps;
} vibeos_kbd_t;

/* One scan code; returns how many bytes of input it makes (0 for a release or a
 * modifier), written to `out`. US layout. */
uint32_t vibeos_kbd_key(vibeos_kbd_t *k, uint8_t sc, char out[VIBEOS_KBD_MAX]);

/* Whether Control is held, for the driver's question to the terminal. */
int vibeos_kbd_ctrl(const vibeos_kbd_t *k);

#endif
