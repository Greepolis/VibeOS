/* IPv4 and IPv6 addresses to and from text (docs/abi/windows.md W2).
 *
 * Both parsers work on arrays of 16-bit units, so the ANSI and wide entry points
 * share one: the narrow string is widened into a local buffer first (a valid
 * address is under 46 characters, so only the head of a longer string matters
 * and a terminator found there is mapped back to the caller's pointer by
 * index).
 *
 * Every parser reports where it stopped through *Terminator: the NUL on success,
 * the first character that does not belong on failure. */

#include "rtl_internal.h"

#define WIDEN_MAX 128u

/* ---- IPv4 --------------------------------------------------------------------------- */

/* Parse the numbers of an IPv4 address from s. Sets *end to the first character
 * that is not part of it; returns 0 and the four bytes if what was consumed is
 * a valid address, or -1.
 *
 * Strict: exactly four decimal parts, each at most 255. Otherwise one to four
 * parts, each decimal, octal (leading 0) or hexadecimal (0x), where the last
 * part fills the bytes that are left (a.b.c is a, b and a 16-bit c). */
static int v4_scan(const vibeos_wchar_t *s, int strict, size_t *end, uint8_t out[4]) {
    uint64_t part[4];
    int n = 0, k;
    size_t i = 0;

    for (;;) {
        unsigned base = 10;
        uint64_t v = 0;
        size_t digits = 0;

        if (!strict && s[i] == '0') {
            if (s[i + 1] == 'x' || s[i + 1] == 'X') {
                base = 16;
                i += 2;
            } else if (s[i + 1] >= '0' && s[i + 1] <= '9') {
                base = 8;
                i += 1;
            }
        }
        for (;; i++) {
            int d = s[i] < 128 ? rtl_hexval(s[i]) : -1;

            if (d < 0 || (unsigned)d >= base) {
                /* A digit that is valid in no base ends the number; one valid
                 * only in a larger base ("08" in octal) is an error where it
                 * stands, which the caller sees as a stop at that digit. */
                break;
            }
            v = v * base + (unsigned)d;
            if (v > 0xFFFFFFFFull) {
                *end = i;
                return -1;
            }
            digits++;
        }
        if (digits == 0) {
            *end = i;
            return -1;
        }
        part[n++] = v;
        if (s[i] == '.' && n < 4) {
            i++;
            continue;
        }
        break;
    }
    *end = i;
    if (strict && n != 4) {
        return -1;
    }
    for (k = 0; k < n - 1; k++) {
        if (part[k] > 255) {
            return -1;
        }
    }
    if (part[n - 1] > (n == 1 ? 0xFFFFFFFFull : (1ull << (8 * (5 - n))) - 1ull)) {
        return -1;
    }
    for (k = 0; k < n - 1; k++) {
        out[k] = (uint8_t)part[k];
    }
    {
        int left = 4 - (n - 1);
        uint64_t last = part[n - 1];

        for (k = 0; k < left; k++) {
            out[n - 1 + k] = (uint8_t)(last >> (8 * (left - 1 - k)));
        }
    }
    return 0;
}

static void widen(const char *s, vibeos_wchar_t *w) {
    size_t i;

    for (i = 0; i < WIDEN_MAX - 1u && s[i]; i++) {
        w[i] = (unsigned char)s[i];
    }
    w[i] = 0;
}

vibeos_ntstatus_t RtlIpv4StringToAddressA(const char *str, vibeos_boolean_t strict,
                                          const char **terminator, vibeos_in_addr_t *addr) {
    vibeos_wchar_t w[WIDEN_MAX];
    uint8_t b[4];
    size_t end;
    int rc;

    if (!str || !addr) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    widen(str, w);
    rc = v4_scan(w, strict != 0, &end, b);
    /* A valid address followed by anything but the end of the string is not one:
     * "1.2.3.4x" stops at the x. */
    if (rc == 0 && w[end] != 0) {
        rc = -1;
    }
    if (terminator) {
        *terminator = str + end;
    }
    if (rc != 0) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    memcpy(addr->Byte, b, 4);
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}

/* Appends the decimal of v at p; returns the new end. */
static char *put_dec(char *p, unsigned v) {
    char t[10];
    int n = 0;

    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n) {
        *p++ = t[--n];
    }
    return p;
}

char *RtlIpv4AddressToStringA(const vibeos_in_addr_t *addr, char *str) {
    int i;

    for (i = 0; i < 4; i++) {
        if (i) {
            *str++ = '.';
        }
        str = put_dec(str, addr->Byte[i]);
    }
    *str = 0;
    return str;                       /* the terminator, so a caller can append */
}

/* ---- IPv6 --------------------------------------------------------------------------- */

