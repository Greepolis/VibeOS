/* Descriptor tables. See include/vibeos/fdtable.h. */

#include "vibeos/fdtable.h"

/* Where pages come from: registered once at boot, before any table exists, and
 * never changed - configuration rather than state, so no lock guards it. The
 * tables themselves are the caller's, under the process's files_lock. */
static void *(*g_page_alloc)(void);
static void (*g_page_free)(void *);

void vibeos_fdtable_set_pages(void *(*alloc)(void), void (*release)(void *)) {
    g_page_alloc = alloc;
    g_page_free = release;
}

void vibeos_fdtable_init(vibeos_fdtable_t *t) {
    uint32_t i;

    if (!t) {
        return;
    }
    for (i = 0; i < VIBEOS_FD_PAGES; i++) {
        t->page[i] = 0;
    }
    t->limit = VIBEOS_FD_MAX;
    t->open = 0;
}

static vibeos_fdent_t *slot(const vibeos_fdtable_t *t, uint64_t fd) {
    vibeos_fdent_t *p;

    if (!t || fd >= VIBEOS_FD_MAX) {
        return 0;
    }
    p = t->page[fd / VIBEOS_FD_PER_PAGE];
    return p ? &p[fd % VIBEOS_FD_PER_PAGE] : 0;
}

/* The slot for `fd`, with its page allocated if it was not. */
static vibeos_fdent_t *slot_make(vibeos_fdtable_t *t, uint64_t fd) {
    uint32_t pg = (uint32_t)(fd / VIBEOS_FD_PER_PAGE), i;
    vibeos_fdent_t *p;

    if (fd >= VIBEOS_FD_MAX) {
        return 0;
    }
    if (!t->page[pg]) {
        if (!g_page_alloc || (p = (vibeos_fdent_t *)g_page_alloc()) == 0) {
            return 0;
        }
        /* A page from the allocator holds whatever it held: the kernel's free
         * pages carry its poison pattern, and a table that read that as
         * descriptors would name descriptions that do not exist. */
        for (i = 0; i < VIBEOS_FD_PER_PAGE; i++) {
            p[i].file = 0;
            p[i].flags = 0;
            p[i].reserved = 0;
        }
        t->page[pg] = p;
    }
    return &t->page[pg][fd % VIBEOS_FD_PER_PAGE];
}

vibeos_file_t *vibeos_fdtable_get(const vibeos_fdtable_t *t, uint64_t fd) {
    vibeos_fdent_t *e = slot(t, fd);
    return e ? e->file : 0;
}

uint32_t vibeos_fdtable_flags(const vibeos_fdtable_t *t, uint64_t fd) {
    vibeos_fdent_t *e = slot(t, fd);
    return (e && e->file) ? e->flags : 0u;
}

int vibeos_fdtable_set_flags(vibeos_fdtable_t *t, uint64_t fd, uint32_t flags) {
    vibeos_fdent_t *e = slot(t, fd);
    if (!e || !e->file) {
        return VIBEOS_FDT_BADFD;
    }
    e->flags = flags;
    return 0;
}

int vibeos_fdtable_install(vibeos_fdtable_t *t, vibeos_file_t *f, uint32_t flags,
                           uint32_t min) {
    uint32_t fd;

    if (!t || !f) {
        return VIBEOS_FDT_FULL;
    }
    /* Lowest first, as POSIX requires: a shell closes 1 and opens a file to
     * redirect standard output, and the file had better be 1. */
    for (fd = min; fd < t->limit && fd < VIBEOS_FD_MAX; fd++) {
        vibeos_fdent_t *e = slot(t, fd);
        if (e && e->file) {
            continue;
        }
        e = slot_make(t, fd);
        if (!e) {
            return VIBEOS_FDT_NOMEM;
        }
        e->file = f;
        e->flags = flags;
        t->open++;
        return (int)fd;
    }
    return VIBEOS_FDT_FULL;
}

