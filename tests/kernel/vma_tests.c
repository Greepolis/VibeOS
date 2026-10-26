/* Host tests for the region layer (plan phase P3).
 *
 * A pure data structure, so this is where it gets exercised: every case below
 * runs in microseconds and none of them needs a virtual machine. The cases that
 * matter are the ones a page-table walk could not represent - a partial unmap
 * that has to leave two regions where there was one - and the refusals.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/vma.h"
#include "vibeos/mm_stats.h"

int test_vma(void);

#define POOL 32u
#define PAGE 4096ull
#define RW   ((vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_WRITE | VIBEOS_PROT_USER))
#define RO   ((vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_USER))

static vibeos_vma_t g_pool[POOL];

static void setup(void) {
    memset(g_pool, 0, sizeof(g_pool));
    vibeos_mm_stats_reset();
    vibeos_vma_pool_init(g_pool, POOL);
}

static int anon(vibeos_vma_list_t *l, uint64_t base, uint64_t len,
                vibeos_prot_t prot) {
    return vibeos_vma_insert(l, base, len, prot, VIBEOS_BACKING_ANON, 0, 0);
}

/* How many regions the list holds, counted by walking rather than trusted from
 * the field - the field is what a bug would corrupt. */
static uint32_t walk_count(vibeos_vma_list_t *l) {
    uint32_t n = 0;
    vibeos_vma_t *c;
    for (c = l->head; c; c = c->next) {
        n++;
    }
    return n;
}

/* Sorted and non-overlapping, checked after every change. This is the whole
 * invariant, and asserting it repeatedly is cheaper than reasoning about which
 * operation could break it. */
static int ordered(vibeos_vma_list_t *l) {
    vibeos_vma_t *c;
    uint64_t last_end = 0;
    for (c = l->head; c; c = c->next) {
        if (c->len == 0u || c->base < last_end) {
            return 0;
        }
        last_end = c->base + c->len;
    }
    return 1;
}

