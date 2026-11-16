/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Case-insensitive names as Linux's utf8-12.1 has them (E4-030). See
 * vibeos/utf8fold.h. */

#include "vibeos/utf8fold.h"

#include "utf8fold_data.h"

#define N(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* Hangul syllables decompose by arithmetic (Unicode section 3.12). */
#define SBASE 0xAC00u
#define LBASE 0x1100u
#define VBASE 0x1161u
#define TBASE 0x11A7u
#define TCOUNT 28u
#define NCOUNT 588u
#define SCOUNT 11172u

/* A segment - a starter and the marks after it, which canonical order sorts
 * among themselves - longer than this is not folded: the name is then compared
 * as bytes. No script needs more than a handful of marks on one letter. */
#define SEG_MAX 32u

static int assigned(uint32_t cp) {
    uint32_t lo = 0, hi = N(g_u8f_assigned);

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;

        if (cp < g_u8f_assigned[mid][0]) {
            hi = mid;
        } else if (cp > g_u8f_assigned[mid][1]) {
            lo = mid + 1u;
        } else {
            return 1;
        }
    }
    return 0;
}

static uint32_t ccc_of(uint32_t cp) {
    uint32_t lo = 0, hi = N(g_u8f_ccc);

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;

        if (cp < g_u8f_ccc[mid][0]) {
            hi = mid;
        } else if (cp > g_u8f_ccc[mid][0]) {
            lo = mid + 1u;
        } else {
            return g_u8f_ccc[mid][1];
        }
    }
    return 0;
}

static const uint32_t *map_of(uint32_t cp, uint32_t *n) {
    uint32_t lo = 0, hi = N(g_u8f_map);

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;

        if (cp < g_u8f_map[mid][0]) {
            hi = mid;
        } else if (cp > g_u8f_map[mid][0]) {
            lo = mid + 1u;
        } else {
            *n = g_u8f_map[mid][2];
            return &g_u8f_pool[g_u8f_map[mid][1]];
        }
    }
    return 0;
}

/* One code point from UTF-8 at *i: as strict as Linux's utf8 reader - no
 * overlong forms, no surrogates, nothing past U+10FFFF. -1 for invalid. */
static int64_t decode(const uint8_t *s, uint32_t len, uint32_t *i) {
    uint32_t c = s[*i], need, cp, k;

    if (c < 0x80u) {
        (*i)++;
        return c;
    }
    if ((c & 0xE0u) == 0xC0u) {
        need = 1;
        cp = c & 0x1Fu;
    } else if ((c & 0xF0u) == 0xE0u) {
        need = 2;
        cp = c & 0x0Fu;
    } else if ((c & 0xF8u) == 0xF0u) {
        need = 3;
        cp = c & 0x07u;
    } else {
        return -1;
    }
    if (*i + need >= len) {
        return -1;   /* the sequence runs past the name */
    }
    for (k = 1; k <= need; k++) {
        if ((s[*i + k] & 0xC0u) != 0x80u) {
            return -1;
        }
        cp = (cp << 6) | (s[*i + k] & 0x3Fu);
    }
    if ((need == 1u && cp < 0x80u) || (need == 2u && cp < 0x800u) || (need == 3u && cp < 0x10000u) ||
        cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
        return -1;
    }
    *i += need + 1u;
    return cp;
}

