/* SPDX-License-Identifier: GPL-3.0-or-later */
/* See vibeos/vt.h. No state outside the structures the callers own, and no
 * hardware: the display and the keyboard driver each hold one. */

#include "vibeos/vt.h"

/* ---- output -------------------------------------------------------------------- */

/* VT_STRING is an OSC, DCS, APC or PM: a string that ends at BEL or at
 * ESC \, of which nothing is used. Before S7 an OSC was text after its first
 * byte, and the window title FinalCommander sets was printed on the screen.
 * VT_SKIP1 is a character-set designation, ESC ( B and its kin, which is one
 * byte more. */
enum { VT_TEXT, VT_ESC, VT_CSI, VT_STRING, VT_STRING_ESC, VT_SKIP1, VT_UTF8 };

static void csi_start(vibeos_vt_t *vt) {
    uint32_t i;

    vt->state = VT_CSI;
    vt->count = 0;
    vt->priv = 0;
    vt->inter = 0;
    for (i = 0; i < VIBEOS_VT_PARAMS; i++) {
        vt->param[i] = 0;
    }
}

/* A byte that starts something, in text: the escape, or a UTF-8 lead. */
static vibeos_vt_op_t text_byte(vibeos_vt_t *vt, unsigned char u, uint32_t *n) {
    if (u == 0x1bu) {
        vt->state = VT_ESC;
        return VIBEOS_VT_NONE;
    }
    if (u < 0x80u) {
        *n = u;
        return VIBEOS_VT_PRINT;
    }
    /* C0 and C1 would be overlong for two bytes; F5 and above past U+10FFFF. */
    if (u >= 0xC2u && u <= 0xDFu) {
        vt->cp = u & 0x1Fu;
        vt->utf_need = vt->utf_len = 1;
    } else if (u >= 0xE0u && u <= 0xEFu) {
        vt->cp = u & 0x0Fu;
        vt->utf_need = vt->utf_len = 2;
    } else if (u >= 0xF0u && u <= 0xF4u) {
        vt->cp = u & 0x07u;
        vt->utf_need = vt->utf_len = 3;
    } else {
        *n = VIBEOS_VT_REPLACEMENT;   /* a stray continuation or an invalid lead */
        return VIBEOS_VT_PRINT;
    }
    vt->state = VT_UTF8;
    return VIBEOS_VT_NONE;
}

static vibeos_vt_op_t utf8_byte(vibeos_vt_t *vt, unsigned char u, uint32_t *n) {
    if ((u & 0xC0u) != 0x80u) {
        /* Cut short. The replacement says so; the byte that cut it is not
         * replayed, except an escape, which must not be lost or the sequence
         * after it prints. */
        vt->state = u == 0x1bu ? VT_ESC : VT_TEXT;
        *n = VIBEOS_VT_REPLACEMENT;
        return VIBEOS_VT_PRINT;
    }
    vt->cp = (vt->cp << 6) | (u & 0x3Fu);
    if (--vt->utf_need != 0u) {
        return VIBEOS_VT_NONE;
    }
    vt->state = VT_TEXT;
    /* Overlong forms and surrogates are not characters. */
    if ((vt->utf_len == 2u && vt->cp < 0x800u) || (vt->utf_len == 3u && (vt->cp < 0x10000u || vt->cp > 0x10FFFFu)) ||
        (vt->cp >= 0xD800u && vt->cp <= 0xDFFFu)) {
        *n = VIBEOS_VT_REPLACEMENT;
    } else {
        *n = vt->cp;
    }
    return VIBEOS_VT_PRINT;
}

static vibeos_vt_op_t esc_byte(vibeos_vt_t *vt, unsigned char u) {
    vt->state = VT_TEXT;
    switch (u) {
        case '[': csi_start(vt); return VIBEOS_VT_NONE;
        case ']':
        case 'P':
        case '_':
        case '^':
            vt->state = VT_STRING;
            return VIBEOS_VT_NONE;
        case '(':
        case ')':
        case '*':
        case '+':
        case '-':
        case '.':
        case '/':
            vt->state = VT_SKIP1;
            return VIBEOS_VT_NONE;
        case 0x1bu: vt->state = VT_ESC; return VIBEOS_VT_NONE;
        case '7': return VIBEOS_VT_SAVE;
        case '8': return VIBEOS_VT_RESTORE;
        case 'D': return VIBEOS_VT_INDEX;
        case 'E': return VIBEOS_VT_NEXT_LINE;
        case 'M': return VIBEOS_VT_REVERSE_INDEX;
        case 'c': return VIBEOS_VT_RESET;
        default: return VIBEOS_VT_NONE;   /* a two-byte sequence nothing here uses */
    }
}

/* Whether parameter `want` is among those of a private mode: ?1049;1006h sets
 * two at once. */
static int has_param(const vibeos_vt_t *vt, uint32_t want) {
    uint32_t i;

    for (i = 0; i <= vt->count && i < VIBEOS_VT_PARAMS; i++) {
        if (vt->param[i] == want) {
            return 1;
        }
    }
    return 0;
}

