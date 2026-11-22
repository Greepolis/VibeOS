/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_VT_H
#define VIBEOS_VT_H

/* The two halves of a text console that are the same on every screen and every
 * keyboard, and have no hardware in them, so the host can test them:
 *
 * - what a stream of output bytes asks the screen to do (the ANSI sequences a
 *   line editor needs: moving the cursor and erasing);
 * - what a stream of PS/2 set-1 scan codes means as terminal input. */

#include <stdint.h>

typedef enum {
    VIBEOS_VT_NONE,        /* part of a sequence; nothing to do yet */
    VIBEOS_VT_PRINT,       /* an ordinary byte, for the screen's own handling */
    VIBEOS_VT_UP,          /* by n */
    VIBEOS_VT_DOWN,
    VIBEOS_VT_RIGHT,
    VIBEOS_VT_LEFT,
    VIBEOS_VT_GOTO,        /* row n, column m, from zero */
    VIBEOS_VT_ERASE_BELOW, /* from the cursor to the end of the screen */
    VIBEOS_VT_ERASE_ALL,
    VIBEOS_VT_ERASE_LINE   /* from the cursor to the end of the row */
} vibeos_vt_op_t;

typedef struct {
    uint8_t state;
    uint8_t count;
    uint16_t param[2];
} vibeos_vt_t;

/* One byte. *n and *m are the operation's numbers. A sequence nobody here acts
 * on (colours, modes) is consumed and is VIBEOS_VT_NONE: it must not print. */
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
