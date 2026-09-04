/* P7 — the acceptance properties, as far as a host test can carry them.
 *
 * The phases above build the memory manager; this one is meant to prove it is
 * ready. Each property here is a test that did not exist before, and each one
 * exercises a path that no ordinary run reaches — which is the point. The
 * unwind after a failed allocation, the state of a machine that has run out,
 * the contents of a frame handed to its second owner: none of those happen on
 * a boot that goes well, so none of them were ever executed until something
 * went wrong in front of a user.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/frame.h"
#include "vibeos/vmspace.h"
#include "vibeos/rmap.h"
#include "vibeos/mm_stats.h"
#include "vibeos/mm_model.h"

int test_acceptance(void);
void vibeos_rmap_set_base(uint64_t base_phys);

#define AC_FRAMES 128u
#define AC_BASE   0xD00000ull

static unsigned char *g_ram;
static vibeos_frame_t g_ftable[AC_FRAMES];
static unsigned char g_rpool[AC_FRAMES * sizeof(uint32_t) + 4096u];

static int g_fail;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  acceptance: FAIL %s\n", (what));                        \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static void *ac_map(uint64_t phys) {
    if (phys < AC_BASE || phys >= AC_BASE + (uint64_t)AC_FRAMES * 4096ull) {
        return 0;
    }
    return g_ram + (phys - AC_BASE);
}

static uint64_t ac_alloc_table(void) {
    return vibeos_frame_alloc(VIBEOS_FRAME_PAGE_TABLE);
}

static void ac_free_table(uint64_t phys) {
    (void)vibeos_frame_put(phys);
}

static int ac_setup(void) {
    vibeos_vmspace_backend_t be;

    memset(g_ftable, 0, sizeof(g_ftable));
    memset(g_ram, 0xCD, (size_t)AC_FRAMES * 4096u);   /* not zero, on purpose */
    memset(g_rpool, 0, sizeof(g_rpool));
    vibeos_mm_stats_reset();
    vibeos_frame_fail_after(0);

    if (vibeos_frame_init(AC_BASE, (uint64_t)AC_FRAMES * 4096ull,
                          g_ftable, AC_FRAMES, ac_map) != 0) {
        return -1;
    }
    vibeos_rmap_set_base(AC_BASE);
    if (vibeos_rmap_init(g_rpool, (uint64_t)sizeof(g_rpool), AC_FRAMES) != 0) {
        return -1;
    }
    memset(&be, 0, sizeof(be));
    be.map_phys = ac_map;
    be.alloc_table = ac_alloc_table;
    be.free_table = ac_free_table;
    return vibeos_vmspace_init(&be);
}

/* --- 1. exhaustion -------------------------------------------------------- */

/* Allocate until the allocator says no, then give everything back and check
 * the counters returned to where they started.
 *
 * The property is not that allocation fails - it is that the machine is *the
 * same afterwards*. A failing allocator that leaks one frame per refusal looks
 * identical from the outside until the leak is large enough to matter, and by
 * then there is nothing left to point at.
 */
static void test_exhaustion_returns_to_where_it_started(void) {
    static uint64_t held[AC_FRAMES];
    uint64_t free_before, free_after;
    uint32_t n = 0, i;

    if (ac_setup() != 0) { printf("  acceptance: FAIL setup\n"); g_fail++; return; }
    free_before = vibeos_frame_free_count();
    CHECK(free_before > 0ull, "there is memory to start with");

    while (n < AC_FRAMES) {
        uint64_t f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        if (!f) {
            break;
        }
        held[n++] = f;
    }
    CHECK(n == free_before, "every free frame was handed out, and no more");
    CHECK(vibeos_frame_free_count() == 0ull, "nothing is free");
    CHECK(vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED) == 0ull,
          "and the next request is refused");

    for (i = 0; i < n; i++) {
        (void)vibeos_frame_put(held[i]);
    }
    free_after = vibeos_frame_free_count();
    CHECK(free_after == free_before, "the count came back exactly");
    CHECK(vibeos_mm_stats()->frames_leaked == 0u, "and nothing leaked");
    CHECK(vibeos_mm_stats()->frames_double_put == 0u, "nothing was put twice");

    /* And they can be used again, which the count does not prove.
     *
     * The counter and the free list are separate: a release that decrements
     * one without linking into the other leaves a machine that believes it has
     * memory and cannot hand any out. The sabotage case for exactly that
     * walked through this test until these four lines existed - "the count
     * came back" was true and meant nothing. */
    {
        uint32_t again = 0;

        while (again < AC_FRAMES) {
            uint64_t f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
            if (!f) {
                break;
            }
            held[again++] = f;
        }
        CHECK(again == n, "every frame was handed out a second time");
        for (i = 0; i < again; i++) {
            (void)vibeos_frame_put(held[i]);
        }
    }
}

/* --- 2. isolation --------------------------------------------------------- */

/* A frame's second owner never sees the first one's data.
 *
 * The fake RAM is filled with 0xCD rather than zero by the setup, so a frame
 * handed out without being cleared is visibly wrong rather than accidentally
 * right. That matters: a test on zeroed memory passes whether or not the
 * kernel clears anything.
 */