static vibeos_vt_op_t csi_final(vibeos_vt_t *vt, unsigned char u, uint32_t *n, uint32_t *m) {
    uint32_t a = vt->param[0], b = vt->param[1], one = a ? a : 1u;

    vt->state = VT_TEXT;
    if (vt->inter) {
        return VIBEOS_VT_NONE;   /* the cursor's shape, and the like */
    }
    if (vt->priv == '?') {
        if (u == 'h' || u == 'l') {
            if (has_param(vt, 25u)) {
                return u == 'h' ? VIBEOS_VT_CURSOR_SHOW : VIBEOS_VT_CURSOR_HIDE;
            }
            if (has_param(vt, 1049u) || has_param(vt, 1047u) || has_param(vt, 47u)) {
                return u == 'h' ? VIBEOS_VT_ALT_ON : VIBEOS_VT_ALT_OFF;
            }
        }
        return VIBEOS_VT_NONE;   /* other modes, and queries such as ?u */
    }
    if (vt->priv) {
        return VIBEOS_VT_NONE;   /* `>`, `<`, `=`: keyboard modes and queries */
    }
    switch (u) {
        case 'A': *n = one; return VIBEOS_VT_UP;
        case 'B': *n = one; return VIBEOS_VT_DOWN;
        case 'C': *n = one; return VIBEOS_VT_RIGHT;
        case 'D': *n = one; return VIBEOS_VT_LEFT;
        case 'H':
        case 'f':
            *n = a ? a - 1u : 0u;
            *m = b ? b - 1u : 0u;
            return VIBEOS_VT_GOTO;
        case 'G':
        case '`':
            *n = a ? a - 1u : 0u;
            return VIBEOS_VT_COLUMN;
        case 'd':
            *n = a ? a - 1u : 0u;
            return VIBEOS_VT_ROW;
        case 'J':
            return a == 0u   ? VIBEOS_VT_ERASE_BELOW
                   : a == 1u ? VIBEOS_VT_ERASE_ABOVE
                   : a <= 3u ? VIBEOS_VT_ERASE_ALL
                             : VIBEOS_VT_NONE;
        case 'K':
            return a == 0u   ? VIBEOS_VT_ERASE_LINE
                   : a == 1u ? VIBEOS_VT_ERASE_LINE_LEFT
                   : a == 2u ? VIBEOS_VT_ERASE_LINE_ALL
                             : VIBEOS_VT_NONE;
        case 'X': *n = one; return VIBEOS_VT_ERASE_CHARS;
        case '@': *n = one; return VIBEOS_VT_INSERT_CHARS;
        case 'P': *n = one; return VIBEOS_VT_DELETE_CHARS;
        case 'L': *n = one; return VIBEOS_VT_INSERT_LINES;
        case 'M': *n = one; return VIBEOS_VT_DELETE_LINES;
        case 'S': *n = one; return VIBEOS_VT_SCROLL_UP;
        case 'T': *n = one; return VIBEOS_VT_SCROLL_DOWN;
        case 'r':
            *n = a;
            *m = b;
            return VIBEOS_VT_REGION;
        case 's': return VIBEOS_VT_SAVE;
        case 'u': return VIBEOS_VT_RESTORE;
        case 'm': return VIBEOS_VT_SGR;
        default: return VIBEOS_VT_NONE;   /* queries, window operations, ... */
    }
}

vibeos_vt_op_t vibeos_vt_feed(vibeos_vt_t *vt, char c, uint32_t *n, uint32_t *m) {
    unsigned char u = (unsigned char)c;

    *n = *m = 0;
    switch (vt->state) {
        case VT_TEXT: return text_byte(vt, u, n);
        case VT_UTF8: return utf8_byte(vt, u, n);
        case VT_ESC: return esc_byte(vt, u);
        case VT_SKIP1:
            vt->state = VT_TEXT;
            return VIBEOS_VT_NONE;
        case VT_STRING:
            if (u == 0x07u) {
                vt->state = VT_TEXT;
            } else if (u == 0x1bu) {
                vt->state = VT_STRING_ESC;
            }
            return VIBEOS_VT_NONE;
        case VT_STRING_ESC:
            /* ESC \ ends it. Any other escape abandons the string and starts
             * what it starts, as xterm has it. */
            if (u == '\\') {
                vt->state = VT_TEXT;
                return VIBEOS_VT_NONE;
            }
            return esc_byte(vt, u);
        default:
            break;
    }
    /* VT_CSI */
    if (u >= '0' && u <= '9') {
        if (vt->count < VIBEOS_VT_PARAMS) {
            uint32_t v = vt->param[vt->count] * 10u + (u - '0');

            /* Saturated: a screen is not 65535 wide, and a number that wrapped
             * would move the cursor somewhere plausible. */
            vt->param[vt->count] = v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
        }
        return VIBEOS_VT_NONE;
    }
    if (u == ';' || u == ':') {
        /* A colon separates the parts of one parameter (38:2:r:g:b). Read as
         * a semicolon it lands the same numbers in the same order; the form
         * with an empty colour-space slot (38:2::r:g:b) is read one place off
         * and draws the wrong colour, which is written down rather than
         * handled - crossterm, ncurses and the C libraries write semicolons. */
        if (vt->count < VIBEOS_VT_PARAMS) {
            vt->count++;
        }
        return VIBEOS_VT_NONE;
    }
    if (u >= 0x3Cu && u <= 0x3Fu) {
        if (vt->count == 0u && vt->param[0] == 0u && !vt->priv) {
            vt->priv = u;
        }
        return VIBEOS_VT_NONE;
    }
    if (u >= 0x20u && u <= 0x2Fu) {
        vt->inter = 1;
        return VIBEOS_VT_NONE;
    }
    if (u < 0x40u || u > 0x7eu) {
        if (u == 0x18u || u == 0x1au || u == 0x1bu) {
            vt->state = u == 0x1bu ? VT_ESC : VT_TEXT;    /* abandoned */
        }
        return VIBEOS_VT_NONE;
    }
    if (vt->count >= VIBEOS_VT_PARAMS) {
        vt->count = VIBEOS_VT_PARAMS - 1u;   /* what was read is what is used */
    }
    return csi_final(vt, u, n, m);
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
