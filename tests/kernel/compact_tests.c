/* Host tests for compaction (plan phase P6, step 5).
 *
 * Compaction is the one operation in this subsystem that moves memory that is
 * still in use, so the tests are weighted accordingly: two of them check that
 * it works, and the rest check that it *refuses*. Getting a move wrong does
 * not lose performance - it hands a running process the contents of a
 * different page, which is the failure this whole rewrite exists to end.
 *
 * The refusals are therefore the specification, not the edge cases:
 *
 *   pinned            a page table or a DMA buffer must never move
 *   writable          the copy-then-repoint window would lose a store
 *   too many holders  a partly-moved frame is a state nothing can recover from
 *   raced             the holder list changed, so what was read is not there
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/vmspace.h"
#include "vibeos/frame.h"
#include "vibeos/rmap.h"
#include "vibeos/mm_stats.h"
#include "vibeos/mm_model.h"
#include "vibeos/reclaim.h"

int test_compact(void);
void vibeos_rmap_set_base(uint64_t base_phys);

#define CP_FRAMES 256u
#define CP_BASE   0x900000ull

static unsigned char *g_ram;
static vibeos_frame_t g_ftable[CP_FRAMES];
static unsigned char g_rpool[CP_FRAMES * sizeof(uint32_t) + 4096u];

static int g_fail;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  compact: FAIL %s\n", (what));                           \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static void *cp_map(uint64_t phys) {
    if (phys < CP_BASE || phys >= CP_BASE + (uint64_t)CP_FRAMES * 4096ull) {
        return 0;
    }
    return g_ram + (phys - CP_BASE);
}

static uint64_t cp_alloc_table(void) {
    return vibeos_frame_alloc(VIBEOS_FRAME_PAGE_TABLE);
}

static void cp_free_table(uint64_t phys) {
    (void)vibeos_frame_put(phys);
}

static uint64_t g_shootdowns;

static void cp_shootdown(uint64_t root_phys) {
    (void)root_phys;
    g_shootdowns++;
}

static int cp_setup(void) {
    vibeos_vmspace_backend_t be;

    memset(g_ftable, 0, sizeof(g_ftable));
    memset(g_ram, 0, (size_t)CP_FRAMES * 4096u);
    memset(g_rpool, 0, sizeof(g_rpool));
    vibeos_mm_stats_reset();
    g_shootdowns = 0;

    if (vibeos_frame_init(CP_BASE, (uint64_t)CP_FRAMES * 4096ull,
                          g_ftable, CP_FRAMES, cp_map) != 0) {
        return -1;
    }
    vibeos_rmap_set_base(CP_BASE);
    if (vibeos_rmap_init(g_rpool, (uint64_t)sizeof(g_rpool), CP_FRAMES) != 0) {
        return -1;
    }
    memset(&be, 0, sizeof(be));
    be.map_phys = cp_map;
    be.alloc_table = cp_alloc_table;
    be.free_table = cp_free_table;
    be.shootdown = cp_shootdown;
    return vibeos_vmspace_init(&be);
}

#define VA_A 0x8000000000ull

/* Map a frame the way the kernel does, which is decision D9: a caller that
 * allocates *in order to map* hands the frame over and lets go, so the
 * mapping's reference is the only one.
 *
 * The tests did not do this at first and every move was refused for the right
 * reason - the frame had an owner that was not a mapping, namely the test
 * itself. That is the same refusal that protects a cache page from being moved
 * out from under the entry holding its address, so the tests were wrong and
 * the layer was right. Arranging a test differently from the code it exercises
 * is how a test comes to disagree with reality. */
static int map_as_kernel_does(vibeos_vmspace_t *as, uint64_t va, uint64_t phys,
                              vibeos_prot_t prot) {
    if (vibeos_vmspace_map(as, va, phys, prot) != 0) {
        return -1;
    }
    (void)vibeos_frame_put(phys);   /* D9: the mapping owns it now */
    return 0;
}

/* --- it works ------------------------------------------------------------- */

