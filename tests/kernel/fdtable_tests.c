/* Host tests for the descriptor table (C5). Each check names the defect it stands for. */

#include <stdio.h>
#include <string.h>

#include "vibeos/fdtable.h"

int test_fdtable(void);

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:fdtable %s\n", what);
    }
    return cond;
}

int test_fdtable(void) {
    vibeos_fdtable_t a, b;
    uint32_t i;

    /* A recycled slot holds the previous tenant's descriptors. */
    memset(&a, 0xA5, sizeof(a));
    vibeos_fdtable_reset(&a);
    for (i = 0; i < vibeos_fdtable_count(); i++) {
        vibeos_fd_t *f = vibeos_fdtable_entry(&a, i);
        if (!f || f->used != 0 || f->pipe != -1 || f->net_sock != -1 || f->pos != 0 || f->wlen != 0 ||
            f->name[0] != 0 || f->wbuf[0] != 0) {
            printf("FAIL:fdtable reset left entry %u in use or holding a stale pipe/socket/buffer\n", i);
            return -1;
        }
    }
    if (!expect(vibeos_fdtable_count() == VIBEOS_FD_SLOTS + VIBEOS_FD_STD,
                "count covers the table and the redirections")) { return -1; }
    if (!expect(vibeos_fdtable_entry(&a, vibeos_fdtable_count()) == NULL,
                "an index past the end is refused")) { return -1; }

    /* fds 0-2 are the console; 3+ are table entries, and only a used one is found. */
    if (!expect(vibeos_fdtable_get(&a, 3) == NULL, "an unused entry is not found")) { return -1; }
    a.fds[0].used = 1;
    a.fds[1].used = 1;
    if (!expect(vibeos_fdtable_get(&a, 3) == &a.fds[0], "fd 3 is table entry 0")) { return -1; }
    if (!expect(vibeos_fdtable_get(&a, 4) == &a.fds[1], "fd 4 is table entry 1")) { return -1; }
    if (!expect(vibeos_fdtable_get(&a, 2) == NULL, "fd 2 is the console, not a table entry")) { return -1; }
    /* In the real structure the entry after the last is the first redirection; make it
     * look in use, so an off-by-one bound finds it instead of an empty slot. */
    a.std[0].used = 1;
    if (!expect(vibeos_fdtable_get(&a, 3 + VIBEOS_FD_SLOTS) == NULL,
                "one past the last entry is refused")) { return -1; }
    a.std[0].used = 0;
    if (!expect(vibeos_fdtable_get(&a, ~0ull) == NULL, "a huge fd is refused, not wrapped")) { return -1; }
    if (!expect(vibeos_fdtable_get(NULL, 3) == NULL, "no table, no entry")) { return -1; }

    /* Redirections are separate: unused means the console. */
    if (!expect(vibeos_fdtable_redirect(&a, 1) == NULL, "an unredirected fd 1 is the console")) { return -1; }
    a.std[1].used = 1;
    if (!expect(vibeos_fdtable_redirect(&a, 1) == &a.std[1], "a redirected fd 1 is found")) { return -1; }
    if (!expect(vibeos_fdtable_redirect(&a, 3) == NULL, "redirect answers 0-2 only")) { return -1; }

    /* The lowest free index, and a full table. */
    if (!expect(vibeos_fdtable_free_index(&a) == 2, "the lowest free index skips the used ones")) { return -1; }
    a.fds[0].used = 0;
    if (!expect(vibeos_fdtable_free_index(&a) == 0, "a closed entry is reused first")) { return -1; }
    for (i = 0; i < VIBEOS_FD_SLOTS; i++) {
        a.fds[i].used = 1;
    }
    if (!expect(vibeos_fdtable_free_index(&a) == -1, "a full table reports -1, not slot 0")) { return -1; }

    /* Claiming hands out a cleared, used entry and reports a full table. */
    for (i = 0; i < VIBEOS_FD_SLOTS; i++) {
        a.fds[i].used = 0;
    }
    a.fds[0].pos = 9;
    a.fds[0].pipe = 5;
    if (!expect(vibeos_fdtable_claim(&a) == 0, "claim returns the lowest free index")) { return -1; }
    if (!expect(a.fds[0].used == 1 && a.fds[0].pos == 0 && a.fds[0].pipe == -1 && a.fds[0].net_sock == -1,
                "a claimed entry is cleared, not the previous holder's")) { return -1; }
    if (!expect(vibeos_fdtable_claim(&a) == 1, "the next claim takes the next entry")) { return -1; }
    for (i = 0; i < VIBEOS_FD_SLOTS; i++) {
        a.fds[i].used = 1;
    }
    if (!expect(vibeos_fdtable_claim(&a) == -1, "claiming from a full table fails")) { return -1; }

    /* fork: the child's table is replaced, not merged with the previous tenant's. */
    memset(&b, 0x5A, sizeof(b));
    a.fds[2].pos = 77;
    a.std[2].used = 1;
    a.std[2].pipe = 3;
    vibeos_fdtable_copy(&b, &a);
    if (!expect(memcmp(&a, &b, sizeof(a)) == 0,
                "a copy is byte-identical, including over what the child's slot held before")) { return -1; }
    b.fds[2].pos = 1;
    if (!expect(a.fds[2].pos == 77, "the copy is independent of its source")) { return -1; }
    return 0;
}
