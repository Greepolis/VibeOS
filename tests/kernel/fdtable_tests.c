/* Host tests for descriptors and open file descriptions (C5; rebuilt for
 * docs/abi/ A3). Each check names the defect it stands for. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/fdtable.h"
#include "vibeos/file.h"
#include "vibeos/mbz.h"

int test_fdtable(void);

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:fdtable %s\n", what);
    }
    return cond;
}

/* Pages that start full of garbage, as the kernel's do (its free pages carry a
 * poison pattern), and that can be refused on demand. */
static int g_pages, g_pages_freed, g_refuse_pages;
static void *t_page(void) {
    void *p;
    if (g_refuse_pages) {
        return 0;
    }
    p = malloc(4096);
    if (p) {
        memset(p, 0xA5, 4096);
        g_pages++;
    }
    return p;
}
static void t_page_free(void *p) {
    g_pages_freed++;
    free(p);
}

static int g_released;
static vibeos_file_t *g_during_release;
static void t_release(vibeos_file_t *f) {
    (void)f;
    g_released++;
}
static const vibeos_file_ops_t g_ops = { "test", 0, 0, 0, 0, 0, 0, t_release };

/* A release that allocates: the slot being released must not be handed out. */
static void t_release_allocs(vibeos_file_t *f) {
    (void)f;
    g_during_release = vibeos_file_alloc(&g_ops, 0);
}
static const vibeos_file_ops_t g_ops_allocs = { "allocs", 0, 0, 0, 0, 0, 0, t_release_allocs };

