/* Host tests for the swap map (plan phase P5, step 1).
 *
 * The guarantee worth testing is not that swap works - it is that a slot is
 * never in two places at once. Deciding *which* page to evict can be wrong
 * without being dangerous; deciding *where* it went cannot. A slot handed out
 * twice, or read while free, hands one process another process's memory at
 * some later fault, with nothing anywhere reporting an error.
 *
 * So the round-trip test is one group and the refusals are five.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/swapmap.h"

int test_swap(void);

#define SM_SLOTS 64u

static uint8_t g_bits[(SM_SLOTS + 7u) / 8u];

/* A device that is just memory, so a round trip can be checked byte for byte
 * without a driver. */
static unsigned char g_disk[SM_SLOTS][4096];
static int g_fail_writes;
static uint32_t g_io_calls;

static int fake_io(void *ctx, uint32_t slot, void *page, int write) {
    (void)ctx;
    g_io_calls++;
    if (slot >= SM_SLOTS) {
        return -1;
    }
    if (write) {
        if (g_fail_writes) {
            return -1;
        }
        memcpy(g_disk[slot], page, 4096);
    } else {
        memcpy(page, g_disk[slot], 4096);
    }
    return 0;
}

static int g_fail;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  swap: FAIL %s\n", (what));                              \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static void sm_reset(void) {
    memset(g_disk, 0, sizeof(g_disk));
    g_fail_writes = 0;
    g_io_calls = 0;
    if (vibeos_swapmap_init(g_bits, SM_SLOTS, fake_io, 0) != 0) {
        printf("  swap: FAIL init\n");
        g_fail++;
    }
}

/* --- it works ------------------------------------------------------------- */

/* A page written to a slot comes back byte for byte, and lands in that slot
 * rather than near it. The second half matters: an off-by-one in the slot
 * arithmetic gives a round trip that passes for a single slot and corrupts
 * every neighbour once there is more than one. */
static void test_round_trip(void) {
    uint32_t a = 0xFFFFFFFFu, b = 0xFFFFFFFFu;
    unsigned char out[4096], in[4096];

    sm_reset();
    CHECK(vibeos_swap_alloc(&a) == 0, "first slot");
    CHECK(vibeos_swap_alloc(&b) == 0, "second slot");
    CHECK(a != b, "and they are different slots");

    memset(out, 0x5E, sizeof(out));
    CHECK(vibeos_swap_write(a, out) == 0, "write a");
    memset(out, 0xC1, sizeof(out));
    CHECK(vibeos_swap_write(b, out) == 0, "write b");

    memset(in, 0, sizeof(in));
    CHECK(vibeos_swap_read(a, in) == 0, "read a");
    CHECK(in[0] == 0x5E && in[4095] == 0x5E, "a came back whole");

    memset(in, 0, sizeof(in));
    CHECK(vibeos_swap_read(b, in) == 0, "read b");
    CHECK(in[0] == 0xC1 && in[4095] == 0xC1, "b came back whole, and unmixed");
}

/* --- the refusals --------------------------------------------------------- */

/* A slot is not handed out twice. Every slot is taken, and every one is
 * distinct - checked by marking them rather than by trusting the count, since
 * a map that returned the same slot twice would still report the right
 * number of allocations. */
static void test_no_slot_twice(void) {
    unsigned char seen[SM_SLOTS];
    uint32_t i, slot;
    uint32_t got = 0;

    sm_reset();
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < SM_SLOTS; i++) {
        if (vibeos_swap_alloc(&slot) != 0) {
            break;
        }
        CHECK(slot < SM_SLOTS, "slot in range");
        CHECK(seen[slot] == 0u, "slot not handed out before");
        seen[slot] = 1;
        got++;
    }
    CHECK(got == SM_SLOTS, "every slot was available exactly once");
    CHECK(vibeos_swap_stats()->allocated == SM_SLOTS, "and all are held");
}

/* Full is a condition, not an error, and it is counted. */
static void test_full_is_reported(void) {
    uint32_t i, slot;

    sm_reset();
    for (i = 0; i < SM_SLOTS; i++) {
        (void)vibeos_swap_alloc(&slot);
    }
    CHECK(vibeos_swap_alloc(&slot) != 0, "full refuses");
    CHECK(vibeos_swap_stats()->full == 1u, "and is counted");

    /* Freeing one makes exactly one available again. */
    CHECK(vibeos_swap_free(3u) == 0, "free one");
    CHECK(vibeos_swap_alloc(&slot) == 0, "one is available");
    CHECK(slot == 3u, "and it is the one that was freed");
    CHECK(vibeos_swap_alloc(&slot) != 0, "and only one");
}

/* A double free is refused and counted: two things believing they own the same
 * page of swap is not something the second write would reveal. */
static void test_double_free(void) {
    uint32_t slot;

    sm_reset();
    CHECK(vibeos_swap_alloc(&slot) == 0, "alloc");
    CHECK(vibeos_swap_free(slot) == 0, "first free");
    CHECK(vibeos_swap_free(slot) != 0, "second free refused");
    CHECK(vibeos_swap_stats()->double_free == 1u, "and counted");
}

/* Reading or writing a slot nobody holds is refused. Reading one would return
 * the previous tenant's page. */
static void test_unallocated_io(void) {
    unsigned char page[4096];

    sm_reset();
    memset(page, 0, sizeof(page));
    CHECK(vibeos_swap_read(0u, page) != 0, "read of a free slot refused");
    CHECK(vibeos_swap_write(0u, page) != 0, "write of a free slot refused");
    CHECK(vibeos_swap_stats()->unallocated_io == 2u, "both counted");
    CHECK(g_io_calls == 0u, "and the device was never touched");
}

/* Out of range is refused before it indexes anything. */
static void test_out_of_range(void) {
    unsigned char page[4096];

    sm_reset();
    CHECK(vibeos_swap_free(SM_SLOTS) != 0, "free out of range");
    CHECK(vibeos_swap_read(SM_SLOTS, page) != 0, "read out of range");
    CHECK(vibeos_swap_write(SM_SLOTS + 100u, page) != 0, "write far out");
    CHECK(vibeos_swap_stats()->bad_slot == 3u, "all counted");
    CHECK(vibeos_swap_is_allocated(SM_SLOTS) == 0, "and not allocated");
}

/* A failed write leaves the slot allocated and says so. Freeing it would put
 * a slot back that a page still believes holds its contents; reporting success
 * would lose the page outright. */
static void test_failed_write_keeps_the_slot(void) {
    uint32_t slot;
    unsigned char page[4096];

    sm_reset();
    memset(page, 0x77, sizeof(page));
    CHECK(vibeos_swap_alloc(&slot) == 0, "alloc");

    g_fail_writes = 1;
    CHECK(vibeos_swap_write(slot, page) != 0, "the write fails");
    CHECK(vibeos_swap_stats()->io_errors == 1u, "counted as an io error");
    CHECK(vibeos_swap_is_allocated(slot) == 1, "the slot is still held");
    CHECK(vibeos_swap_stats()->writes == 0u, "and not counted as a write");
}

int test_swap(void) {
    g_fail = 0;

    test_round_trip();
    test_no_slot_twice();
    test_full_is_reported();
    test_double_free();
    test_unallocated_io();
    test_out_of_range();
    test_failed_write_keeps_the_slot();

    if (g_fail == 0) {
        printf("  swap: 7 groups ok\n");
    }
    return g_fail == 0 ? 0 : 1;
}
