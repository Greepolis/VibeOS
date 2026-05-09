#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    size_t i;

    if (!d || !s || n == 0 || d == s) {
        return dest;
    }

    if (d < s) {
        for (i = 0; i < n; i++) {
            d[i] = s[i];
        }
        return dest;
    }

    for (i = n; i > 0; i--) {
        d[i - 1] = s[i - 1];
    }
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    return memcpy(dest, src, n);
}

void *memset(void *dest, int c, size_t n) {
    unsigned char *d = (unsigned char *)dest;
    unsigned char v = (unsigned char)c;
    size_t i;

    if (!d || n == 0) {
        return dest;
    }

    for (i = 0; i < n; i++) {
        d[i] = v;
    }
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    size_t i;

    if (n == 0 || x == y) {
        return 0;
    }

    for (i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (int)x[i] - (int)y[i];
        }
    }
    return 0;
}
