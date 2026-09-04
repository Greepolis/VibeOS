/* Host tests for the reverse map (plan phase P6, step 1).
 *
 * What this layer has to be right about is identity, not quantity: compaction
 * will move a frame and repoint whatever these lists name, so a list that is
 * merely the right *length* while naming the wrong mapping is worse than no
 * list at all - it would let a move repoint an address space that never held
 * the frame and leave the one that did pointing at freed memory.
 *
 * So most of what follows checks that what comes back is what went in, and the
 * count tests exist mainly to pin the invariant the address-space layer is
 * audited against.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/rmap.h"

int test_rmap(void);
void vibeos_rmap_set_base(uint64_t base_phys);

#define RM_FRAMES 64u
#define RM_BASE   0x400000ull
#define FRAME(n)  (RM_BASE + (uint64_t)(n) * 4096ull)

/* Deliberately small: exhaustion is a state this layer must report rather than
 * crash in, and a pool that cannot run out cannot test that.
 *
 * Sized in bytes rather than in nodes, because the node is private to the
 * layer and this file must not know its size. The first version guessed 32
 * bytes per node when they are 24, so the pool held a third more than intended
 * and the exhaustion test passed without ever exhausting anything - a test that
 * asserts a condition it never reaches, which is the failure mode this project
 * has hit twice with vacuous NTFS cases. The attempt count below is a multiple
 * of the pool rather than a number tuned to it, so the test stays honest if the
 * node ever changes size. */
#define RM_POOL_BYTES 2048u

static unsigned char g_pool[RM_FRAMES * sizeof(uint32_t) + RM_POOL_BYTES];

static int g_fail;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  rmap: FAIL %s\n", (what));                              \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static void rm_reset(void) {
    memset(g_pool, 0, sizeof(g_pool));
    vibeos_rmap_set_base(RM_BASE);
    if (vibeos_rmap_init(g_pool, (uint64_t)sizeof(g_pool), RM_FRAMES) != 0) {
        printf("  rmap: FAIL init\n");
        g_fail++;
    }
}

/* A frame with two holders reports both, by name. */
static void test_two_holders(void) {
    vibeos_rmap_holder_t got[4];
    uint32_t n;
    int saw_a = 0, saw_b = 0;
    uint32_t i;

    rm_reset();
    CHECK(vibeos_rmap_add(FRAME(3), 0x1000ull, 0x8000ull) == 0, "add a");
    CHECK(vibeos_rmap_add(FRAME(3), 0x2000ull, 0x9000ull) == 0, "add b");
    CHECK(vibeos_rmap_count(FRAME(3)) == 2u, "count is two");

    n = vibeos_rmap_holders(FRAME(3), got, 4u);
    CHECK(n == 2u, "two holders returned");
    for (i = 0; i < n; i++) {
        if (got[i].root_phys == 0x1000ull && got[i].va == 0x8000ull) { saw_a = 1; }
        if (got[i].root_phys == 0x2000ull && got[i].va == 0x9000ull) { saw_b = 1; }
    }
    CHECK(saw_a && saw_b, "both holders named correctly");

    /* And nobody else's frame gained anything. */
    CHECK(vibeos_rmap_count(FRAME(4)) == 0u, "neighbour frame untouched");
}

/* Adding the same holder twice does not lengthen the list.
 *
 * The copy-on-write fault maps over an entry that is already there, and so
 * does a second map of the same page. If either grew the list, the count would
 * stop matching the frame's owner count and the audit built on that invariant
 * would report a defect on every healthy boot. */
static void test_duplicate_add(void) {
    rm_reset();
    CHECK(vibeos_rmap_add(FRAME(5), 0x1000ull, 0x8000ull) == 0, "first add");
    CHECK(vibeos_rmap_add(FRAME(5), 0x1000ull, 0x8000ull) == 0, "second add");
    CHECK(vibeos_rmap_count(FRAME(5)) == 1u, "duplicate did not lengthen");
}

/* The page offset is not part of the identity. A holder recorded for an
 * address inside a page must be found by any address inside that page, or
 * unmap and map would disagree about the same mapping. */
static void test_page_aligned(void) {
    rm_reset();
    CHECK(vibeos_rmap_add(FRAME(6), 0x1000ull, 0x8000ull) == 0, "add");
    CHECK(vibeos_rmap_remove(FRAME(6), 0x1000ull, 0x8ABCull) == 0,
          "removed by an address inside the same page");
    CHECK(vibeos_rmap_count(FRAME(6)) == 0u, "gone");
}