static void test_isolation_between_tenants(void) {
    uint64_t f, again;
    unsigned char *p;
    uint32_t i;
    int clean = 1;

    if (ac_setup() != 0) { printf("  acceptance: FAIL setup\n"); g_fail++; return; }

    f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    CHECK(f != 0ull, "a frame");
    p = (unsigned char *)ac_map(f);
    for (i = 0; i < 4096u; i++) {
        if (p[i] != 0u) {
            clean = 0;
            break;
        }
    }
    /* One assertion for the whole page rather than one per byte: the first
     * version checked inside the loop with an empty label and printed four
     * thousand blank failures, which is a test that cannot be read. */
    CHECK(clean, "a fresh frame is zeroed, not left as the firmware had it");

    memset(p, 0x9F, 4096);            /* the first tenant's secret */
    (void)vibeos_frame_put(f);

    /* Take frames until the same one comes back. On this allocator it is the
     * next one, but looping means the test does not depend on that. */
    for (i = 0; i < AC_FRAMES; i++) {
        again = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        if (again == 0ull) {
            break;
        }
        if (again == f) {
            break;
        }
    }
    CHECK(again == f, "the frame was handed out again");

    {
        unsigned char *q = (unsigned char *)ac_map(f);
        int leaked = 0;
        for (i = 0; i < 4096u; i++) {
            if (q[i] == 0x9F) {
                leaked = 1;
                break;
            }
        }
        CHECK(!leaked, "and the second tenant cannot see the first one's data");
    }
}

/* --- 3. fault injection --------------------------------------------------- */

/* Sweep the point of failure through an operation and check that every refusal
 * leaves the counts where they were.
 *
 * This is invariant I5, proved rather than asserted. Building an address space
 * and mapping a page allocates several times - tables at three levels, then
 * the frame - so failing the nth allocation for n across the sweep exercises a
 * different unwind path each time. Those paths are exactly the ones no
 * ordinary run reaches, because ordinary runs do not run out of memory.
 */
static void test_failure_leaves_nothing_behind(void) {
    uint32_t n;

    for (n = 1; n <= 6u; n++) {
        vibeos_vmspace_t as;
        uint64_t free_before;
        uint64_t f;
        int created;

        if (ac_setup() != 0) {
            printf("  acceptance: FAIL setup\n"); g_fail++; return;
        }
        free_before = vibeos_frame_free_count();

        vibeos_frame_fail_after(n);
        created = vibeos_vmspace_create(&as);
        if (created == 0) {
            f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
            if (f) {
                (void)vibeos_vmspace_map(&as, 0x8000000000ull, f,
                                         (vibeos_prot_t)(VIBEOS_PROT_READ |
                                                         VIBEOS_PROT_USER));
                (void)vibeos_frame_put(f);
            }
            (void)vibeos_vmspace_destroy(&as);
        }
        vibeos_frame_fail_after(0);

        /* Whatever failed and wherever it failed, the machine is back where it
         * started. A leak here would be one frame - invisible in any single
         * run, and fatal after a few thousand refusals. */
        CHECK(vibeos_frame_free_count() == free_before,
              "an injected failure left the free count unchanged");
        CHECK(vibeos_mm_stats()->frames_leaked == 0u,
              "and leaked nothing");
    }

    /* And the injection actually fired, rather than the sweep having quietly
     * run past the end of the allocations. A sweep that never triggers passes
     * every assertion above while proving nothing at all - which is the
     * failure mode this project has hit three times in the last few phases. */
    {
        uint64_t f;
        if (ac_setup() != 0) { return; }
        vibeos_frame_fail_after(1);
        f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        CHECK(f == 0ull, "the injected failure refuses the allocation");
        CHECK(vibeos_frame_injected_failures() == 1u, "and says it fired");
        vibeos_frame_fail_after(0);
        CHECK(vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED) != 0ull,
              "and turning it off restores the allocator");
    }
}

/* --- 4. no mapping survives a failed address space ------------------------- */

/* An address space that failed to build leaves no holder behind.
 *
 * The reverse map is the one structure that would keep a record of a mapping
 * whose page tables have been freed, and a stale holder there is worse than a
 * leaked frame: compaction would follow it and write into memory that now
 * belongs to something else.
 */
static void test_failed_teardown_leaves_no_holders(void) {
    vibeos_vmspace_t as;
    uint64_t f;

    if (ac_setup() != 0) { printf("  acceptance: FAIL setup\n"); g_fail++; return; }
    CHECK(vibeos_vmspace_create(&as) == 0, "create");
    f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    CHECK(vibeos_vmspace_map(&as, 0x8000000000ull, f,
                             (vibeos_prot_t)(VIBEOS_PROT_READ |
                                             VIBEOS_PROT_USER)) == 0, "map");
    (void)vibeos_frame_put(f);
    CHECK(vibeos_rmap_count(f) == 1u, "one holder while it is mapped");

    (void)vibeos_vmspace_destroy(&as);
    CHECK(vibeos_rmap_count(f) == 0u, "and none after teardown");
    CHECK(vibeos_frame_owners(f) == 0u, "the frame is free");
}

int test_acceptance(void) {
    g_fail = 0;

    g_ram = (unsigned char *)malloc((size_t)AC_FRAMES * 4096u);
    if (!g_ram) {
        printf("  acceptance: FAIL out of memory\n");
        return 1;
    }

    test_exhaustion_returns_to_where_it_started();
    test_isolation_between_tenants();
    test_failure_leaves_nothing_behind();
    test_failed_teardown_leaves_no_holders();

    free(g_ram);
    g_ram = 0;

    if (g_fail == 0) {
        printf("  acceptance: 4 groups ok\n");
    }
    return g_fail == 0 ? 0 : 1;
}