static uint32_t encode(uint32_t cp, uint8_t *o) {
    if (cp < 0x80u) {
        o[0] = (uint8_t)cp;
        return 1;
    }
    if (cp < 0x800u) {
        o[0] = (uint8_t)(0xC0u | (cp >> 6));
        o[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        o[0] = (uint8_t)(0xE0u | (cp >> 12));
        o[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        o[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    o[0] = (uint8_t)(0xF0u | (cp >> 18));
    o[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
    o[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
    o[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* A name read as its folded code points, one segment at a time, so that
 * neither side of a comparison needs the whole folded name at once. */
typedef struct {
    const uint8_t *s;
    uint32_t len, i;
    uint32_t q[8], qh, qn;        /* code points decoded and expanded, not yet in a segment */
    uint32_t seg[SEG_MAX], sn, sp;
    int bad;
} fold_it_t;

/* Refill the queue with the next character's expansion: 0, 1 at the end, -1. */
static int fill(fold_it_t *t) {
    int64_t cp;
    const uint32_t *m;
    uint32_t mn = 0, k;

    if (t->i >= t->len) {
        return 1;
    }
    cp = decode(t->s, t->len, &t->i);
    if (cp < 0 || !assigned((uint32_t)cp)) {
        return -1;
    }
    t->qh = 0;
    t->qn = 0;
    if ((uint32_t)cp >= SBASE && (uint32_t)cp < SBASE + SCOUNT) {
        uint32_t si = (uint32_t)cp - SBASE;

        t->q[t->qn++] = LBASE + si / NCOUNT;
        t->q[t->qn++] = VBASE + (si % NCOUNT) / TCOUNT;
        if (si % TCOUNT != 0u) {
            t->q[t->qn++] = TBASE + si % TCOUNT;
        }
        return 0;
    }
    m = map_of((uint32_t)cp, &mn);
    if (m && mn > 0u) {   /* no expansion is empty; were one, the point would stand for itself */
        for (k = 0; k < mn; k++) {
            t->q[t->qn++] = m[k];
        }
    } else {
        t->q[t->qn++] = (uint32_t)cp;
    }
    return 0;
}

/* The next folded code point: 1 and *cp, 0 at the end, -1 when the name does
 * not fold. */
static int fold_next(fold_it_t *t, uint32_t *cp) {
    uint32_t i, j, k;

    if (t->bad) {
        return -1;
    }
    if (t->sp < t->sn) {
        *cp = t->seg[t->sp++];
        return 1;
    }
    t->sn = 0;
    t->sp = 0;
    for (;;) {
        if (t->qh == t->qn) {
            int r = fill(t);

            if (r < 0) {
                t->bad = 1;
                return -1;
            }
            if (r > 0) {
                break;
            }
        }
        /* A starter after anything ends the segment: it stays queued. */
        if (t->sn > 0u && ccc_of(t->q[t->qh]) == 0u) {
            break;
        }
        if (t->sn == SEG_MAX) {
            t->bad = 1;
            return -1;
        }
        t->seg[t->sn++] = t->q[t->qh++];
    }
    if (t->sn == 0u) {
        return 0;
    }
    /* Canonical order: the marks of the segment, stably by class. */
    for (i = 1; i < t->sn; i++) {
        uint32_t c = ccc_of(t->seg[i]);

        for (j = i; j > 0u && c != 0u; j--) {
            uint32_t p = ccc_of(t->seg[j - 1u]);

            if (p == 0u || p <= c) {
                break;
            }
            k = t->seg[j];
            t->seg[j] = t->seg[j - 1u];
            t->seg[j - 1u] = k;
        }
    }
    *cp = t->seg[t->sp++];
    return 1;
}

static void fold_start(fold_it_t *t, const char *s, uint32_t len) {
    t->s = (const uint8_t *)s;
    t->len = len;
    t->i = 0;
    t->qh = t->qn = 0;
    t->sn = t->sp = 0;
    t->bad = 0;
}

int32_t vibeos_utf8_fold(const char *in, uint32_t len, uint8_t *out, uint32_t cap) {
    fold_it_t t;
    uint32_t cp, o = 0, k;
    int r;

    fold_start(&t, in, len);
    while ((r = fold_next(&t, &cp)) > 0) {
        uint8_t b[4];
        uint32_t w = encode(cp, b);

        if (o + w > cap) {
            return -1;
        }
        for (k = 0; k < w; k++) {
            out[o++] = b[k];
        }
    }
    return r < 0 ? -1 : (int32_t)o;
}

int vibeos_utf8_fold_eq(const char *a, uint32_t alen, const char *b, uint32_t blen) {
    fold_it_t x, y;
    uint32_t i, ca, cb;

    fold_start(&x, a, alen);
    fold_start(&y, b, blen);
    for (;;) {
        int ra = fold_next(&x, &ca), rb = fold_next(&y, &cb);

        if (ra < 0 || rb < 0) {
            break;   /* one does not fold: the bytes decide */
        }
        if (ra == 0 || rb == 0) {
            /* Equal only if both ended here - unless the longer one would
             * still turn out not to fold. */
            if (ra == rb) {
                return 1;
            }
            while ((ra = fold_next(&x, &ca)) > 0) {
            }
            while ((rb = fold_next(&y, &cb)) > 0) {
            }
            if (ra < 0 || rb < 0) {
                break;
            }
            return 0;
        }
        if (ca != cb) {
            /* Different - unless the rest of either does not fold. */
            while ((ra = fold_next(&x, &ca)) > 0) {
            }
            while ((rb = fold_next(&y, &cb)) > 0) {
            }
            if (ra < 0 || rb < 0) {
                break;
            }
            return 0;
        }
    }
    if (alen != blen) {
        return 0;
    }
    for (i = 0; i < alen && a[i] == b[i]; i++) {
    }
    return i == alen;
}
