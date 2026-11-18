/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The mount manifest's parser (hierarchy F2 step 3). Portable and
 * host-tested; the machine reads the file and applies what this returns. */

#include "vibeos/mounttab.h"

#include <string.h>

static int mt_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* Copies one field into dst (cap bytes with the terminator). Returns the
 * field's length, or -1 when it does not fit. */
static long mt_field(const char *s, long n, char *dst, uint32_t cap) {
    long k;

    if ((uint32_t)n + 1u > cap) {
        return -1;
    }
    for (k = 0; k < n; k++) {
        dst[k] = s[k];
    }
    dst[n] = 0;
    return n;
}

static int mt_is(const char *s, long n, const char *word) {
    long k;

    for (k = 0; k < n; k++) {
        if (word[k] != s[k]) {
            return 0;
        }
    }
    return word[n] == 0;
}

/* mode=0755 (octal, at most 07777) and size=<n>k / <n>m. */
static int mt_number_option(const char *s, long n, vibeos_mounttab_entry_t *e) {
    long k;
    uint64_t v = 0;

    if (n > 5 && memcmp(s, "mode=", 5) == 0) {
        for (k = 5; k < n; k++) {
            if (s[k] < '0' || s[k] > '7' || k > 9) {
                return -1;
            }
            v = v * 8u + (uint64_t)(s[k] - '0');
        }
        e->mode = (uint32_t)v;
        return 0;
    }
    if (n > 6 && memcmp(s, "size=", 5) == 0) {
        char unit = s[n - 1];
        for (k = 5; k < n - 1; k++) {
            if (s[k] < '0' || s[k] > '9' || k > 12) {
                return -1;
            }
            v = v * 10u + (uint64_t)(s[k] - '0');
        }
        if (unit == 'm' || unit == 'M') {
            v *= 1024u;
        } else if (unit != 'k' && unit != 'K') {
            return -1;
        }
        if (v == 0u || v > 0xFFFFFFFFu) {
            return -1;
        }
        e->size_kib = (uint32_t)v;
        return 0;
    }
    return -1;
}

static int mt_options(const char *s, long n, vibeos_mounttab_entry_t *e) {
    long a = 0, b;

    while (a < n) {
        for (b = a; b < n && s[b] != ','; b++) {
        }
        if (b == a) {
            return -1;   /* ",," or a trailing comma: a word lost, not nothing */
        }
        if (mt_is(s + a, b - a, "ro")) {
            e->options |= VIBEOS_MNT_RO;
        } else if (mt_is(s + a, b - a, "rw")) {
            e->options &= ~VIBEOS_MNT_RO;
        } else if (mt_is(s + a, b - a, "noexec")) {
            e->options |= VIBEOS_MNT_NOEXEC;
        } else if (mt_is(s + a, b - a, "exec")) {
            e->options &= ~VIBEOS_MNT_NOEXEC;
        } else if (mt_is(s + a, b - a, "nodev")) {
            e->options |= VIBEOS_MNT_NODEV;
        } else if (mt_is(s + a, b - a, "dev")) {
            e->options &= ~VIBEOS_MNT_NODEV;
        } else if (mt_is(s + a, b - a, "nosuid")) {
            e->options |= VIBEOS_MNT_NOSUID;
        } else if (mt_is(s + a, b - a, "suid")) {
            e->options &= ~VIBEOS_MNT_NOSUID;
        } else if (!mt_is(s + a, b - a, "defaults") && mt_number_option(s + a, b - a, e) != 0) {
            return -1;
        }
        a = b + 1;
        if (b == n - 1 && s[b] == ',') {
            return -1;
        }
    }
    return 0;
}