/* A read-only page moves, its contents arrive, and the mapping follows it. */
static void test_move_carries_contents_and_mapping(void) {
    vibeos_vmspace_t as;
    uint64_t old_f, new_f;
    unsigned char *p;

    if (cp_setup() != 0 || vibeos_vmspace_create(&as) != 0) {
        printf("  compact: FAIL setup\n"); g_fail++; return;
    }
    old_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    new_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    CHECK(old_f && new_f, "two frames");

    p = (unsigned char *)cp_map(old_f);
    memset(p, 0xA5, 4096);

    CHECK(map_as_kernel_does(&as, VA_A, old_f,
                             VIBEOS_PROT_READ | VIBEOS_PROT_USER) == 0, "map");
    CHECK(vibeos_rmap_count(old_f) == 1u, "one holder before");

    CHECK(vibeos_vmspace_move_frame(old_f, new_f) == 0, "the move is accepted");

    /* The contents arrived. */
    {
        unsigned char *q = (unsigned char *)cp_map(new_f);
        int same = 1;
        unsigned i;
        for (i = 0; i < 4096u; i++) {
            if (q[i] != 0xA5) { same = 0; break; }
        }
        CHECK(same, "contents copied");
    }

    /* The mapping followed, and the reverse map with it. */
    {
        uint64_t *pte = vibeos_vmspace_entry(&as, VA_A);
        CHECK(pte != 0, "entry still there");
        if (pte) {
            CHECK((*pte & 0x000FFFFFFFFFF000ull) == new_f,
                  "the entry points at the new frame");
        }
    }
    CHECK(vibeos_rmap_count(new_f) == 1u, "the new frame has the holder");
    CHECK(vibeos_rmap_count(old_f) == 0u, "the old frame has none");
    CHECK(g_shootdowns >= 1u, "a shootdown was sent");
    CHECK(vibeos_mm_stats()->compact_moved == 1u, "counted as moved");
    CHECK(vibeos_mm_stats()->compact_mappings_moved == 1u, "one mapping moved");
}

/* Two address spaces sharing a read-only frame both follow it. A move that
 * repointed one and not the other would leave two processes reading different
 * memory through the same address - which is exactly the class of defect that
 * makes this operation dangerous. */
static void test_move_follows_every_holder(void) {
    vibeos_vmspace_t a, b;
    uint64_t old_f, new_f;

    if (cp_setup() != 0) { printf("  compact: FAIL setup\n"); g_fail++; return; }
    if (vibeos_vmspace_create(&a) != 0 || vibeos_vmspace_create(&b) != 0) {
        printf("  compact: FAIL create\n"); g_fail++; return;
    }
    old_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    new_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);

    (void)vibeos_vmspace_map(&a, VA_A, old_f,
                             VIBEOS_PROT_READ | VIBEOS_PROT_USER);
    (void)map_as_kernel_does(&b, VA_A, old_f,
                             VIBEOS_PROT_READ | VIBEOS_PROT_USER);
    CHECK(vibeos_rmap_count(old_f) == 2u, "two holders");

    CHECK(vibeos_vmspace_move_frame(old_f, new_f) == 0, "accepted");
    CHECK(vibeos_mm_stats()->compact_mappings_moved == 2u, "both moved");

    {
        uint64_t *pa = vibeos_vmspace_entry(&a, VA_A);
        uint64_t *pb = vibeos_vmspace_entry(&b, VA_A);
        CHECK(pa && (*pa & 0x000FFFFFFFFFF000ull) == new_f, "a follows");
        CHECK(pb && (*pb & 0x000FFFFFFFFFF000ull) == new_f, "b follows");
    }
    CHECK(vibeos_rmap_count(new_f) == 2u, "both holders recorded on the new");
    CHECK(vibeos_rmap_count(old_f) == 0u, "and none on the old");
}

/* --- it refuses ----------------------------------------------------------- */

/* Pinned never moves. The safety refusal. */
static void test_refuses_pinned(void) {
    vibeos_vmspace_t as;
    uint64_t old_f, new_f;

    if (cp_setup() != 0 || vibeos_vmspace_create(&as) != 0) {
        printf("  compact: FAIL setup\n"); g_fail++; return;
    }
    old_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    new_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    (void)map_as_kernel_does(&as, VA_A, old_f,
                             VIBEOS_PROT_READ | VIBEOS_PROT_USER);
    vibeos_frame_set_flag(old_f, VIBEOS_FRAME_PINNED);

    CHECK(vibeos_vmspace_move_frame(old_f, new_f) != 0, "refused");
    CHECK(vibeos_mm_stats()->compact_refused_pinned == 1u, "counted as pinned");
    CHECK(vibeos_mm_stats()->compact_moved == 0u, "and nothing moved");

    /* And the entry is untouched - a refusal that had already copied or
     * repointed something would be worse than an acceptance. */
    {
        uint64_t *pte = vibeos_vmspace_entry(&as, VA_A);
        CHECK(pte && (*pte & 0x000FFFFFFFFFF000ull) == old_f,
              "the mapping is exactly as it was");
    }
}

/* A writable holder is refused, because the copy-then-repoint window would
 * lose a store. This is the restriction that makes the window harmless rather
 * than unlikely. */