/* Parse an IPv6 address from s. *end is where it stopped. */
static int v6_scan(const vibeos_wchar_t *s, size_t *end, uint8_t out[16]) {
    uint16_t g[8];
    int ng = 0, gap = -1, i;
    size_t p = 0;

    if (s[0] == ':') {
        if (s[1] != ':') {
            *end = 0;
            return -1;                /* a single leading colon */
        }
        gap = 0;
        p = 2;
        if (s[p] >= 128 || rtl_hexval(s[p]) < 0) {
            goto done;                /* "::" alone */
        }
    }
    for (;;) {
        size_t q = p;
        unsigned v = 0;
        int digits = 0;

        while (s[q] < 128 && rtl_hexval(s[q]) >= 0) {
            q++;
        }
        if (s[q] == '.') {
            /* The last 32 bits written as an IPv4 address; always strict. */
            uint8_t b[4];
            size_t e = 0;

            if (ng > 6) {
                *end = p;
                return -1;
            }
            if (v4_scan(s + p, 1, &e, b) != 0) {
                *end = p + e;
                return -1;
            }
            g[ng++] = (uint16_t)((b[0] << 8) | b[1]);
            g[ng++] = (uint16_t)((b[2] << 8) | b[3]);
            p += e;
            break;
        }
        for (; digits < 5 && s[p] < 128 && rtl_hexval(s[p]) >= 0; p++, digits++) {
            v = v * 16u + (unsigned)rtl_hexval(s[p]);
        }
        if (digits == 0 || digits > 4 || ng == 8) {
            *end = p;
            return -1;
        }
        g[ng++] = (uint16_t)v;
        if (s[p] != ':') {
            break;
        }
        if (s[p + 1] == ':') {
            if (gap >= 0) {
                *end = p + 1;
                return -1;            /* "::" twice */
            }
            gap = ng;
            p += 2;
            if (s[p] >= 128 || rtl_hexval(s[p]) < 0) {
                break;                /* a trailing "::" */
            }
        } else {
            p++;
        }
    }
done:
    *end = p;
    if (gap < 0 ? ng != 8 : ng > 7) {
        return -1;
    }
    {
        /* The groups after the gap move up by what the gap stands for; the
         * zeros between are the caller's (v6_parse clears the buffer). */
        int fill = 8 - ng, o = 0;

        for (i = 0; i < ng; i++) {
            if (i == gap) {
                o += fill;
            }
            out[o * 2] = (uint8_t)(g[i] >> 8);
            out[o * 2 + 1] = (uint8_t)g[i];
            o++;
        }
    }
    return 0;
}

/* The result needs the skipped groups to be zero, which v6_scan cannot assume of
 * its caller's buffer: clear first, then scan. */
static vibeos_ntstatus_t v6_parse(const vibeos_wchar_t *w, size_t *end, vibeos_in6_addr_t *addr) {
    uint8_t b[16];
    int rc;

    memset(b, 0, sizeof(b));
    rc = v6_scan(w, end, b);
    if (rc == 0 && w[*end] != 0) {
        rc = -1;                      /* trailing characters */
    }
    if (rc != 0) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    memcpy(addr->Byte, b, 16);
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}

vibeos_ntstatus_t RtlIpv6StringToAddressW(const vibeos_wchar_t *str, const vibeos_wchar_t **terminator,
                                          vibeos_in6_addr_t *addr) {
    size_t end;
    vibeos_ntstatus_t st;
    vibeos_wchar_t w[WIDEN_MAX];
    size_t n = 0;

    if (!str || !addr) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    /* Parse a bounded copy so the scanner may look one unit ahead of a
     * terminator without leaving the caller's string. */
    while (n < WIDEN_MAX - 1u && str[n]) {
        w[n] = str[n];
        n++;
    }
    w[n] = 0;
    st = v6_parse(w, &end, addr);
    if (terminator) {
        *terminator = str + end;
    }
    return st;
}

vibeos_ntstatus_t RtlIpv6StringToAddressA(const char *str, const char **terminator,
                                          vibeos_in6_addr_t *addr) {
    vibeos_wchar_t w[WIDEN_MAX];
    size_t end;
    vibeos_ntstatus_t st;

    if (!str || !addr) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    widen(str, w);
    st = v6_parse(w, &end, addr);
    if (terminator) {
        *terminator = str + end;
    }
    return st;
}

static char *put_hex(char *p, unsigned v) {
    static const char digits[] = "0123456789abcdef";
    int shift = 12;

    while (shift > 0 && ((v >> shift) & 0xFu) == 0) {
        shift -= 4;
    }
    for (; shift >= 0; shift -= 4) {
        *p++ = digits[(v >> shift) & 0xFu];
    }
    return p;
}

/* Lower case, no leading zeros, the longest run of two or more zero groups
 * written "::" (the first, when two are as long), and an IPv4-mapped address
 * (::ffff:a.b.c.d) with its last 32 bits in dotted form. Other embedded-IPv4
 * forms Windows prints in dotted form too (::a.b.c.d, the ISATAP ones); those
 * are written in hex here until a test asks. */
char *RtlIpv6AddressToStringA(const vibeos_in6_addr_t *addr, char *str) {
    unsigned g[8];
    int i, best = -1, best_len = 0, cur = -1, cur_len = 0;
    int mapped;

    for (i = 0; i < 8; i++) {
        g[i] = (unsigned)(addr->Byte[i * 2] << 8) | addr->Byte[i * 2 + 1];
    }
    mapped = g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0 && g[4] == 0 && g[5] == 0xFFFF;
    for (i = 0; i < (mapped ? 6 : 8); i++) {
        if (g[i] == 0) {
            if (cur < 0) {
                cur = i;
                cur_len = 0;
            }
            cur_len++;
            if (cur_len > best_len) {
                best = cur;
                best_len = cur_len;
            }
        } else {
            cur = -1;
        }
    }
    if (best_len < 2) {
        best = -1;
    }
    /* A while loop: the longest run of zero groups is skipped in one step. */
    i = 0;
    while (i < (mapped ? 6 : 8)) {
        if (i == best) {
            *str++ = ':';
            *str++ = ':';
            i += best_len;
            continue;
        }
        if (i > 0 && !(best >= 0 && i == best + best_len)) {
            *str++ = ':';
        }
        str = put_hex(str, g[i]);
        i++;
    }
    if (mapped) {
        vibeos_in_addr_t v4;

        memcpy(v4.Byte, addr->Byte + 12, 4);
        *str++ = ':';
        return RtlIpv4AddressToStringA(&v4, str);
    }
    *str = 0;
    return str;
}