/* Removing one holder leaves the other, and leaves the right one. */
static void test_remove_middle(void) {
    vibeos_rmap_holder_t got[4];
    uint32_t n;

    rm_reset();
    (void)vibeos_rmap_add(FRAME(7), 0x1000ull, 0x8000ull);
    (void)vibeos_rmap_add(FRAME(7), 0x2000ull, 0x9000ull);
    (void)vibeos_rmap_add(FRAME(7), 0x3000ull, 0xA000ull);

    CHECK(vibeos_rmap_remove(FRAME(7), 0x2000ull, 0x9000ull) == 0, "remove b");
    CHECK(vibeos_rmap_count(FRAME(7)) == 2u, "two left");

    n = vibeos_rmap_holders(FRAME(7), got, 4u);
    CHECK(n == 2u, "two returned");
    CHECK(got[0].root_phys != 0x2000ull && got[1].root_phys != 0x2000ull,
          "the removed holder is not among them");
}

/* A remove that matches nothing is counted, not silently ignored: the two
 * sides disagreeing about what was mapped is the defect this subsystem keeps
 * producing, seen from the other end. */
static void test_missing_remove_counted(void) {
    rm_reset();
    CHECK(vibeos_rmap_stats()->missing_remove == 0u, "starts at zero");
    CHECK(vibeos_rmap_remove(FRAME(8), 0x1000ull, 0x8000ull) != 0,
          "remove of nothing fails");
    CHECK(vibeos_rmap_stats()->missing_remove == 1u, "and is counted");
}

/* Forgetting a whole address space removes its holders from every frame and
 * leaves everybody else's alone. This is what teardown uses, and getting it
 * wrong in the generous direction would silently unrecord live mappings. */
static void test_forget_root(void) {
    rm_reset();
    (void)vibeos_rmap_add(FRAME(9),  0x1000ull, 0x8000ull);
    (void)vibeos_rmap_add(FRAME(10), 0x1000ull, 0x9000ull);
    (void)vibeos_rmap_add(FRAME(9),  0x2000ull, 0x8000ull);

    vibeos_rmap_forget_root(0x1000ull);

    CHECK(vibeos_rmap_count(FRAME(10)) == 0u, "its other frame forgotten too");
    CHECK(vibeos_rmap_count(FRAME(9)) == 1u, "the other root survives");
    {
        vibeos_rmap_holder_t got[2];
        CHECK(vibeos_rmap_holders(FRAME(9), got, 2u) == 1u, "one holder");
        CHECK(got[0].root_phys == 0x2000ull, "and it is the right one");
    }
}

/* Forgetting a frame returns its nodes to the pool. A layer that dropped the
 * list without freeing the nodes would work perfectly until the pool ran out,
 * which is the kind of leak that only appears on a long-lived machine. */
static void test_nodes_returned(void) {
    uint32_t i;

    rm_reset();
    for (i = 0; i < 8u; i++) {
        (void)vibeos_rmap_add(FRAME(11), 0x1000ull + i * 0x1000ull, 0x8000ull);
    }
    CHECK(vibeos_rmap_stats()->nodes_used == 8u, "eight in use");
    vibeos_rmap_forget_frame(FRAME(11));
    CHECK(vibeos_rmap_count(FRAME(11)) == 0u, "list empty");
    CHECK(vibeos_rmap_stats()->nodes_used == 0u, "nodes returned to the pool");
}

/* An exhausted pool reports itself and keeps working for everything already
 * recorded. It must not corrupt the lists it has, because degrading reclaim is
 * survivable and losing a mapping is not. */
static void test_exhaustion_is_reported(void) {
    /* More attempts than the pool can hold whatever the node size is: even at
     * the smallest plausible node the pool holds fewer than this. */
    const uint32_t attempts = RM_POOL_BYTES / 8u;
    uint32_t i;
    uint32_t added = 0;

    rm_reset();
    for (i = 0; i < attempts; i++) {
        if (vibeos_rmap_add(FRAME(12), 0x1000ull + (uint64_t)i * 0x1000ull,
                            0x8000ull) == 0) {
            added++;
        }
    }
    CHECK(vibeos_rmap_stats()->exhausted > 0u, "exhaustion reported");
    CHECK(vibeos_rmap_count(FRAME(12)) == added,
          "every add that claimed to succeed is on the list");
    CHECK(added < attempts, "and some genuinely failed");
}

/* An address outside the frame range is refused rather than indexing off the
 * end of the table. */
static void test_out_of_range(void) {
    rm_reset();
    CHECK(vibeos_rmap_add(RM_BASE - 4096ull, 0x1000ull, 0x8000ull) != 0,
          "below the range");
    CHECK(vibeos_rmap_add(FRAME(RM_FRAMES), 0x1000ull, 0x8000ull) != 0,
          "above the range");
    CHECK(vibeos_rmap_count(RM_BASE - 4096ull) == 0u, "count of nothing");
}

int test_rmap(void) {
    g_fail = 0;

    test_two_holders();
    test_duplicate_add();
    test_page_aligned();
    test_remove_middle();
    test_missing_remove_counted();
    test_forget_root();
    test_nodes_returned();
    test_exhaustion_is_reported();
    test_out_of_range();

    if (g_fail == 0) {
        printf("  rmap: 9 groups ok\n");
    }
    return g_fail == 0 ? 0 : 1;
}
