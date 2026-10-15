/* The printf subset DbgPrint needs (docs/abi/windows.md W2), with Windows'
 * meanings on x64 (LLP64), which are not the C library's on Linux:
 *
 *   %d %i %u %x %X %o    32 bits; with l the same (long is 32 bits); with ll, I64,
 *                        j, z, t or I 64 bits; h and hh narrow it
 *   %c %C                a char, and a wide char (the other way round with l, w, h)
 *   %s %S                an ANSI string, and a wide one (the other way round with
 *                        l, w, h); a null pointer prints "(null)"
 *   %p                   a pointer as 16 upper-case hex digits
 *   %%
 *
 * Flags - 0 + space #, a width and a precision (each also from an argument with
 * *) are honoured. A wide string is narrowed to the output's bytes: Latin-1
 * passes through and anything above U+00FF becomes '?'.
 *
 * The output is NUL-terminated and cut at the buffer's end; the function does not
 * report what a longer buffer would have held, because DbgPrint truncates and
 * moves on. */

#include "rtl_internal.h"

typedef struct {
    char  *buf;
    size_t cap;      /* bytes, the NUL included */
    size_t len;      /* bytes stored so far */
} out_t;

static void put(out_t *o, char c) {
    if (o->len + 1u < o->cap) {
        o->buf[o->len++] = c;
    }
}

static void pad(out_t *o, char c, long n) {
    while (n-- > 0) {
        put(o, c);
    }
}

/* One converted field: sign/prefix and digits, padded to the width. */
static void field(out_t *o, const char *prefix, size_t plen, const char *body, size_t blen, int left,
                  int zero, long width) {
    long total = (long)(plen + blen), fill = width > total ? width - total : 0;
    size_t i;

    if (!left && !zero) {
        pad(o, ' ', fill);
    }
    for (i = 0; i < plen; i++) {
        put(o, prefix[i]);
    }
    if (!left && zero) {
        pad(o, '0', fill);
    }
    for (i = 0; i < blen; i++) {
        put(o, body[i]);
    }
    if (left) {
        pad(o, ' ', fill);
    }
}

static char narrow(vibeos_wchar_t w) { return w < 0x100 ? (char)w : '?'; }

