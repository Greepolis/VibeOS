/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See vibeos/vt.h. No state outside the structures the callers own, and no
 * hardware: the display and the keyboard driver each hold one. */

#include "vibeos/vt.h"

/* ---- output -------------------------------------------------------------------- */

enum { VT_TEXT, VT_ESC, VT_CSI };

vibeos_vt_op_t vibeos_vt_feed(vibeos_vt_t *vt, char c, uint32_t *n, uint32_t *m) {
    unsigned char u = (unsigned char)c;
    uint32_t a, b;

    *n = *m = 0;
    if (vt->state == VT_TEXT) {
        if (u == 0x1bu) {
            vt->state = VT_ESC;
            return VIBEOS_VT_NONE;
        }
        return VIBEOS_VT_PRINT;
    }
    if (vt->state == VT_ESC) {
        if (u == '[') {
            vt->state = VT_CSI;
            vt->count = 0;
            vt->param[0] = vt->param[1] = 0;
        } else if (u != 0x1bu) {
            vt->state = VT_TEXT;    /* a two-byte sequence nothing here uses */
        }
        return VIBEOS_VT_NONE;
    }
    if (u >= '0' && u <= '9') {
        if (vt->count < 2u) {
            uint32_t v = vt->param[vt->count] * 10u + (u - '0');

            /* Saturated: a screen is not 65535 wide, and a number that wrapped
             * would move the cursor somewhere plausible. */
            vt->param[vt->count] = v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
        }
        return VIBEOS_VT_NONE;
    }
    if (u == ';') {
        if (vt->count < 2u) {
            vt->count++;
        }
        return VIBEOS_VT_NONE;
    }
    if (u < 0x40u || u > 0x7eu) {
        if (u == 0x18u || u == 0x1au || u == 0x1bu) {
            vt->state = u == 0x1bu ? VT_ESC : VT_TEXT;    /* abandoned */
        }
        return VIBEOS_VT_NONE;   /* `?` and the like: private, ends at its letter */
    }
    vt->state = VT_TEXT;
    a = vt->param[0];
    b = vt->param[1];
    switch (u) {
        case 'A': *n = a ? a : 1u; return VIBEOS_VT_UP;
        case 'B': *n = a ? a : 1u; return VIBEOS_VT_DOWN;
        case 'C': *n = a ? a : 1u; return VIBEOS_VT_RIGHT;
        case 'D': *n = a ? a : 1u; return VIBEOS_VT_LEFT;
        case 'H':
        case 'f':
            *n = a ? a - 1u : 0u;
            *m = b ? b - 1u : 0u;
            return VIBEOS_VT_GOTO;
        case 'J':
            return a == 0u ? VIBEOS_VT_ERASE_BELOW : a == 2u ? VIBEOS_VT_ERASE_ALL : VIBEOS_VT_NONE;
        case 'K':
            return a == 0u ? VIBEOS_VT_ERASE_LINE : VIBEOS_VT_NONE;
        default:
            return VIBEOS_VT_NONE;
    }
}

/* ---- input --------------------------------------------------------------------- */

