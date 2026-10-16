/* STRING and UNICODE_STRING (docs/abi/windows.md W2), from Microsoft's description
 * of each function. Every length here is in BYTES, as Windows counts them, and a
 * UNICODE_STRING is not assumed to be terminated - except where a function says
 * it makes one that is. */

#include "rtl_internal.h"

static size_t cstrlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

void RtlInitString(vibeos_astring_t *dest, const char *src) {
    size_t n = src ? cstrlen(src) : 0;

    /* Length is a USHORT and MaximumLength counts the terminator, so the longest
     * string that keeps both is 0xFFFE characters. */
    if (n > 0xFFFEu) {
        n = 0xFFFEu;
    }
    dest->Length = (uint16_t)n;
    dest->MaximumLength = src ? (uint16_t)(n + 1u) : 0;
    dest->Buffer = (char *)(uintptr_t)src;
}

void RtlInitUnicodeString(vibeos_ustring_t *dest, const vibeos_wchar_t *src) {
    size_t n = src ? rtl_wcslen(src) * 2u : 0;

    /* The same arithmetic in bytes and in whole characters: Length stays even
     * and below MaximumLength by the terminator. */
    if (n > 0xFFFCu) {
        n = 0xFFFCu;
    }
    dest->Length = (uint16_t)n;
    dest->MaximumLength = src ? (uint16_t)(n + 2u) : 0;
    dest->Buffer = (vibeos_wchar_t *)(uintptr_t)src;
}

vibeos_boolean_t RtlCreateUnicodeString(vibeos_ustring_t *dest, const vibeos_wchar_t *src) {
    size_t bytes;
    vibeos_wchar_t *buf;

    /* Windows dereferences a NULL source. FALSE is the kinder reading of a
     * request there is no answer to. */
    if (!dest || !src) {
        return 0;
    }
    bytes = rtl_wcslen(src) * 2u;
    if (bytes > 0xFFFCu) {
        return 0;
    }
    buf = (vibeos_wchar_t *)vibeos_rtl_alloc(bytes + 2u);
    if (!buf) {
        return 0;
    }
    memcpy(buf, src, bytes);
    buf[bytes / 2u] = 0;
    dest->Length = (uint16_t)bytes;
    dest->MaximumLength = (uint16_t)(bytes + 2u);
    dest->Buffer = buf;
    return 1;
}

void RtlFreeUnicodeString(vibeos_ustring_t *str) {
    if (!str) {
        return;
    }
    if (str->Buffer) {
        vibeos_rtl_free(str->Buffer);
        memset(str, 0, sizeof(*str));
    }
}

vibeos_ntstatus_t RtlDuplicateUnicodeString(uint32_t flags, const vibeos_ustring_t *src,
                                            vibeos_ustring_t *dest) {
    uint32_t size;
    vibeos_wchar_t *buf;

    if ((flags & ~3u) != 0 || !src || !dest) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    if (src->Length == 0 && !(flags & VIBEOS_RTL_DUPLICATE_UNICODE_STRING_ALLOCATE_NULL_STRING)) {
        /* An empty source yields an empty destination without a buffer, unless
         * the caller asked for one. */
        dest->Length = 0;
        dest->MaximumLength = 0;
        dest->Buffer = 0;
        return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
    }
    size = src->Length;
    if (flags & VIBEOS_RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE) {
        size += 2u;
    }
    if (size > 0xFFFFu) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_NAME_TOO_LONG);
    }
    buf = (vibeos_wchar_t *)vibeos_rtl_alloc(size);
    if (!buf) {
        return VIBEOS_RTL_ST(VIBEOS_STATUS_NO_MEMORY);
    }
    if (src->Length) {
        memcpy(buf, src->Buffer, src->Length);
    }
    if (flags & VIBEOS_RTL_DUPLICATE_UNICODE_STRING_NULL_TERMINATE) {
        buf[src->Length / 2u] = 0;
    }
    dest->Length = src->Length;
    dest->MaximumLength = (uint16_t)size;
    dest->Buffer = buf;
    return VIBEOS_RTL_ST(VIBEOS_STATUS_SUCCESS);
}

int32_t RtlCompareString(const vibeos_astring_t *a, const vibeos_astring_t *b,
                         vibeos_boolean_t case_insensitive) {
    unsigned n = a->Length < b->Length ? a->Length : b->Length;
    unsigned i;

    for (i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a->Buffer[i];
        unsigned char y = (unsigned char)b->Buffer[i];

        if (case_insensitive) {
            x = vibeos_rtl_upcase_ansi(x);
            y = vibeos_rtl_upcase_ansi(y);
        }
        if (x != y) {
            return (int32_t)x - (int32_t)y;
        }
    }
    return (int32_t)a->Length - (int32_t)b->Length;
}

int32_t RtlCompareUnicodeString(const vibeos_ustring_t *a, const vibeos_ustring_t *b,
                                vibeos_boolean_t case_insensitive) {
    unsigned n = (a->Length < b->Length ? a->Length : b->Length) / 2u;
    unsigned i;

    for (i = 0; i < n; i++) {
        vibeos_wchar_t x = a->Buffer[i];
        vibeos_wchar_t y = b->Buffer[i];

        if (case_insensitive) {
            x = RtlUpcaseUnicodeChar(x);
            y = RtlUpcaseUnicodeChar(y);
        }
        if (x != y) {
            return (int32_t)x - (int32_t)y;
        }
    }
    /* The remainder is the difference in length, in bytes. */
    return (int32_t)a->Length - (int32_t)b->Length;
}

void RtlUpperString(vibeos_astring_t *dest, const vibeos_astring_t *src) {
    unsigned n = src->Length < dest->MaximumLength ? src->Length : dest->MaximumLength;
    unsigned i;

    for (i = 0; i < n; i++) {
        dest->Buffer[i] = (char)vibeos_rtl_upcase_ansi((unsigned char)src->Buffer[i]);
    }
    dest->Length = (uint16_t)n;
}