int test_vma(void) {
    vibeos_vma_list_t l;
    vibeos_vma_list_t child;

    /* ---- 1. insert, find, and the addresses either side ------------------ */
    setup();
    memset(&l, 0, sizeof(l));
    if (anon(&l, 0x10000ull, 2ull * PAGE, RW) != 0) { goto fail; }
    if (!vibeos_vma_find(&l, 0x10000ull)) { goto fail; }
    if (!vibeos_vma_find(&l, 0x10000ull + PAGE)) { goto fail; }
    if (vibeos_vma_find(&l, 0x10000ull - 1ull)) { goto fail; }
    if (vibeos_vma_find(&l, 0x10000ull + 2ull * PAGE)) { goto fail; }
    if (!ordered(&l)) { goto fail; }

    /* ---- 2. an overlapping insert is refused, and changes nothing -------- */
    if (anon(&l, 0x10000ull + PAGE, 2ull * PAGE, RW) == 0) {
        printf("FAIL:vma accepted an overlapping insert\n");
        goto fail;
    }
    if (walk_count(&l) != 1u) { goto fail; }
    if (vibeos_vma_live() != 1u) {
        printf("FAIL:vma leaked a descriptor on a refused insert\n");
        goto fail;
    }

    /* ---- 3. adjacent and alike regions merge ----------------------------- *
     *
     * A program that grows its heap one page at a time would otherwise end
     * with a thousand descriptors describing one range. */
    if (anon(&l, 0x12000ull, PAGE, RW) != 0) { goto fail; }
    if (walk_count(&l) != 1u) {
        printf("FAIL:vma kept two descriptors for one contiguous range\n");
        goto fail;
    }
    if (l.head->len != 3ull * PAGE) { goto fail; }

    /* ---- 4. adjacent but different regions do not merge ------------------ */
    if (anon(&l, 0x13000ull, PAGE, RO) != 0) { goto fail; }
    if (walk_count(&l) != 2u) {
        printf("FAIL:vma merged regions with different protection\n");
        goto fail;
    }

    /* ---- 5. unmapping the middle leaves two ------------------------------ *
     *
     * The case a page-table walk had no way to represent, and the reason this
     * layer exists. */
    setup();
    memset(&l, 0, sizeof(l));
    if (anon(&l, 0x20000ull, 4ull * PAGE, RW) != 0) { goto fail; }
    if (vibeos_vma_remove(&l, 0x21000ull, PAGE) != PAGE) { goto fail; }
    if (walk_count(&l) != 2u) {
        printf("FAIL:vma did not split on a partial unmap\n");
        goto fail;
    }
    if (l.head->base != 0x20000ull || l.head->len != PAGE) { goto fail; }
    if (l.head->next->base != 0x22000ull || l.head->next->len != 2ull * PAGE) { goto fail; }
    if (!ordered(&l)) { goto fail; }

    /* ---- 6. trimming the head and the tail ------------------------------- */
    setup();
    memset(&l, 0, sizeof(l));
    if (anon(&l, 0x30000ull, 4ull * PAGE, RW) != 0) { goto fail; }
    if (vibeos_vma_remove(&l, 0x30000ull, PAGE) != PAGE) { goto fail; }
    if (l.head->base != 0x31000ull || l.head->len != 3ull * PAGE) { goto fail; }
    if (vibeos_vma_remove(&l, 0x33000ull, PAGE) != PAGE) { goto fail; }
    if (l.head->base != 0x31000ull || l.head->len != 2ull * PAGE) { goto fail; }
    if (walk_count(&l) != 1u) { goto fail; }

    /* ---- 7. removing a range with holes reports what it actually removed - */
    setup();
    memset(&l, 0, sizeof(l));
    if (anon(&l, 0x40000ull, PAGE, RW) != 0) { goto fail; }
    if (anon(&l, 0x42000ull, PAGE, RW) != 0) { goto fail; }
    if (vibeos_vma_remove(&l, 0x40000ull, 3ull * PAGE) != 2ull * PAGE) {
        printf("FAIL:vma reported the range it was asked for, not the one it removed\n");
        goto fail;
    }
    if (walk_count(&l) != 0u) { goto fail; }
    if (vibeos_vma_live() != 0u) { goto fail; }

    /* ---- 8. protect splits, and refuses a range with a hole -------------- */
    setup();
    memset(&l, 0, sizeof(l));
    if (anon(&l, 0x50000ull, 4ull * PAGE, RW) != 0) { goto fail; }
    if (vibeos_vma_protect(&l, 0x51000ull, PAGE, RO) != 0) { goto fail; }
    if (walk_count(&l) != 3u) {
        printf("FAIL:vma protect did not split around the range\n");
        goto fail;
    }
    if (!vibeos_vma_find(&l, 0x51000ull) ||
        vibeos_vma_find(&l, 0x51000ull)->prot != RO) { goto fail; }
    if (vibeos_vma_find(&l, 0x50000ull)->prot != RW) { goto fail; }
    if (!ordered(&l)) { goto fail; }
    /* A range that is not entirely mapped is refused, and the refusal leaves
     * the list exactly as it was - a partial application is a state the caller
     * never asked for and cannot discover. */
    {
        uint32_t before = walk_count(&l);
        if (vibeos_vma_protect(&l, 0x53000ull, 4ull * PAGE, RO) == 0) {
            printf("FAIL:vma protected a range containing unmapped pages\n");
            goto fail;
        }
        if (walk_count(&l) != before) {
            printf("FAIL:vma changed the list on a refused protect\n");
            goto fail;
        }
    }
    /* Protecting back to what the neighbours are folds them together again. */
    if (vibeos_vma_protect(&l, 0x51000ull, PAGE, RW) != 0) { goto fail; }
    if (walk_count(&l) != 1u) {
        printf("FAIL:vma did not merge after protect made the regions alike\n");
        goto fail;
    }

    /* One change across five regions that it makes alike: each merge frees
     * a descriptor, the successor among them, and the walk has to continue
     * from what survived rather than from a pointer read before the merge. */
    setup();
    memset(&l, 0, sizeof(l));
    if (anon(&l, 0x70000ull, PAGE, RW) != 0 || anon(&l, 0x71000ull, PAGE, RO) != 0 ||
        anon(&l, 0x72000ull, PAGE, RW) != 0 || anon(&l, 0x73000ull, PAGE, RO) != 0 ||
        anon(&l, 0x74000ull, PAGE, RW) != 0 || walk_count(&l) != 5u) {
        goto fail;
    }
    if (vibeos_vma_protect(&l, 0x70000ull, 5ull * PAGE, RW) != 0 || walk_count(&l) != 1u ||
        vibeos_vma_live() != 1u || !ordered(&l) || l.head->len != 5ull * PAGE) {
        printf("FAIL:vma a change that merges several regions walked a freed one\n");
        goto fail;
    }

    /* M-127: one address space holds no more than a quarter of the pool, and
     * the rest is still there for another. Regions alternate protection so
     * that none merges. */
    setup();
    memset(&l, 0, sizeof(l));
    memset(&child, 0, sizeof(child));
    {
        uint32_t i, got = 0;

        for (i = 0; i < POOL; i++) {
            if (anon(&l, 0x100000ull + (uint64_t)i * PAGE, PAGE, (i & 1u) ? RO : RW) == 0) {
                got++;
            }
        }
        if (got != POOL / 4u || walk_count(&l) != POOL / 4u ||
            anon(&child, 0x900000ull, PAGE, RW) != 0) {
            printf("FAIL:vma one address space took more than its quarter of the pool\n");
            goto fail;
        }
    }

    /* ---- 9. clone, and clone running out of descriptors ------------------ */
    setup();
    memset(&l, 0, sizeof(l));
    memset(&child, 0, sizeof(child));
    if (anon(&l, 0x60000ull, PAGE, RW) != 0) { goto fail; }
    if (anon(&l, 0x62000ull, PAGE, RO) != 0) { goto fail; }
    if (vibeos_vma_clone(&child, &l) != 0) { goto fail; }
    if (walk_count(&child) != 2u) { goto fail; }
    if (child.head->base != 0x60000ull || child.head->next->base != 0x62000ull) { goto fail; }
    if (child.head->prot != RW || child.head->next->prot != RO) { goto fail; }
    /* Independent afterwards: unmapping in the child leaves the parent alone,
     * which is the entire point of cloning rather than sharing the list. */
    if (vibeos_vma_remove(&child, 0x60000ull, PAGE) != PAGE) { goto fail; }
    if (walk_count(&l) != 2u) {
        printf("FAIL:vma clone shared descriptors with the parent\n");
        goto fail;
    }
    vibeos_vma_clear(&child);
    vibeos_vma_clear(&l);
    if (vibeos_vma_live() != 0u) {
        printf("FAIL:vma leaked %u descriptors across clone and clear\n",
               vibeos_vma_live());
        goto fail;
    }

    /* An exhausted pool leaves the destination empty rather than half a
     * picture: a process whose regions describe some of its memory is worse
     * than one whose fork failed. */
    {
        vibeos_vma_list_t big, others[3], small;
        uint32_t i;

        setup();
        memset(&big, 0, sizeof(big));
        memset(others, 0, sizeof(others));
        memset(&small, 0, sizeof(small));
        /* The pool emptied by four lists, each at its quarter (M-127). */
        for (i = 0; i < POOL; i++) {
            /* Every other page, so nothing merges and each insert costs one. */
            vibeos_vma_list_t *to = i < POOL / 4u ? &big : &others[i / (POOL / 4u) - 1u];

            if (anon(to, 0x100000ull + (uint64_t)i * 2ull * PAGE, PAGE, RW) != 0) {
                break;
            }
        }
        if (vibeos_vma_live() != POOL) {
            printf("FAIL:vma four lists did not empty the pool\n");
            goto fail;
        }
        if (vibeos_vma_clone(&small, &big) == 0) {
            printf("FAIL:vma cloned into an exhausted pool\n");
            goto fail;
        }
        if (small.head != 0 || small.count != 0u) {
            printf("FAIL:vma left half a cloned list behind\n");
            goto fail;
        }
    }

    /* ---- 10. a split advances the backing offset ------------------------
     *
     * Anonymous memory has no offset to get wrong, so this needs a file-backed
     * region to be visible at all - and it stays invisible until something
     * actually reads through the upper half, which is P4. Written now, while
     * the code that has to be right is being written. */
    {
        vibeos_vma_list_t f;
        vibeos_vma_t *hi;

        setup();
        memset(&f, 0, sizeof(f));
        if (vibeos_vma_insert(&f, 0x70000ull, 4ull * PAGE, RO,
                              VIBEOS_BACKING_FILE, 7u, 0x8000ull) != 0) { goto fail; }
        if (vibeos_vma_remove(&f, 0x71000ull, PAGE) != PAGE) { goto fail; }
        hi = vibeos_vma_find(&f, 0x72000ull);
        if (!hi) { goto fail; }
        if (hi->backing_offset != 0x8000ull + 2ull * PAGE) {
            printf("FAIL:vma split did not advance the backing offset\n");
            goto fail;
        }
        /* ...and trimming the head advances it too, for the same reason. */
        if (vibeos_vma_remove(&f, 0x72000ull, PAGE) != PAGE) { goto fail; }
        hi = vibeos_vma_find(&f, 0x73000ull);
        if (!hi || hi->backing_offset != 0x8000ull + 3ull * PAGE) {
            printf("FAIL:vma head trim did not advance the backing offset\n");
            goto fail;
        }
        vibeos_vma_clear(&f);
    }

    /* ---- W3: a region's state and its allocation ------------------------ */
    {
        vibeos_vma_list_t w;
        vibeos_vma_t *v;

        memset(&w, 0, sizeof(w));
        /* A reservation of four pages, the middle two committed. */
        if (vibeos_vma_insert_alloc(&w, 0x100000ull, 4ull * PAGE, RW, 0u, VIBEOS_VMA_RESERVED,
                                    0x100000ull, 0x04u) != 0 ||
            vibeos_vma_change(&w, 0x101000ull, 2ull * PAGE, VIBEOS_VMA_CHANGE_STATE, RW, 0u, VIBEOS_VMA_DEMAND) != 0) {
            printf("FAIL:vma a reservation and a commit inside it\n");
            goto fail;
        }
        if (walk_count(&w) != 3u || !ordered(&w) ||
            vibeos_vma_find(&w, 0x100000ull)->state != VIBEOS_VMA_RESERVED ||
            vibeos_vma_find(&w, 0x101000ull)->state != VIBEOS_VMA_DEMAND ||
            vibeos_vma_find(&w, 0x103000ull)->state != VIBEOS_VMA_RESERVED) {
            printf("FAIL:vma a commit inside a reservation is three regions, each its state\n");
            goto fail;
        }
        v = vibeos_vma_find(&w, 0x102000ull);
        if (v->alloc_base != 0x100000ull || v->alloc_prot != 0x04u) {
            printf("FAIL:vma a split keeps the allocation's base and protection\n");
            goto fail;
        }
        /* Decommitted again, the three are one reservation once more. */
        if (vibeos_vma_change(&w, 0x101000ull, 2ull * PAGE, VIBEOS_VMA_CHANGE_STATE, RW, 0u, VIBEOS_VMA_RESERVED) != 0 ||
            walk_count(&w) != 1u) {
            printf("FAIL:vma decommitting merges the reservation back into one region\n");
            goto fail;
        }
        /* A second allocation right after it is not merged into it, though
         * everything else about the two is alike. */
        if (vibeos_vma_insert_alloc(&w, 0x104000ull, PAGE, RW, 0u, VIBEOS_VMA_RESERVED,
                                    0x104000ull, 0x04u) != 0 || walk_count(&w) != 2u) {
            printf("FAIL:vma two allocations side by side stay two regions\n");
            goto fail;
        }
        if (vibeos_vma_change(&w, 0x105000ull, PAGE, VIBEOS_VMA_CHANGE_STATE, RW, 0u, VIBEOS_VMA_DEMAND) == 0) {
            printf("FAIL:vma a state change outside every region is refused\n");
            goto fail;
        }
        /* The protection as Windows spelled it: PAGE_READWRITE | PAGE_GUARD on
         * one page of a committed range. The machine's protection is the same
         * on both sides, so only the spelling can keep the guard page apart. */
        if (vibeos_vma_change(&w, 0x100000ull, 4ull * PAGE,
                              VIBEOS_VMA_CHANGE_STATE | VIBEOS_VMA_CHANGE_PROT, RW, 0x04u,
                              VIBEOS_VMA_DEMAND) != 0 ||
            vibeos_vma_change(&w, 0x102000ull, PAGE, VIBEOS_VMA_CHANGE_PROT, RW, 0x104u,
                              VIBEOS_VMA_DEMAND) != 0) {
            printf("FAIL:vma a change of the spelled protection alone\n");
            goto fail;
        }
        if (vibeos_vma_find(&w, 0x102000ull)->user_prot != 0x104u ||
            vibeos_vma_find(&w, 0x101000ull)->user_prot != 0x04u ||
            vibeos_vma_find(&w, 0x103000ull)->user_prot != 0x04u || walk_count(&w) != 4u) {
            printf("FAIL:vma a page spelled differently is a region of its own\n");
            goto fail;
        }
        memset(&child, 0, sizeof(child));
        if (vibeos_vma_clone(&child, &w) != 0 ||
            vibeos_vma_find(&child, 0x104000ull)->alloc_base != 0x104000ull ||
            vibeos_vma_find(&child, 0x100000ull)->state != VIBEOS_VMA_DEMAND ||
            vibeos_vma_find(&child, 0x104000ull)->state != VIBEOS_VMA_RESERVED ||
            vibeos_vma_find(&child, 0x102000ull)->user_prot != 0x104u) {
            printf("FAIL:vma fork's copy keeps states and allocations\n");
            goto fail;
        }
        vibeos_vma_clear(&child);
        vibeos_vma_clear(&w);
    }

    /* ---- W3 step 10: an allocation split and joined (placeholders) ------ */
    {
        vibeos_vma_list_t w;

        memset(&w, 0, sizeof(w));
        if (vibeos_vma_insert_alloc(&w, 0x200000ull, 4ull * PAGE, 0u, 0u, VIBEOS_VMA_RESERVED,
                                    0x200000ull, 0x01u) != 0 ||
            vibeos_vma_change_alloc(&w, 0x202000ull, PAGE, VIBEOS_VMA_CHANGE_ALLOC, 0u, 0u,
                                    VIBEOS_VMA_RESERVED, 0x202000ull, 0x01u) != 0) {
            printf("FAIL:vma an allocation's middle page given a base of its own\n");
            goto fail;
        }
        if (walk_count(&w) != 3u || vibeos_vma_find(&w, 0x201000ull)->alloc_base != 0x200000ull ||
            vibeos_vma_find(&w, 0x202000ull)->alloc_base != 0x202000ull ||
            vibeos_vma_find(&w, 0x203000ull)->alloc_base != 0x200000ull ||
            vibeos_vma_find(&w, 0x202000ull)->state != VIBEOS_VMA_RESERVED) {
            printf("FAIL:vma a base changed on one page only, its state untouched\n");
            goto fail;
        }
        /* The state and the allocation in one change, and back to one. */
        if (vibeos_vma_change_alloc(&w, 0x203000ull, PAGE,
                                    VIBEOS_VMA_CHANGE_ALLOC | VIBEOS_VMA_CHANGE_STATE |
                                        VIBEOS_VMA_CHANGE_PROT, RW, 0x04u, VIBEOS_VMA_DEMAND,
                                    0x203000ull, 0x04u) != 0 ||
            vibeos_vma_find(&w, 0x203000ull)->state != VIBEOS_VMA_DEMAND ||
            vibeos_vma_find(&w, 0x203000ull)->alloc_prot != 0x04u ||
            vibeos_vma_change_alloc(&w, 0x200000ull, 4ull * PAGE,
                                    VIBEOS_VMA_CHANGE_ALLOC | VIBEOS_VMA_CHANGE_STATE |
                                        VIBEOS_VMA_CHANGE_PROT, 0u, 0u, VIBEOS_VMA_RESERVED,
                                    0x200000ull, 0x01u) != 0 ||
            walk_count(&w) != 1u) {
            printf("FAIL:vma split allocations joined back into one region\n");
            goto fail;
        }
        /* vibeos_vma_change does not take the allocation's flag. */
        if (vibeos_vma_change(&w, 0x200000ull, PAGE, VIBEOS_VMA_CHANGE_ALLOC, 0u, 0u,
                              VIBEOS_VMA_RESERVED) != 0 ||
            vibeos_vma_find(&w, 0x200000ull)->alloc_base != 0x200000ull || walk_count(&w) != 1u) {
            printf("FAIL:vma vibeos_vma_change leaves the allocation alone\n");
            goto fail;
        }
        vibeos_vma_clear(&w);
    }

    return 0;

fail:
    return -1;
}
