/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The kernel services drm_torture needs, owned by the test rather than by the
 * kernel: file descriptions, user copies, locks and counted pages. The DRM
 * driver is compiled as it ships; this file is the smallest machine it can run
 * on, so the torture can start without the kernel's architecture layer. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/file.h"
#include "vibeos/ksvc.h"

#define SUPPORT_FILES 8u
#define SUPPORT_PAGES 2048u

static vibeos_file_t g_files[SUPPORT_FILES];

typedef struct {
    void *page;
    uint32_t refs;
} support_page_t;

static support_page_t g_pages[SUPPORT_PAGES];

static void support_die(const char *why, const void *where) {
    fprintf(stderr, "DRM_TORTURE_SUPPORT %s %p\n", why, where);
    abort();
}

static support_page_t *support_page_find(const void *page) {
    uint32_t i;

    for (i = 0; i < SUPPORT_PAGES; i++) {
        if (g_pages[i].page == page) {
            return &g_pages[i];
        }
    }
    return 0;
}

vibeos_file_t *vibeos_file_alloc(const vibeos_file_ops_t *ops, uint32_t flags) {
    uint32_t i;

    for (i = 0; i < SUPPORT_FILES; i++) {
        if (!g_files[i].ops) {
            memset(&g_files[i], 0, sizeof(g_files[i]));
            g_files[i].ops = ops;
            g_files[i].flags = flags;
            g_files[i].refs = 1u;
            return &g_files[i];
        }
    }
    return 0;
}

void vibeos_file_put(vibeos_file_t *f) {
    if (!f || !f->ops || f->refs != 1u) {
        support_die("bad file put", f);
    }
    f->refs = 0u;
    f->ops->release(f);
    f->ops = 0;
}

void vibeos_file_stat_clear(vibeos_file_stat_t *st) {
    memset(st, 0, sizeof(*st));
}

int vibeos_uaccess_copy(void *dst, const void *src, uint64_t len) {
    memmove(dst, src, (size_t)len);
    return 0;
}

void ks_lock(vibeos_lock_t *l, const char *name) {
    (void)l;
    (void)name;
}

void ks_unlock(vibeos_lock_t *l) {
    (void)l;
}

void *ks_page_alloc(void) {
    void *page = calloc(1, 4096u);
    uint32_t i;

    if (!page) {
        support_die("page allocation failed", 0);
    }
    for (i = 0; i < SUPPORT_PAGES; i++) {
        if (!g_pages[i].page) {
            g_pages[i].page = page;
            g_pages[i].refs = 1u;
            return page;
        }
    }
    support_die("page table full", page);
    return 0;
}

void ks_page_hold(void *page) {
    support_page_t *p = support_page_find(page);

    if (!p || p->refs == 0u) {
        support_die("holding an unknown page", page);
    }
    p->refs++;
}

void ks_page_unhold(void *page) {
    support_page_t *p = support_page_find(page);

    if (!p || p->refs == 0u) {
        support_die("releasing an unknown page", page);
    }
    if (--p->refs == 0u) {
        free(p->page);
        p->page = 0;
    }
}

void ks_page_free(void *page, const char *why) {
    (void)why;
    ks_page_unhold(page);
}

__attribute__((destructor)) static void support_no_page_leaks(void) {
    uint32_t i;

    for (i = 0; i < SUPPORT_PAGES; i++) {
        if (g_pages[i].page) {
            support_die("a page outlived the test", g_pages[i].page);
        }
    }
}
