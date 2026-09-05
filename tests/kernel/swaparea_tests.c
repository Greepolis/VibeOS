/* Host tests for the swap area — where swap lives.
 *
 * One property matters more than everything else here and the tests are
 * weighted for it: **a transfer never touches a block outside the area.**
 *
 * Swap sits on the same device as the filesystem. An area that miscomputes its
 * bounds does not fail — it writes a page of somebody's memory over a
 * directory, and the damage is found when the machine next boots. A driver
 * cannot catch that: the filesystem's blocks are perfectly valid addresses, so
 * this layer is the only place it can be stopped.
 *
 * The fake device below therefore records every LBA it is asked for, and the
 * tests assert on the range rather than on the return value.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/swaparea.h"
#include "vibeos/swapmap.h"

int test_swaparea(void);

#define SA_SECTORS   256u          /* the whole fake disk                    */
#define SA_AREA_FIRST 64u          /* where swap is allowed to live          */
#define SA_AREA_LEN   128u         /* 128 sectors = 16 slots                 */

static unsigned char g_disk[SA_SECTORS][VIBEOS_SWAP_SECTOR_BYTES];
static uint8_t g_bits[64];

/* Every sector the layer asked for, so a transfer outside the area is caught
 * by the test rather than only by the layer's own counter. */
static uint64_t g_lo_touched;
static uint64_t g_hi_touched;
static uint32_t g_calls;

static int g_fail;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            printf("  swaparea: FAIL %s\n", (what));                          \
            g_fail++;                                                         \
        }                                                                     \
    } while (0)

static int fake_block(void *ctx, uint32_t device, uint64_t lba, uint32_t count,
                      void *buf, int write) {
    uint32_t i;

    (void)ctx; (void)device;
    g_calls++;
    if (lba < g_lo_touched) { g_lo_touched = lba; }
    if (lba + count > g_hi_touched) { g_hi_touched = lba + count; }

    if (lba + count > SA_SECTORS) {
        return -1;      /* off the end of the disk entirely */
    }
    for (i = 0; i < count; i++) {
        unsigned char *d = g_disk[lba + i];
        unsigned char *p = (unsigned char *)buf + i * VIBEOS_SWAP_SECTOR_BYTES;
        if (write) {
            memcpy(d, p, VIBEOS_SWAP_SECTOR_BYTES);
        } else {
            memcpy(p, d, VIBEOS_SWAP_SECTOR_BYTES);
        }
    }
    return 0;
}

static void sa_reset(void) {
    memset(g_disk, 0xA7, sizeof(g_disk));   /* recognisable filesystem data */
    memset(g_bits, 0, sizeof(g_bits));
    memset(vibeos_swaparea_stats(), 0, sizeof(vibeos_swaparea_stats_t));
    g_lo_touched = 0xFFFFFFFFFFFFFFFFull;
    g_hi_touched = 0;
    g_calls = 0;
}

static vibeos_swap_area_t partition_area(void) {
    vibeos_swap_area_t a;

    memset(&a, 0, sizeof(a));
    a.kind = VIBEOS_SWAP_PARTITION;
    a.device = 0;
    a.first_sector = SA_AREA_FIRST;
    a.sectors = SA_AREA_LEN;
    a.contiguous = 1;
    a.origin = "test partition";
    return a;
}

/* --- the safety property -------------------------------------------------- */

/* Using every slot touches only the area, and touches all of it.
 *
 * Both halves matter. Staying inside is the safety property; covering the
 * whole area is what catches a translation that is *too* conservative and
 * quietly wastes half the swap - a defect that looks like working swap until
 * the machine runs out earlier than it should.
 */
static void test_every_slot_stays_inside(void) {
    vibeos_swap_area_t a = partition_area();
    uint32_t slots, i;
    unsigned char page[VIBEOS_SWAP_SLOT_BYTES];

    sa_reset();
    slots = vibeos_swaparea_configure(&a, fake_block, 0, g_bits,
                                      (uint32_t)sizeof(g_bits));
    CHECK(slots == SA_AREA_LEN / VIBEOS_SWAP_SECTORS_PER_SLOT,
          "the area holds the slots its length allows");

    for (i = 0; i < slots; i++) {
        uint32_t slot;
        CHECK(vibeos_swap_alloc(&slot) == 0, "a slot");
        memset(page, (int)(i + 1u), sizeof(page));
        CHECK(vibeos_swap_write(slot, page) == 0, "written");
    }

    CHECK(g_lo_touched == SA_AREA_FIRST, "nothing below the area was touched");
    CHECK(g_hi_touched == SA_AREA_FIRST + SA_AREA_LEN,
          "nothing above it either, and all of it was used");

    /* And the rest of the disk is exactly as it was. This is the assertion
     * that would have caught the defect this layer exists to prevent. */
    {
        uint32_t s;
        int intact = 1;
        for (s = 0; s < SA_SECTORS; s++) {
            if (s >= SA_AREA_FIRST && s < SA_AREA_FIRST + SA_AREA_LEN) {
                continue;
            }
            if (g_disk[s][0] != 0xA7) {
                intact = 0;
                break;
            }
        }
        CHECK(intact, "every sector outside the area is untouched");
    }
}