int vibeos_fdtable_install_at(vibeos_fdtable_t *t, uint64_t fd, vibeos_file_t *f,
                              uint32_t flags, vibeos_file_t **old) {
    vibeos_fdent_t *e;

    if (old) {
        *old = 0;
    }
    if (!t || !f || fd >= t->limit || fd >= VIBEOS_FD_MAX) {
        return VIBEOS_FDT_BADFD;
    }
    e = slot_make(t, fd);
    if (!e) {
        return VIBEOS_FDT_NOMEM;
    }
    if (e->file) {
        if (old) {
            *old = e->file;
        }
    } else {
        t->open++;
    }
    e->file = f;
    e->flags = flags;
    return 0;
}

vibeos_file_t *vibeos_fdtable_remove(vibeos_fdtable_t *t, uint64_t fd) {
    vibeos_fdent_t *e = slot(t, fd);
    vibeos_file_t *f;

    if (!e || !e->file) {
        return 0;
    }
    f = e->file;
    e->file = 0;
    e->flags = 0;
    t->open--;
    return f;
}

int vibeos_fdtable_copy(vibeos_fdtable_t *dst, const vibeos_fdtable_t *src) {
    uint32_t pg, i;

    if (!dst || !src) {
        return VIBEOS_FDT_NOMEM;
    }
    dst->limit = src->limit;
    for (pg = 0; pg < VIBEOS_FD_PAGES; pg++) {
        if (!src->page[pg]) {
            continue;
        }
        for (i = 0; i < VIBEOS_FD_PER_PAGE; i++) {
            const vibeos_fdent_t *s = &src->page[pg][i];
            vibeos_fdent_t *d;
            if (!s->file) {
                continue;
            }
            d = slot_make(dst, (uint64_t)pg * VIBEOS_FD_PER_PAGE + i);
            if (!d) {
                vibeos_fdtable_destroy(dst);
                return VIBEOS_FDT_NOMEM;
            }
            vibeos_file_get(s->file);
            d->file = s->file;
            d->flags = s->flags;
            dst->open++;
        }
    }
    return 0;
}

uint32_t vibeos_fdtable_drop_cloexec(vibeos_fdtable_t *t) {
    uint32_t pg, i, n = 0;

    if (!t) {
        return 0;
    }
    for (pg = 0; pg < VIBEOS_FD_PAGES; pg++) {
        if (!t->page[pg]) {
            continue;
        }
        for (i = 0; i < VIBEOS_FD_PER_PAGE; i++) {
            vibeos_fdent_t *e = &t->page[pg][i];
            if (e->file && (e->flags & VIBEOS_FD_CLOEXEC)) {
                vibeos_file_t *f = e->file;
                e->file = 0;
                e->flags = 0;
                t->open--;
                vibeos_file_put(f);
                n++;
            }
        }
    }
    return n;
}

void vibeos_fdtable_destroy(vibeos_fdtable_t *t) {
    uint32_t pg, i;

    if (!t) {
        return;
    }
    for (pg = 0; pg < VIBEOS_FD_PAGES; pg++) {
        vibeos_fdent_t *p = t->page[pg];
        if (!p) {
            continue;
        }
        for (i = 0; i < VIBEOS_FD_PER_PAGE; i++) {
            if (p[i].file) {
                vibeos_file_t *f = p[i].file;
                p[i].file = 0;
                vibeos_file_put(f);
            }
        }
        t->page[pg] = 0;
        if (g_page_free) {
            g_page_free(p);
        }
    }
    t->open = 0;
}

int vibeos_fdtable_highest(const vibeos_fdtable_t *t) {
    int pg, i;

    if (!t) {
        return -1;
    }
    for (pg = (int)VIBEOS_FD_PAGES - 1; pg >= 0; pg--) {
        if (!t->page[pg]) {
            continue;
        }
        for (i = (int)VIBEOS_FD_PER_PAGE - 1; i >= 0; i--) {
            if (t->page[pg][i].file) {
                return pg * (int)VIBEOS_FD_PER_PAGE + i;
            }
        }
    }
    return -1;
}
