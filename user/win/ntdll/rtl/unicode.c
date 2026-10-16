/* Code pages and case (docs/abi/windows.md W2).
 *
 * The ANSI code page is Windows-1252 and nothing else is offered: DBCS lead bytes
 * do not exist in it, so one character is always one byte. Case mapping covers
 * what W2's callers and the Wine tests of these functions need, not Unicode: see
 * RtlUpcaseUnicodeChar. */

#include "rtl_internal.h"

/* 0x80-0x9F of Windows-1252. The five bytes it leaves undefined (81 8D 8F 90 9D)
 * map to the control character of the same value, as Windows maps them. */
static const vibeos_wchar_t g_cp1252_high[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

vibeos_wchar_t vibeos_rtl_ansi_to_wchar(unsigned char c) {
    if (c >= 0x80 && c <= 0x9F) {
        return g_cp1252_high[c - 0x80];
    }
    return c;                      /* 00-7F and A0-FF are Latin-1 as they stand */
}

unsigned char vibeos_rtl_upcase_ansi(unsigned char c) {
    if (c >= 'a' && c <= 'z') {
        return (unsigned char)(c - 0x20);
    }
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
        return (unsigned char)(c - 0x20);
    }
    switch (c) {
    case 0xFF: return 0x9F;        /* y diaeresis -> Y diaeresis, which Latin-1 lacks */
    case 0x9A: return 0x8A;        /* s caron */
    case 0x9C: return 0x8C;        /* oe ligature */
    case 0x9E: return 0x8E;        /* z caron */
    default:   return c;
    }
}

/* The subset W2 needs: ASCII, and Latin-1's lower-case letters (U+00E0-U+00FE
 * except the division sign U+00F7, each 0x20 down) with U+00FF going up to
 * U+0178. Windows also maps U+00B5, Greek, Cyrillic and the rest of Latin
 * Extended; those wait for a caller that needs them, and a string compared
 * case-insensitively here is therefore wrong for them. */
vibeos_wchar_t RtlUpcaseUnicodeChar(vibeos_wchar_t c) {
    if (c >= 'a' && c <= 'z') {
        return (vibeos_wchar_t)(c - 0x20);
    }
    if (c >= 0xE0 && c <= 0xFE && c != 0xF7) {
        return (vibeos_wchar_t)(c - 0x20);
    }
    if (c == 0xFF) {
        return 0x0178;
    }
    return c;
}

vibeos_wchar_t RtlAnsiCharToUnicodeChar(unsigned char **src) {
    vibeos_wchar_t w = vibeos_rtl_ansi_to_wchar(**src);

    (*src)++;
    return w;
}

vibeos_ntstatus_t RtlMultiByteToUnicodeN(vibeos_wchar_t *dest, uint32_t dest_size,
                                         uint32_t *result_size, const char *src,
                                         uint32_t src_size) {
    uint32_t i, n;

    if (!dest) {
        /* A query for the size the whole string needs. */
        if (result_size) {
            *result_size = src_size * 2u;
        }
        return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
    }
    /* The destination is counted in bytes and holds whole characters; what does
     * not fit is dropped and reported in ResultSize, not as a failure. */
    n = dest_size / 2u;
    if (n > src_size) {
        n = src_size;
    }
    for (i = 0; i < n; i++) {
        dest[i] = vibeos_rtl_ansi_to_wchar((unsigned char)src[i]);
    }
    if (result_size) {
        *result_size = n * 2u;
    }
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}
