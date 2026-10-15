/* memcpy and friends for a DLL linked -nostdlib (docs/abi/windows.md W2).
 *
 * The compiler emits calls to these for struct copies and for loops it
 * recognises as a copy, and there is no C runtime in ntdll to supply them.
 * Only the ntdll build compiles this file; the host test uses the C library's.
 *
 * The recognition is switched off for this file: a loop that is a memset,
 * compiled into a call to memset, is a memset that calls itself. */

#include <stddef.h>

#pragma GCC optimize("no-tree-loop-distribute-patterns")

void *memset(void *d, int c, size_t n) {
    unsigned char *p = (unsigned char *)d;

    while (n--) {
        *p++ = (unsigned char)c;
    }
    return d;
}

void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;

    while (n--) {
        *p++ = *q++;
    }
    return d;
}

void *memmove(void *d, const void *s, size_t n) {
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;

    if (p < q) {
        while (n--) {
            *p++ = *q++;
        }
    } else {
        p += n;
        q += n;
        while (n--) {
            *--p = *--q;
        }
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    for (; n; n--, p++, q++) {
        if (*p != *q) {
            return *p - *q;
        }
    }
    return 0;
}