/* A slot past the end is refused before the device sees it. */
static void test_slot_past_the_end_is_refused(void) {
    vibeos_swap_area_t a = partition_area();
    uint32_t slots;
    unsigned char page[VIBEOS_SWAP_SLOT_BYTES];

    sa_reset();
    slots = vibeos_swaparea_configure(&a, fake_block, 0, g_bits,
                                      (uint32_t)sizeof(g_bits));
    memset(page, 0x5C, sizeof(page));

    /* The swap map refuses an out-of-range slot on its own, so reach past it
     * and ask the area directly - which is the layer that has to be right when
     * something one level up is not. */
    CHECK(vibeos_swaparea_slot_sector(slots) == 0ull,
          "a slot past the end has no sector");
    CHECK(vibeos_swaparea_slot_sector(slots - 1u) ==
              SA_AREA_FIRST + (uint64_t)(slots - 1u) *
                  VIBEOS_SWAP_SECTORS_PER_SLOT,
          "and the last one is where it should be");
    CHECK(g_calls == 0u, "no transfer was issued for either question");
}

/* Slot zero is at the start of the area, not at the start of the disk.
 *
 * An off-by-a-base here is the whole defect in one line: every page written
 * would land in the first sectors of the device, which on a real machine is
 * the partition table and the boot sector.
 */
static void test_slot_zero_is_not_sector_zero(void) {
    vibeos_swap_area_t a = partition_area();

    sa_reset();
    (void)vibeos_swaparea_configure(&a, fake_block, 0, g_bits,
                                    (uint32_t)sizeof(g_bits));
    CHECK(vibeos_swaparea_slot_sector(0u) == SA_AREA_FIRST,
          "slot zero starts where the area does");
}

/* --- the refusals --------------------------------------------------------- */

/* A fragmented file is refused. Writing across the gap would put a page into
 * whatever lies between the extents, which on a filesystem is somebody's data.
 */
static void test_fragmented_file_is_refused(void) {
    vibeos_swap_area_t a = partition_area();

    sa_reset();
    a.kind = VIBEOS_SWAP_FILE;
    a.contiguous = 0;
    CHECK(vibeos_swaparea_configure(&a, fake_block, 0, g_bits,
                                    (uint32_t)sizeof(g_bits)) == 0u,
          "refused");
    CHECK(vibeos_swaparea_stats()->refused_fragmented == 1u, "counted");
    CHECK(vibeos_swaparea_slots() == 0u, "and nothing is configured");
}

/* A contiguous file is accepted, and translates exactly like a partition. The
 * two kinds differ in how they are obtained, not in how they are addressed. */
static void test_contiguous_file_is_accepted(void) {
    vibeos_swap_area_t a = partition_area();

    sa_reset();
    a.kind = VIBEOS_SWAP_FILE;
    a.contiguous = 1;
    CHECK(vibeos_swaparea_configure(&a, fake_block, 0, g_bits,
                                    (uint32_t)sizeof(g_bits)) > 0u, "accepted");
    CHECK(vibeos_swaparea_slot_sector(0u) == SA_AREA_FIRST,
          "and addressed the same way");
}

/* No area is a state, not a failure, and it is counted separately from an area
 * that was offered and rejected. */
static void test_no_area(void) {
    vibeos_swap_area_t a;

    sa_reset();
    memset(&a, 0, sizeof(a));
    a.kind = VIBEOS_SWAP_NONE;
    CHECK(vibeos_swaparea_configure(&a, fake_block, 0, g_bits,
                                    (uint32_t)sizeof(g_bits)) == 0u,
          "nothing configured");
    CHECK(vibeos_swaparea_stats()->refused_no_area == 1u,
          "counted as absent rather than as rejected");
    CHECK(vibeos_swaparea_stats()->refused_too_small == 0u, "and not as small");
}

/* An area too short for one slot, and a partial slot at the end.
 *
 * The leftover is dropped rather than rounded up: half a slot outside the area
 * is the same defect as a slot past the end, arrived at by arithmetic instead
 * of by an index.
 */
static void test_short_and_partial(void) {
    vibeos_swap_area_t a = partition_area();

    sa_reset();
    a.sectors = VIBEOS_SWAP_SECTORS_PER_SLOT - 1u;
    CHECK(vibeos_swaparea_configure(&a, fake_block, 0, g_bits,
                                    (uint32_t)sizeof(g_bits)) == 0u,
          "too short for even one slot");
    CHECK(vibeos_swaparea_stats()->refused_too_small == 1u, "counted");

    sa_reset();
    a = partition_area();
    a.sectors = VIBEOS_SWAP_SECTORS_PER_SLOT * 3u + 3u;   /* three and a bit */
    CHECK(vibeos_swaparea_configure(&a, fake_block, 0, g_bits,
                                    (uint32_t)sizeof(g_bits)) == 3u,
          "the partial slot at the end is dropped, not rounded up");
    CHECK(vibeos_swaparea_slot_sector(3u) == 0ull, "and is not addressable");
}

/* A bitmap too small for the area is refused rather than quietly using the
 * smaller number. The asymmetry is the kind that gets "fixed" later by
 * trusting whichever value is larger. */
static void test_bitmap_too_small(void) {
    vibeos_swap_area_t a = partition_area();

    sa_reset();
    CHECK(vibeos_swaparea_configure(&a, fake_block, 0, g_bits, 1u) == 0u,
          "refused");
    CHECK(vibeos_swaparea_stats()->refused_no_bitmap == 1u, "counted");
}

int test_swaparea(void) {
    g_fail = 0;

    test_every_slot_stays_inside();
    test_slot_past_the_end_is_refused();
    test_slot_zero_is_not_sector_zero();
    test_fragmented_file_is_refused();
    test_contiguous_file_is_accepted();
    test_no_area();
    test_short_and_partial();
    test_bitmap_too_small();

    if (g_fail == 0) {
        printf("  swaparea: 8 groups ok\n");
    }
    return g_fail == 0 ? 0 : 1;
}