size_t vibeos_rtl_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    out_t o;

    o.buf = buf;
    o.cap = cap;
    o.len = 0;
    if (cap == 0) {
        return 0;
    }
    for (; *fmt; fmt++) {
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        long width = 0, prec = -1;
        int lenmod = 0;          /* 0 int, 1 hh, 2 h, 3 l, 4 64-bit, 5 w */
        char conv;

        if (*fmt != '%') {
            put(&o, *fmt);
            continue;
        }
        fmt++;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else if (*fmt == '0') zero = 1;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt++ - '0');
            }
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt++ - '0');
                }
            }
        }
        for (;; fmt++) {
            if (*fmt == 'h') lenmod = lenmod == 2 ? 1 : 2;
            else if (*fmt == 'l') lenmod = lenmod == 3 ? 4 : 3;
            else if (*fmt == 'w') lenmod = 5;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'L') lenmod = 4;
            else if (*fmt == 'I') {
                /* I64 and I32 name a width; a bare I is a pointer-sized integer. */
                if (fmt[1] == '6' && fmt[2] == '4') { lenmod = 4; fmt += 2; }
                else if (fmt[1] == '3' && fmt[2] == '2') { lenmod = 0; fmt += 2; }
                else lenmod = 4;
            } else break;
        }
        conv = *fmt;
        if (!conv) {
            break;
        }
        if (prec >= 0) {
            zero = 0;            /* a precision turns the 0 flag off for integers */
        }
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            char digits[72];
            char prefix[3];
            size_t plen = 0, nd = 0, i;
            uint64_t v;
            unsigned base = 10;
            int neg = 0, upper = (conv == 'X' || conv == 'p');

            if (conv == 'p') {
                v = (uint64_t)(uintptr_t)va_arg(ap, void *);
                base = 16;
                prec = 16;
                zero = 0;
            } else if (conv == 'd' || conv == 'i') {
                int64_t s = lenmod == 4 ? va_arg(ap, int64_t) : (int64_t)va_arg(ap, int);

                if (lenmod == 1) s = (int8_t)s;
                else if (lenmod == 2) s = (int16_t)s;
                neg = s < 0;
                v = neg ? (uint64_t)0 - (uint64_t)s : (uint64_t)s;
            } else {
                v = lenmod == 4 ? va_arg(ap, uint64_t) : (uint64_t)va_arg(ap, unsigned);

                if (lenmod == 1) v = (uint8_t)v;
                else if (lenmod == 2) v = (uint16_t)v;
                base = conv == 'o' ? 8u : (conv == 'u' ? 10u : 16u);
            }
            if (neg) {
                prefix[plen++] = '-';
            } else if ((conv == 'd' || conv == 'i') && plus) {
                prefix[plen++] = '+';
            } else if ((conv == 'd' || conv == 'i') && space) {
                prefix[plen++] = ' ';
            }
            if (alt && v != 0 && base == 16 && conv != 'p') {
                prefix[plen++] = '0';
                prefix[plen++] = upper ? 'X' : 'x';
            }
            do {
                unsigned d = (unsigned)(v % base);

                digits[nd++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10);
                v /= base;
            } while (v && nd < sizeof(digits));
            while ((long)nd < prec && nd < sizeof(digits)) {
                digits[nd++] = '0';
            }
            if (prec == 0 && nd == 1 && digits[0] == '0') {
                nd = 0;          /* "%.0d" of zero prints nothing */
            }
            /* digits are least significant first */
            for (i = 0; i < nd / 2; i++) {
                char t = digits[i];

                digits[i] = digits[nd - 1 - i];
                digits[nd - 1 - i] = t;
            }
            field(&o, prefix, plen, digits, nd, left, zero, width);
            break;
        }
        case 'c': case 'C': {
            int wide = (conv == 'c') ? (lenmod == 3 || lenmod == 5) : !(lenmod == 2);
            int arg = va_arg(ap, int);
            char ch = wide ? narrow((vibeos_wchar_t)arg) : (char)arg;

            field(&o, "", 0, &ch, 1, left, 0, width);
            break;
        }
        case 's': case 'S': {
            int wide = (conv == 's') ? (lenmod == 3 || lenmod == 5) : !(lenmod == 2);
            const void *p = va_arg(ap, const void *);
            char tmp[256];
            size_t n = 0;

            if (!p) {
                const char *nul = "(null)";

                while (nul[n] && (prec < 0 || (long)n < prec)) {
                    n++;
                }
                field(&o, "", 0, nul, n, left, 0, width);
                break;
            }
            if (!wide) {
                const char *s = (const char *)p;

                while (s[n] && (prec < 0 || (long)n < prec)) {
                    n++;
                }
                field(&o, "", 0, s, n, left, 0, width);
            } else {
                const vibeos_wchar_t *w = (const vibeos_wchar_t *)p;
                size_t k = 0;

                while (w[n] && (prec < 0 || (long)n < prec)) {
                    n++;
                }
                /* Narrowed in pieces so that a long string needs no buffer of its
                 * own; the padding is decided from the whole length first. */
                if (!left) {
                    pad(&o, ' ', width - (long)n);
                }
                while (k < n) {
                    size_t j, m = n - k < sizeof(tmp) ? n - k : sizeof(tmp);

                    for (j = 0; j < m; j++) {
                        tmp[j] = narrow(w[k + j]);
                    }
                    for (j = 0; j < m; j++) {
                        put(&o, tmp[j]);
                    }
                    k += m;
                }
                if (left) {
                    pad(&o, ' ', width - (long)n);
                }
            }
            break;
        }
        case '%':
            put(&o, '%');
            break;
        default:
            /* An unknown conversion is printed as written, as the C runtime does. */
            put(&o, '%');
            put(&o, conv);
            break;
        }
    }
    o.buf[o.len] = 0;
    return o.len;
}
