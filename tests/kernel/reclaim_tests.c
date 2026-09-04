/* Host tests for reclaim (plan phase P6, steps 2 to 4).
 *
 * The properties here divide sharply into two kinds, and the tests are ordered
 * to say so. Most of what a reclaim policy does is a performance question: it
 * could evict badly and the machine would only be slow. Two of them are not,
 * and getting either wrong does not degrade anything - it corrupts:
 *
 *   a pinned frame must never be a candidate, because a device or a page-table
 *   walker holds its address and will keep using it;
 *
 *   the minimum must not refuse the allocations that exist to end the
 *   pressure, or the machine deadlocks at exactly the moment it needed to free
 *   something.
 *
 * Those two are tested first and hardest.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/reclaim.h"
#include "vibeos/frame.h"
#include "vibeos/mm_model.h"

int test_reclaim(void);

static int g_fail;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  reclaim: FAIL %s\n", (what));                           \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

/* Pressure is driven directly rather than through a frame allocator: the
 * policy is a pure function of the free count, and a test that had to exhaust
 * real memory to reach the minimum would be slow, flaky, and would not be able
 * to sit exactly on a boundary. */
static uint64_t g_free;

static uint64_t fake_free(void) {
    return g_free;
}

/* A clean tier that frees what it is asked for, up to a stock. */
static uint32_t g_clean_stock;
static uint32_t g_clean_calls;

static uint32_t fake_drop(uint32_t want) {
    uint32_t n = want < g_clean_stock ? want : g_clean_stock;

    g_clean_calls++;
    g_clean_stock -= n;
    return n;
}

static void rc_reset(uint64_t low, uint64_t min) {
    memset(vibeos_reclaim_stats(), 0, sizeof(vibeos_reclaim_stats_t));
    vibeos_reclaim_set_free_source(fake_free);
    vibeos_reclaim_set_clean_source(fake_drop);
    g_clean_stock = 0;
    g_clean_calls = 0;
    (void)vibeos_reclaim_set_marks(low, min);
}

/* A small frame table, only for the pinning tests: pinning is a bit on the
 * frame descriptor, so unlike the rest of this file it cannot be tested
 * against a stub. */
#define RC_FRAMES 32u
#define RC_BASE   0x500000ull
#define RC_FRAME(n) (RC_BASE + (uint64_t)(n) * 4096ull)

static unsigned char g_ram[RC_FRAMES * 4096u];
static vibeos_frame_t g_ftable[RC_FRAMES];

static void *rc_map(uint64_t phys) {
    if (phys < RC_BASE || phys >= RC_BASE + (uint64_t)RC_FRAMES * 4096ull) {
        return 0;
    }
    return g_ram + (phys - RC_BASE);
}

static int frames_ready(void) {
    return vibeos_frame_init(RC_BASE, (uint64_t)RC_FRAMES * 4096ull,
                             g_ftable, RC_FRAMES, rc_map) == 0;
}

/* --- the two safety properties ------------------------------------------- */

/* Pinning: the property that does not degrade when it is wrong.
 *
 * An eviction that reaches a page table, a DMA buffer, or a frame a device
 * holds the address of does not make the machine slow - it corrupts it, and
 * asynchronously, which is the hardest kind of defect this project has. So the
 * bit has to survive being set, be visible to the check reclaim actually uses,
 * and come back off again; a pin that could not be released would be a leak
 * that no counter would ever report as one.
 */