static void test_refuses_writable(void) {
    vibeos_vmspace_t as;
    uint64_t old_f, new_f;

    if (cp_setup() != 0 || vibeos_vmspace_create(&as) != 0) {
        printf("  compact: FAIL setup\n"); g_fail++; return;
    }
    old_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    new_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    (void)map_as_kernel_does(&as, VA_A, old_f,
                             VIBEOS_PROT_READ | VIBEOS_PROT_WRITE |
                             VIBEOS_PROT_USER);

    CHECK(vibeos_vmspace_move_frame(old_f, new_f) != 0, "refused");
    CHECK(vibeos_mm_stats()->compact_refused_writable == 1u, "counted");
    CHECK(vibeos_mm_stats()->compact_moved == 0u, "nothing moved");
}

/* One writable holder among several read-only ones is still a refusal, **in
 * either order**.
 *
 * Both orders, because the first version of this test ran only one and a
 * sabotage case walked straight through it. The reverse map inserts at the
 * head, so the most recently mapped holder is examined first - and with the
 * writable mapping added last, a check that stopped after one holder still
 * found it and the test still passed. It was asserting the right thing about
 * the wrong arrangement.
 *
 * A loop that stops early is the natural way to get this wrong, and on a real
 * machine the writable holder is not reliably the one at the head. */
static void writable_among_readonly(int writable_first, const char *what) {
    vibeos_vmspace_t a, b;
    uint64_t old_f, new_f;
    vibeos_prot_t ro = (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_USER);
    vibeos_prot_t rw = (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_WRITE |
                                       VIBEOS_PROT_USER);

    if (cp_setup() != 0) { printf("  compact: FAIL setup\n"); g_fail++; return; }
    if (vibeos_vmspace_create(&a) != 0 || vibeos_vmspace_create(&b) != 0) {
        printf("  compact: FAIL create\n"); g_fail++; return;
    }
    old_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    new_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);

    if (writable_first) {
        (void)vibeos_vmspace_map(&a, VA_A, old_f, rw);
        (void)map_as_kernel_does(&b, VA_A, old_f, ro);
    } else {
        (void)vibeos_vmspace_map(&a, VA_A, old_f, ro);
        (void)map_as_kernel_does(&b, VA_A, old_f, rw);
    }
    CHECK(vibeos_rmap_count(old_f) == 2u, "two holders");
    CHECK(vibeos_vmspace_move_frame(old_f, new_f) != 0, what);
    CHECK(vibeos_mm_stats()->compact_refused_writable == 1u, "counted");
    CHECK(vibeos_rmap_count(old_f) == 2u, "both holders still on the old frame");
}

static void test_refuses_if_any_holder_is_writable(void) {
    writable_among_readonly(0, "refused with the writable holder mapped last");
    writable_among_readonly(1, "refused with the writable holder mapped first");
}

/* Moving a frame onto itself, or on an unaligned address, is refused before
 * anything is touched. */
static void test_refuses_nonsense(void) {
    uint64_t f;

    if (cp_setup() != 0) { printf("  compact: FAIL setup\n"); g_fail++; return; }
    f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);

    CHECK(vibeos_vmspace_move_frame(f, f) != 0, "onto itself");
    CHECK(vibeos_vmspace_move_frame(f + 1ull, f) != 0, "unaligned source");
    CHECK(vibeos_vmspace_move_frame(f, f + 1ull) != 0, "unaligned target");
}

/* A frame with a reference that is not a mapping is refused, and this is a
 * safety refusal rather than a limitation.
 *
 * An allocated frame nobody has mapped still has an owner: whoever allocated
 * it and kept the physical address - the page cache, a DMA buffer, a kernel
 * structure. The reverse map cannot repoint those because it does not know
 * they exist, so moving the contents would leave that holder reading the old
 * frame while everything else reads the new one.
 *
 * This test was written the other way round at first, asserting that such a
 * frame *should* move because it looked like the easiest case. The
 * fragmentation test disproved it: the "moved" frame never became free,
 * because the allocation reference stayed exactly where it was. That is the
 * harmless face of the same defect. */
static void test_refuses_untracked_reference(void) {
    uint64_t old_f, new_f;
    unsigned char *p;

    if (cp_setup() != 0) { printf("  compact: FAIL setup\n"); g_fail++; return; }
    old_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    new_f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    p = (unsigned char *)cp_map(old_f);
    memset(p, 0x3C, 4096);

    CHECK(vibeos_rmap_count(old_f) == 0u, "no holders");
    CHECK(vibeos_frame_owners(old_f) == 1u, "but one owner");
    CHECK(vibeos_vmspace_move_frame(old_f, new_f) != 0, "refused");
    CHECK(vibeos_mm_stats()->compact_refused_untracked == 1u, "counted");
    CHECK(vibeos_mm_stats()->compact_moved == 0u, "nothing moved");
}

/* --- the only honest test of compaction ----------------------------------- */