int test_fdtable(void) {
    static vibeos_fdtable_t a, b;
    vibeos_file_t *f, *g, *old;
    uint64_t base_under;
    uint32_t i;
    int fd, n;

    vibeos_file_reset();
    vibeos_fdtable_set_pages(t_page, t_page_free);
    g_pages = g_pages_freed = g_refuse_pages = g_released = 0;
    base_under = vibeos_mbz_count(VIBEOS_MBZ_FILE_PUT_UNDERFLOW);

    /* ---- a description is counted, and released once ------------------------- */
    f = vibeos_file_alloc(&g_ops, VIBEOS_O_RDWR | VIBEOS_O_CLOEXEC);
    if (!expect(f && f->refs == 1u && f->pipe == -1 && f->sock == -1,
                "a new description has one reference and no pipe or socket")) { return -1; }
    if (!expect(f->flags == VIBEOS_O_RDWR, "O_CLOEXEC is the descriptor's, not kept here")) { return -1; }
    vibeos_file_get(f);
    vibeos_file_put(f);
    if (!expect(g_released == 0, "a put that leaves a reference does not release")) { return -1; }
    vibeos_file_put(f);
    if (!expect(g_released == 1 && vibeos_file_in_use() == 0u, "the last put releases it")) { return -1; }
    vibeos_file_put(f);
    if (!expect(g_released == 1 &&
                vibeos_mbz_count(VIBEOS_MBZ_FILE_PUT_UNDERFLOW) == base_under + 1u,
                "a put with nothing left is counted, and releases nothing a second time")) { return -1; }

    f = vibeos_file_alloc(&g_ops_allocs, 0);
    vibeos_file_put(f);
    if (!expect(g_during_release && g_during_release != f,
                "a slot being released is not handed out by an alloc inside the release")) { return -1; }
    vibeos_file_put(g_during_release);

    /* ---- the table: empty, lowest first, grown by pages ------------------------- */
    vibeos_fdtable_init(&a);
    if (!expect(g_pages == 0 && vibeos_fdtable_get(&a, 0) == NULL && vibeos_fdtable_highest(&a) == -1,
                "an empty table owns no page and names nothing")) { return -1; }
    f = vibeos_file_alloc(&g_ops, 0);
    fd = vibeos_fdtable_install(&a, f, 0, 0);
    if (!expect(fd == 0 && vibeos_fdtable_get(&a, 0) == f && g_pages == 1,
                "the first descriptor is 0, in a page allocated for it")) { return -1; }
    for (i = 1; i < 200u; i++) {
        vibeos_file_get(f);
        if (vibeos_fdtable_install(&a, f, 0, 0) != (int)i) {
            printf("FAIL:fdtable descriptor %u was not the next free one\n", i);
            return -1;
        }
    }
    if (!expect(a.open == 200u && g_pages == 1, "two hundred descriptors, one page")) { return -1; }
    if (!expect(vibeos_fdtable_get(&a, 1) == f && vibeos_fdtable_get(&a, 200) == NULL,
                "the page was cleared, not read as descriptors (its bytes were poison)")) { return -1; }
    old = vibeos_fdtable_remove(&a, 1);
    if (!expect(old == f && vibeos_fdtable_get(&a, 1) == NULL, "remove hands the description back")) { return -1; }
    if (!expect(vibeos_fdtable_install(&a, old, 0, 0) == 1,
                "a closed number is the next one handed out - a shell's redirection relies on it")) { return -1; }
    vibeos_file_get(f);
    if (!expect(vibeos_fdtable_install(&a, f, 0, 150) == 200,
                "install from a minimum finds the first free at or above it")) { return -1; }
    vibeos_file_get(f);
    if (!expect(vibeos_fdtable_install_at(&a, 300, f, 0, &old) == 0 && old == NULL && g_pages == 2,
                "a descriptor in the second page allocates that page")) { return -1; }
    if (!expect(vibeos_fdtable_install_at(&a, VIBEOS_FD_MAX, f, 0, &old) == VIBEOS_FDT_BADFD,
                "a descriptor past the table is refused")) { return -1; }
    a.limit = 301;
    vibeos_file_get(f);
    if (!expect(vibeos_fdtable_install(&a, f, 0, 300) == VIBEOS_FDT_FULL,
                "no free descriptor below the limit is EMFILE")) { return -1; }
    vibeos_file_put(f);
    a.limit = VIBEOS_FD_MAX;
    g_refuse_pages = 1;
    vibeos_file_get(f);
    if (!expect(vibeos_fdtable_install(&a, f, 0, 600) == VIBEOS_FDT_NOMEM && f->refs == 203u,
                "no page is ENOMEM, and the reference stays the caller's")) { return -1; }
    vibeos_file_put(f);
    g_refuse_pages = 0;

    /* ---- dup shares an offset --------------------------------------------------- */
    g = vibeos_file_alloc(&g_ops, 0);
    fd = vibeos_fdtable_install(&a, g, 0, 0);
    vibeos_file_get(g);
    if (!expect(vibeos_fdtable_install_at(&a, 250, g, 0, &old) == 0, "dup2 onto a free number")) { return -1; }
    vibeos_fdtable_get(&a, (uint64_t)fd)->pos = 42;
    if (!expect(vibeos_fdtable_get(&a, 250)->pos == 42,
                "two descriptors for one description share its offset")) { return -1; }
    vibeos_file_get(g);
    if (!expect(vibeos_fdtable_install_at(&a, 250, g, 0, &old) == 0 && old == g,
                "dup2 onto a taken number hands back what it replaced")) { return -1; }
    vibeos_file_put(old);

    /* ---- fork copies references; cloexec is the descriptor's -------------------- */
    vibeos_fdtable_init(&b);
    n = (int)g->refs;
    (void)vibeos_fdtable_set_flags(&a, 250, VIBEOS_FD_CLOEXEC);
    if (!expect(vibeos_fdtable_copy(&b, &a) == 0 && b.open == a.open && g->refs == (uint32_t)n * 2u,
                "a copied table holds a reference to every description")) { return -1; }
    if (!expect(vibeos_fdtable_flags(&b, 250) == VIBEOS_FD_CLOEXEC, "and keeps the flags")) { return -1; }
    if (!expect(vibeos_fdtable_drop_cloexec(&b) == 1u && vibeos_fdtable_get(&b, 250) == NULL &&
                vibeos_fdtable_get(&a, 250) == g, "exec closes only its own close-on-exec numbers")) { return -1; }
    g_released = 0;
    vibeos_fdtable_destroy(&b);
    if (!expect(g_released == 0 && b.open == 0u, "the parent still holds every description")) { return -1; }
    vibeos_fdtable_destroy(&a);
    if (!expect(g_released == 2 && vibeos_file_in_use() == 0u && g_pages_freed == g_pages,
                "the last table releases each description once, and gives its pages back")) { return -1; }
    if (!expect(vibeos_mbz_count(VIBEOS_MBZ_FILE_PUT_UNDERFLOW) == base_under + 1u,
                "no put underflowed along the way")) { return -1; }

    g_refuse_pages = 1;
    vibeos_fdtable_init(&a);
    vibeos_fdtable_init(&b);
    f = vibeos_file_alloc(&g_ops, 0);
    g_refuse_pages = 0;
    (void)vibeos_fdtable_install(&a, f, 0, 0);
    g_refuse_pages = 1;
    if (!expect(vibeos_fdtable_copy(&b, &a) == VIBEOS_FDT_NOMEM && b.open == 0u && f->refs == 1u,
                "a copy that cannot get a page leaves the child empty and takes no reference")) { return -1; }
    g_refuse_pages = 0;
    vibeos_fdtable_destroy(&a);
    return 0;
}