static void test_pinning(void) {
    if (!frames_ready()) {
        printf("  reclaim: FAIL frame table for pinning\n");
        g_fail++;
        return;
    }

    CHECK(vibeos_reclaim_is_pinned(RC_FRAME(1)) == 0, "starts unpinned");

    vibeos_reclaim_pin(RC_FRAME(1));
    CHECK(vibeos_reclaim_is_pinned(RC_FRAME(1)) == 1, "pin is visible");
    CHECK(vibeos_reclaim_is_pinned(RC_FRAME(2)) == 0, "and only on that frame");

    vibeos_reclaim_unpin(RC_FRAME(1));
    CHECK(vibeos_reclaim_is_pinned(RC_FRAME(1)) == 0, "unpin releases it");

    /* An address outside the table must answer "not pinned" rather than
     * indexing off the end of it - a reclaim scan that walked past the table
     * would read whatever followed and evict on the strength of it. */
    CHECK(vibeos_reclaim_is_pinned(RC_BASE - 4096ull) == 0, "below the table");
    CHECK(vibeos_reclaim_is_pinned(RC_FRAME(RC_FRAMES)) == 0, "above the table");

    /* Pinning something outside the table must not corrupt anything inside
     * it. The first frame is checked afterwards because index clamping - a
     * plausible way to write frame_index - would land exactly there. */
    vibeos_reclaim_pin(RC_BASE - 4096ull);
    vibeos_reclaim_pin(RC_FRAME(RC_FRAMES));
    CHECK(vibeos_reclaim_is_pinned(RC_FRAME(0)) == 0,
          "an out-of-range pin did not land on frame zero");
}

/* The reserve exists so that the work which ends the pressure can still
 * allocate. A minimum that refuses everybody is a machine that stops rather
 * than one that recovers. */
static void test_reserve_admits_privileged(void) {
    rc_reset(100, 10);

    g_free = 5;   /* below the minimum */
    CHECK(vibeos_reclaim_pressure() == VIBEOS_MEM_CRITICAL, "critical below min");
    CHECK(vibeos_reclaim_admit(0) == 0, "ordinary allocation refused");
    CHECK(vibeos_reclaim_admit(1) == 1, "privileged allocation still admitted");
    CHECK(vibeos_reclaim_stats()->admit_refused == 1u, "the refusal is counted");

    /* And the privileged path must not have been counted as a refusal - a
     * counter that moves for the allocations that are allowed makes the number
     * useless for spotting the ones that are not. */
    CHECK(vibeos_reclaim_stats()->admit_refused == 1u, "privileged not counted");
}

/* A minimum at or above the low mark would make every allocation critical.
 * Refused rather than clamped: silently moving somebody's number turns a
 * configuration mistake into a mystery. */
static void test_marks_must_be_ordered(void) {
    CHECK(vibeos_reclaim_set_marks(100, 100) != 0, "min == low refused");
    CHECK(vibeos_reclaim_set_marks(100, 200) != 0, "min > low refused");
    CHECK(vibeos_reclaim_set_marks(100, 10) == 0, "a sane pair is accepted");
}

/* --- the boundaries ------------------------------------------------------- */

/* Exactly on a mark counts as being at that level. Off-by-one here means the
 * minimum is not the minimum, and the reserve is one frame smaller than
 * whoever set it believes. */
static void test_boundaries(void) {
    rc_reset(100, 10);

    g_free = 101; CHECK(vibeos_reclaim_pressure() == VIBEOS_MEM_OK,       "above low is ok");
    g_free = 100; CHECK(vibeos_reclaim_pressure() == VIBEOS_MEM_LOW,      "on low is low");
    g_free = 11;  CHECK(vibeos_reclaim_pressure() == VIBEOS_MEM_LOW,      "just above min is low");
    g_free = 10;  CHECK(vibeos_reclaim_pressure() == VIBEOS_MEM_CRITICAL, "on min is critical");
    g_free = 0;   CHECK(vibeos_reclaim_pressure() == VIBEOS_MEM_CRITICAL, "empty is critical");
}

/* A transition is counted once, not once per question asked. A counter that
 * ticks on every query measures how chatty the caller is. */