/* Fragment memory deliberately, then ask for a contiguous run.
 *
 * The plan calls this the only honest test of compaction, because every other
 * test here checks a mechanism in isolation. This one asks what a machine
 * actually asks: there is plenty free and none of it in one piece - can a
 * driver still get its buffer?
 *
 * The occupied frames are *mapped read-only*, not merely allocated, and that
 * is the whole point rather than a detail. A plain allocated frame has an
 * owner the reverse map cannot see and is correctly refused; what fragments a
 * real machine and what compaction can actually move are both the mapped kind
 * - page-cache pages and program text. Fragmenting with unmovable frames would
 * be a test of the refusal, which is elsewhere.
 */
static void test_fragmented_then_contiguous(void) {
    vibeos_vmspace_t as;
    uint64_t held[CP_FRAMES];
    uint32_t n = 0, i;
    uint32_t before, after;
    const uint32_t want = 8u;

    if (cp_setup() != 0 || vibeos_vmspace_create(&as) != 0) {
        printf("  compact: FAIL setup\n"); g_fail++; return;
    }
    vibeos_reclaim_set_region(CP_BASE, CP_FRAMES);

    /* Half the region, each frame mapped read-only at its own address. Not all
     * of it: the address space needs page tables of its own, and an allocator
     * with nothing left cannot build them. */
    for (i = 0; i < CP_FRAMES; i++) {
        uint64_t f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        if (!f) {
            break;
        }
        if (map_as_kernel_does(&as, VA_A + (uint64_t)n * 4096ull, f,
                               (vibeos_prot_t)(VIBEOS_PROT_READ |
                                               VIBEOS_PROT_USER)) != 0) {
            break;
        }
        held[n++] = f;
    }
    CHECK(n > 32u, "enough mapped frames to fragment with");

    /* Unmap every other one. Half free, and no two adjacent - the worst case,
     * and the one a first-fit allocator can do nothing with. */
    for (i = 0; i < n; i += 2u) {
        (void)vibeos_vmspace_unmap(&as, VA_A + (uint64_t)i * 4096ull);
        held[i] = 0;
    }

    before = vibeos_reclaim_compact(0u);   /* measures without moving */
    CHECK(before < want, "fragmented: the largest run is shorter than we want");

    after = vibeos_reclaim_compact(want);
    CHECK(after >= want, "after compaction a run of the wanted length exists");
    CHECK(vibeos_reclaim_stats()->compact_frames_moved > 0u,
          "and it took actual moves to get there");
}

/* A window that cannot be cleared reports the truth rather than a success.
 *
 * Every odd frame is pinned, so no window of the wanted size can ever be
 * emptied. The compactor must come back with a measurement that says so - a
 * caller that was told "done" and then failed to allocate would have no way to
 * find out why. */
static void test_unclearable_window_is_honest(void) {
    vibeos_vmspace_t as;
    uint64_t held[CP_FRAMES];
    uint32_t n = 0, i;
    uint32_t after;

    if (cp_setup() != 0 || vibeos_vmspace_create(&as) != 0) {
        printf("  compact: FAIL setup\n"); g_fail++; return;
    }
    vibeos_reclaim_set_region(CP_BASE, CP_FRAMES);

    for (i = 0; i < CP_FRAMES; i++) {
        uint64_t f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        if (!f) { break; }
        if (map_as_kernel_does(&as, VA_A + (uint64_t)n * 4096ull, f,
                               (vibeos_prot_t)(VIBEOS_PROT_READ |
                                               VIBEOS_PROT_USER)) != 0) {
            break;
        }
        held[n++] = f;
    }
    for (i = 0; i < n; i += 2u) {
        (void)vibeos_vmspace_unmap(&as, VA_A + (uint64_t)i * 4096ull);
        held[i] = 0;
    }
    /* Everything still mapped is pinned, so nothing can move. */
    for (i = 1; i < n; i += 2u) {
        vibeos_frame_set_flag(held[i], VIBEOS_FRAME_PINNED);
    }

    after = vibeos_reclaim_compact(16u);
    CHECK(after < 16u, "it reports the run it could not open");
    CHECK(vibeos_mm_stats()->compact_refused_pinned > 0u,
          "and says the frames were pinned");
}

int test_compact(void) {
    g_fail = 0;

    g_ram = (unsigned char *)malloc((size_t)CP_FRAMES * 4096u);
    if (!g_ram) {
        printf("  compact: FAIL out of memory\n");
        return 1;
    }

    test_move_carries_contents_and_mapping();
    test_move_follows_every_holder();
    test_refuses_pinned();
    test_refuses_writable();
    test_refuses_if_any_holder_is_writable();
    test_refuses_nonsense();
    test_refuses_untracked_reference();
    test_fragmented_then_contiguous();
    test_unclearable_window_is_honest();

    free(g_ram);
    g_ram = 0;

    if (g_fail == 0) {
        printf("  compact: 9 groups ok\n");
    }
    return g_fail == 0 ? 0 : 1;
}