static int mt_line(const char *s, long n, vibeos_mounttab_t *out) {
    const char *f[6];
    long fl[6];
    uint32_t nf = 0, i;
    long k = 0;
    vibeos_mounttab_entry_t *e;

    while (k < n) {
        long a;
        while (k < n && mt_space(s[k])) {
            k++;
        }
        if (k >= n) {
            break;
        }
        a = k;
        while (k < n && !mt_space(s[k])) {
            k++;
        }
        if (nf == 6u) {
            return VIBEOS_MOUNTTAB_FIELDS;
        }
        f[nf] = s + a;
        fl[nf++] = k - a;
    }
    if (nf == 0u) {
        return VIBEOS_MOUNTTAB_OK;   /* blank or comment */
    }
    if (nf != 5u) {
        return VIBEOS_MOUNTTAB_FIELDS;
    }
    if (out->count >= VIBEOS_MOUNTTAB_MAX) {
        return VIBEOS_MOUNTTAB_TOO_MANY;
    }
    e = &out->entry[out->count];
    memset(e, 0, sizeof(*e));
    if (mt_field(f[0], fl[0], e->what, sizeof(e->what)) < 0 || mt_field(f[1], fl[1], e->at, sizeof(e->at)) < 0 ||
        mt_field(f[2], fl[2], e->type, sizeof(e->type)) < 0) {
        return VIBEOS_MOUNTTAB_TOO_LONG;
    }
    if ((fl[0] >= 5 && memcmp(e->what, "UUID=", 5) == 0) || (fl[0] >= 6 && memcmp(e->what, "LABEL=", 6) == 0)) {
        e->is_volume = 1;
        if (e->what[e->what[0] == 'U' ? 5 : 6] == 0) {
            return VIBEOS_MOUNTTAB_WHAT;
        }
    }
    /* Absolute, not the root, no empty component and no "." or "..": a
     * mount point is a place, and the attach table compares strings. */
    if (e->at[0] != '/' || e->at[1] == 0 || e->at[fl[1] - 1] == '/') {
        return VIBEOS_MOUNTTAB_PATH;
    }
    for (k = 0; k < fl[1]; k++) {
        if (e->at[k] == '/' && (e->at[k + 1] == '/' ||
                                (e->at[k + 1] == '.' && (e->at[k + 2] == '/' || e->at[k + 2] == 0 ||
                                                         (e->at[k + 2] == '.' &&
                                                          (e->at[k + 3] == '/' || e->at[k + 3] == 0)))))) {
            return VIBEOS_MOUNTTAB_PATH;
        }
    }
    for (i = 0; i < out->count; i++) {
        /* No strcmp in the freestanding image: mt_is compares fl[1] bytes and
         * the terminator. */
        if (mt_is(e->at, fl[1], out->entry[i].at)) {
            return VIBEOS_MOUNTTAB_DUPLICATE;
        }
    }
    if (mt_options(f[3], fl[3], e) != 0) {
        return VIBEOS_MOUNTTAB_OPTION;
    }
    if (mt_is(f[4], fl[4], "required")) {
        e->required = 1;
    } else if (!mt_is(f[4], fl[4], "optional")) {
        return VIBEOS_MOUNTTAB_POLICY;
    }
    out->count++;
    return VIBEOS_MOUNTTAB_OK;
}

int vibeos_mounttab_parse(const char *text, long len, vibeos_mounttab_t *out, uint32_t *bad_line) {
    long a = 0;
    uint32_t line = 0;
    int versioned = 0;

    memset(out, 0, sizeof(*out));
    *bad_line = 0;
    while (text && a < len) {
        long b, end;
        int why;

        for (b = a; b < len && text[b] != '\n'; b++) {
        }
        line++;
        for (end = a; end < b && text[end] != '#'; end++) {
        }
        if (!versioned) {
            long s = a, t;
            while (s < end && mt_space(text[s])) {
                s++;
            }
            for (t = end; t > s && mt_space(text[t - 1]); t--) {
            }
            if (t == s) {
                a = b + 1;
                continue;   /* comments and blank lines may come first */
            }
            if (!mt_is(text + s, t - s, "vibeos-mounts 1")) {
                *bad_line = line;
                memset(out, 0, sizeof(*out));
                return VIBEOS_MOUNTTAB_NO_VERSION;
            }
            versioned = 1;
        } else if ((why = mt_line(text + a, end - a, out)) != VIBEOS_MOUNTTAB_OK) {
            *bad_line = line;
            memset(out, 0, sizeof(*out));
            return why;
        }
        a = b + 1;
    }
    if (!versioned) {
        *bad_line = line;
        return VIBEOS_MOUNTTAB_NO_VERSION;
    }
    return VIBEOS_MOUNTTAB_OK;
}

const char *vibeos_mounttab_why(int why) {
    /* A switch, not a table of pointers: such a table is relocated data
     * (.data.rel.ro), which reads to nm as writable state with no lock. */
    switch (why) {
    case VIBEOS_MOUNTTAB_OK: return "ok";
    case VIBEOS_MOUNTTAB_NO_VERSION: return "no_version";
    case VIBEOS_MOUNTTAB_FIELDS: return "fields";
    case VIBEOS_MOUNTTAB_TOO_LONG: return "too_long";
    case VIBEOS_MOUNTTAB_PATH: return "path";
    case VIBEOS_MOUNTTAB_DUPLICATE: return "duplicate";
    case VIBEOS_MOUNTTAB_OPTION: return "option";
    case VIBEOS_MOUNTTAB_POLICY: return "policy";
    case VIBEOS_MOUNTTAB_TOO_MANY: return "too_many";
    case VIBEOS_MOUNTTAB_WHAT: return "what";
    default: return "?";
    }
}

/* What the machine mounted from code before F2, written as a manifest. The
 * ESP is "boot" here - the volume the machine started from - because a
 * default cannot know its serial; a root that ships a manifest names it by
 * UUID. Optional, because a bare ESP is its own root and has nothing to put
 * at /boot/efi. */
const char *vibeos_mounttab_default(void) {
    return "vibeos-mounts 1\n"
           "boot      /boot/efi  vfat      rw                          optional\n"
           "tmpfs     /tmp       tmpfs     nosuid,nodev,mode=1777      required\n"
           "proc      /proc      proc      nosuid,nodev,noexec         required\n"
           "devtmpfs  /dev       devtmpfs  nosuid                      required\n"
           "tmpfs     /run       tmpfs     nosuid,nodev,mode=0755,size=4m  required\n"
           "tmpfs     /dev/shm   tmpfs     nosuid,nodev,mode=1777      required\n";
}