static void test_transitions_counted_once(void) {
    rc_reset(100, 10);

    g_free = 50;
    (void)vibeos_reclaim_pressure();
    (void)vibeos_reclaim_pressure();
    (void)vibeos_reclaim_pressure();
    CHECK(vibeos_reclaim_stats()->low_events == 1u, "one low event for three asks");

    g_free = 5;
    (void)vibeos_reclaim_pressure();
    (void)vibeos_reclaim_pressure();
    CHECK(vibeos_reclaim_stats()->critical_events == 1u, "one critical event");

    /* Back up and down again is a second event, because it is a second time
     * the machine was in trouble. */
    g_free = 500;
    (void)vibeos_reclaim_pressure();
    g_free = 50;
    (void)vibeos_reclaim_pressure();
    CHECK(vibeos_reclaim_stats()->low_events == 2u, "returning to low counts again");
}

/* Marks that were never set mean "no policy", not "always critical". A layer
 * that defaulted to critical would refuse every allocation on a machine that
 * had simply not configured it.
 *
 * **This one has to run first**, and the reason is not a test-harness detail:
 * there is deliberately no way to un-set the marks. Being able to turn the
 * policy off at runtime is a capability nobody has asked for, and adding it so
 * that a test can reach a state would be bending the design to fit the test.
 * The unconfigured state exists exactly once, before anything configures it,
 * so that is where it is checked. The first version of this ran last, found
 * the marks another test had set, and failed - which was the test being wrong,
 * not the layer. */
static void test_unconfigured_is_ok(void) {
    vibeos_reclaim_set_free_source(fake_free);
    g_free = 0;   /* would be critical under any sane pair of marks */
    CHECK(vibeos_reclaim_pressure() == VIBEOS_MEM_OK,
          "unconfigured reports ok rather than critical");
    CHECK(vibeos_reclaim_admit(0) == 1, "and admits");
}

/* --- the scan ------------------------------------------------------------- */

/* What it frees is what the clean tier gave it, and it says so. */
static void test_run_reports_what_it_freed(void) {
    rc_reset(100, 10);
    g_clean_stock = 3;

    CHECK(vibeos_reclaim_run(8u) == 3u, "freed three of eight");
    CHECK(vibeos_reclaim_stats()->freed_clean == 3u, "counted as clean");
    CHECK(vibeos_reclaim_stats()->scans == 1u, "one scan");
}

/* The shortfall is counted, so "reclaim did nothing" and "reclaim had nothing
 * it was allowed to take" are different numbers. A gate that could not tell
 * them apart would be satisfied by a reclaim that had quietly stopped working.
 */
static void test_shortfall_is_counted(void) {
    rc_reset(100, 10);
    g_clean_stock = 2;

    CHECK(vibeos_reclaim_run(10u) == 2u, "freed what it could");
    CHECK(vibeos_reclaim_stats()->skipped_no_swap == 8u,
          "the eight it could not take are counted, not silent");
}

/* Asking for nothing does nothing, and does not call the tier. */
static void test_zero_is_a_noop(void) {
    rc_reset(100, 10);
    g_clean_stock = 5;

    CHECK(vibeos_reclaim_run(0u) == 0u, "nothing freed");
    CHECK(g_clean_calls == 0u, "and the tier was not asked");
    CHECK(g_clean_stock == 5u, "so it still has its pages");
}

/* With no clean source at all the run is honest rather than crashing, and the
 * whole request shows up as unreclaimable. */
static void test_no_source(void) {
    rc_reset(100, 10);
    vibeos_reclaim_set_clean_source(0);

    CHECK(vibeos_reclaim_run(4u) == 0u, "freed nothing");
    CHECK(vibeos_reclaim_stats()->skipped_no_swap == 4u, "all four counted");
}

int test_reclaim(void) {
    g_fail = 0;

    test_unconfigured_is_ok();      /* must be first; see its comment */
    test_pinning();
    test_reserve_admits_privileged();
    test_marks_must_be_ordered();
    test_boundaries();
    test_transitions_counted_once();
    test_run_reports_what_it_freed();
    test_shortfall_is_counted();
    test_zero_is_a_noop();
    test_no_source();

    if (g_fail == 0) {
        printf("  reclaim: 10 groups ok\n");
    }
    return g_fail == 0 ? 0 : 1;
}