static const char g_plain[0x3A] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b','\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`', 0,   '\\','z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '
};

static const char g_shifted[0x3A] = {
    0,   27,  '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b','\t',
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', 0,  'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' '
};

#define SC_LCTRL 0x1Du
#define SC_LSHIFT 0x2Au
#define SC_RSHIFT 0x36u
#define SC_ALT 0x38u
#define SC_CAPS 0x3Au

int vibeos_kbd_ctrl(const vibeos_kbd_t *k) {
    return k->ctrl_l || k->ctrl_r;
}

/* xterm's numbering: 1, plus 1 for Shift, 2 for Alt, 4 for Control. */
static uint32_t modifier(const vibeos_kbd_t *k) {
    return 1u + ((k->shift_l || k->shift_r) ? 1u : 0u) + (k->alt ? 2u : 0u) +
           (vibeos_kbd_ctrl(k) ? 4u : 0u);
}

static uint32_t arrow(const vibeos_kbd_t *k, char last, char *out) {
    uint32_t mod = modifier(k), n = 0;

    out[n++] = 27;
    out[n++] = '[';
    if (mod > 1u) {
        out[n++] = '1';
        out[n++] = ';';
        out[n++] = (char)('0' + mod);
    }
    out[n++] = last;
    return n;
}

static uint32_t tilde(const vibeos_kbd_t *k, char code, char *out) {
    uint32_t mod = modifier(k), n = 0;

    out[n++] = 27;
    out[n++] = '[';
    out[n++] = code;
    if (mod > 1u) {
        out[n++] = ';';
        out[n++] = (char)('0' + mod);
    }
    out[n++] = '~';
    return n;
}

uint32_t vibeos_kbd_key(vibeos_kbd_t *k, uint8_t sc, char out[VIBEOS_KBD_MAX]) {
    int extended, release;
    uint8_t code;
    char c;
    uint32_t n = 0;

    if (k->skip > 0u) {
        k->skip--;
        return 0;
    }
    if (sc == 0xE1u) {
        k->skip = 5u;   /* Pause: no release, and its bytes look like Ctrl and NumLock */
        k->e0 = 0;
        return 0;
    }
    if (sc == 0xE0u) {
        k->e0 = 1;
        return 0;
    }
    extended = k->e0;
    k->e0 = 0;
    release = (sc & 0x80u) != 0u;
    code = sc & 0x7Fu;

    /* Print Screen and a few others wrap themselves in a false Shift, E0 2A. It
     * is not the Shift key, and believing it leaves Shift held. */
    if (extended && (code == SC_LSHIFT || code == SC_RSHIFT)) {
        return 0;
    }
    if (code == SC_LCTRL) {
        if (extended) {
            k->ctrl_r = !release;
        } else {
            k->ctrl_l = !release;
        }
        return 0;
    }
    if (code == SC_LSHIFT) {
        k->shift_l = !release;
        return 0;
    }
    if (code == SC_RSHIFT) {
        k->shift_r = !release;
        return 0;
    }
    if (code == SC_ALT) {
        /* The right one is AltGr, which this US map has no use for. */
        if (!extended) {
            k->alt = !release;
        }
        return 0;
    }
    if (release) {
        return 0;
    }
    if (code == SC_CAPS) {
        k->caps = !k->caps;
        return 0;
    }
    /* The navigation keys, and the keypad's with NumLock off: same codes. */
    switch (code) {
        case 0x48u: return arrow(k, 'A', out);
        case 0x50u: return arrow(k, 'B', out);
        case 0x4Du: return arrow(k, 'C', out);
        case 0x4Bu: return arrow(k, 'D', out);
        case 0x47u: return arrow(k, 'H', out);
        case 0x4Fu: return arrow(k, 'F', out);
        case 0x53u: return tilde(k, '3', out);
        case 0x49u: return tilde(k, '5', out);
        case 0x51u: return tilde(k, '6', out);
        default: break;
    }
    if (extended) {
        if (code == 0x1Cu) {
            out[0] = '\n';      /* keypad Enter */
            return 1;
        }
        if (code == 0x35u) {
            out[0] = '/';       /* keypad slash */
            return 1;
        }
        return 0;
    }
    if (code >= 0x3Au) {
        return 0;
    }
    {
        int shifted = k->shift_l || k->shift_r;
        char base = g_plain[code];

        /* Caps Lock is for letters: it does not turn 1 into !. */
        if (k->caps && base >= 'a' && base <= 'z') {
            shifted = !shifted;
        }
        c = shifted ? g_shifted[code] : base;
    }
    if (c == 0) {
        return 0;
    }
    /* Control and a letter is a control character, whichever case. */
    if (vibeos_kbd_ctrl(k)) {
        char low = g_plain[code];

        if (low >= 'a' && low <= 'z') {
            c = (char)(low - 'a' + 1);
        } else if (low == '\\') {
            c = 28;
        }
    }
    /* Alt is an ESC in front of the key, as a terminal sends it. */
    if (k->alt) {
        out[n++] = 27;
    }
    out[n++] = c;
    return n;
}
