/* Host tests for the frame layer (plan phase P1).
 *
 * This is the point of putting L0 in the portable kernel: today's frame
 * accounting can only be exercised by booting a virtual machine and hoping the
 * interleaving comes out wrong, which is how one defect survived four fixes.
 * Everything below runs in milliseconds and covers the paths that actually
 * broke - a release of something the table does not describe, a release of
 * something already free, and a poison that nobody checked.
 *
 * Each test asserts the refusal as well as the success, because in this
 * subsystem the refusals are the safety property.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/frame.h"
#include "vibeos/mm_stats.h"

int test_frame(void);

#define TEST_FRAMES 64u
#define TEST_BASE   0x100000ull

static unsigned char *g_ram;

static void *test_map(uint64_t phys) {
    if (phys < TEST_BASE || phys >= TEST_BASE + (uint64_t)TEST_FRAMES * 4096ull) {
        return 0;
    }
    return g_ram + (phys - TEST_BASE);
}

static vibeos_frame_t g_table[TEST_FRAMES];

static int setup(void) {
    memset(g_table, 0, sizeof(g_table));
    vibeos_mm_stats_reset();
    return vibeos_frame_init(TEST_BASE, (uint64_t)TEST_FRAMES * 4096ull,
                             g_table, TEST_FRAMES, test_map);
}

int test_frame(void) {
    uint64_t a, b, c;
    uint64_t i;

    g_ram = (unsigned char *)malloc((size_t)TEST_FRAMES * 4096u);
    if (!g_ram) {
        return -1;
    }

    /* ---- init: everything free, and counted ---------------------------- */
    if (setup() != 0) { goto fail; }
    if (vibeos_frame_total() != TEST_FRAMES) { goto fail; }
    if (vibeos_frame_free_count() != TEST_FRAMES) { goto fail; }
    if (vibeos_mm_stats()->frames_free != TEST_FRAMES) { goto fail; }

    /* ---- alloc: one owner, zeroed, and out of the free list ------------ */
    a = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (a == 0ull) { goto fail; }
    if (vibeos_frame_owners(a) != 1u) { goto fail; }
    if (vibeos_frame_state(a) != VIBEOS_FRAME_ALLOCATED) { goto fail; }
    if (vibeos_frame_free_count() != TEST_FRAMES - 1u) { goto fail; }
    for (i = 0; i < 4096ull; i++) {
        if (((unsigned char *)test_map(a))[i] != 0u) {
            printf("FAIL:frame handed out unzeroed at %llu\n",
                   (unsigned long long)i);
            goto fail;
        }
    }

    /* ---- get/put: freed only at zero (I1) ------------------------------ */
    vibeos_frame_get(a);
    if (vibeos_frame_owners(a) != 2u) { goto fail; }
    if (vibeos_frame_put(a) != 0) { goto fail; }        /* still one owner */
    if (vibeos_frame_free_count() != TEST_FRAMES - 1u) { goto fail; }
    if (vibeos_frame_put(a) != 1) { goto fail; }        /* now it goes back  */
    if (vibeos_frame_free_count() != TEST_FRAMES) { goto fail; }

    /* ...and it came back poisoned, all of it. The old free list wrote a
     * pointer into the first word, so this could not be asserted at offset 0. */
    {
        const uint64_t *w = (const uint64_t *)test_map(a);
        if (w[0] != 0xDEAD0000DEAD0000ull ||
            w[511] != 0xDEAD0000DEAD0000ull) {
            printf("FAIL:frame not poisoned end to end\n");
            goto fail;
        }
    }

    /* ---- a frame the table does not describe is never freed (I2) ------- */
    if (setup() != 0) { goto fail; }
    if (vibeos_frame_put(TEST_BASE - 4096ull) != 0) { goto fail; }
    if (vibeos_frame_put(TEST_BASE + (uint64_t)TEST_FRAMES * 4096ull) != 0) { goto fail; }
    if (vibeos_mm_stats()->frames_leaked != 2ull) { goto fail; }
    if (vibeos_frame_free_count() != TEST_FRAMES) { goto fail; }

    /* ---- releasing something with no owners is refused and counted ----- */
    b = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (b == 0ull) { goto fail; }
    if (vibeos_frame_put(b) != 1) { goto fail; }
    if (vibeos_frame_put(b) != 0) { goto fail; }         /* the second is a bug */
    if (vibeos_mm_stats()->frames_double_put != 1ull) { goto fail; }

    /* ---- a write to a free frame is noticed when it is handed out ------ */
    if (setup() != 0) { goto fail; }
    c = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (c == 0ull) { goto fail; }
    if (vibeos_frame_put(c) != 1) { goto fail; }
    ((unsigned char *)test_map(c))[2048] = 0x42u;         /* somebody's stale write */
    /* It goes back to the head of the list, so the next allocation is the same
     * frame - which is exactly the case that used to corrupt silently. */
    if (vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED) != c) { goto fail; }
    if (vibeos_mm_stats()->poison_hits != 1ull) {
        printf("FAIL:write to a freed frame went unnoticed\n");
        goto fail;
    }

    /* ---- a frame that has never been freed is not judged by the poison ---
     *
     * The layer cannot poison at init: when the kernel brings it up, part of the
     * region already holds live page tables and the descriptor table itself, and
     * filling it would destroy them. So a never-freed frame holds whatever was
     * there before, and reporting that as corruption would make the detector cry
     * wolf on every boot - which is how a detector stops being read. */
    if (setup() != 0) { goto fail; }
    memset(g_ram, 0x5A, (size_t)TEST_FRAMES * 4096u);
    if (vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED) == 0ull) { goto fail; }
    if (vibeos_mm_stats()->poison_hits != 0ull) {
        printf("FAIL:untouched frame reported as corrupted\n");
        goto fail;
    }

    /* ---- ...and the same, from a descriptor table full of garbage ------
     *
     * The case above was correct about the property and could not see the
     * defect, because setup() memsets the table first. The kernel does not: it
     * hands this layer a slab from the bump allocator, contents unknown. Every
     * descriptor whose stale flags byte happened to have the was-freed bit set
     * came up claiming a release that never happened, and the poison check then
     * read the frame's virgin contents as corruption - 3019 reported
     * use-after-frees in a boot with none.
     *
     * It hid for months behind something that looks unrelated: the size of the
     * exec staging buffers, which decides where in physical memory this table
     * lands and therefore what garbage is in it. Shrinking them made a
     * "memory-layout-sensitive corruption" appear on demand, and it was this.
     *
     * 0xFF rather than a chosen value, so the test does not encode which bit
     * the layer happens to use. */
    memset(g_table, 0xFF, sizeof(g_table));
    vibeos_mm_stats_reset();
    if (vibeos_frame_init(TEST_BASE, (uint64_t)TEST_FRAMES * 4096ull,
                          g_table, TEST_FRAMES, test_map) != 0) { goto fail; }
    memset(g_ram, 0x5A, (size_t)TEST_FRAMES * 4096u);
    if (vibeos_frame_dirty_at_init() != TEST_FRAMES) {
        printf("FAIL:dirty descriptors not counted\n");
        goto fail;
    }
    if (vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED) == 0ull) { goto fail; }
    if (vibeos_mm_stats()->poison_hits != 0ull) {
        printf("FAIL:stale descriptor flags read as corruption\n");
        goto fail;
    }
    /* The rest of the descriptor has to be sound too, or the failure just moves:
     * a stale owner count is a frame handed out to a second holder. */
    if (vibeos_mm_stats()->double_allocs != 0ull) {
        printf("FAIL:stale descriptor owners read as a double allocation\n");
        goto fail;
    }
    if (vibeos_frame_free_count() != TEST_FRAMES - 1u) { goto fail; }

    /* ---- reserve: takes a range out, and refuses after the first alloc - */
    if (setup() != 0) { goto fail; }
    if (vibeos_frame_reserve(TEST_BASE, 4096ull * 4ull) != 0) { goto fail; }
    if (vibeos_frame_free_count() != TEST_FRAMES - 4u) { goto fail; }
    if (vibeos_frame_state(TEST_BASE) != VIBEOS_FRAME_RESERVED) { goto fail; }
    /* Reserved frames never come back from alloc. */
    for (i = 0; i < TEST_FRAMES - 4u; i++) {
        uint64_t got = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
        if (got == 0ull) { goto fail; }
        if (got < TEST_BASE + 4096ull * 4ull) {
            printf("FAIL:reserved frame handed out\n");
            goto fail;
        }
    }
    /* ...and now there is nothing left, which must change nothing (I5). */
    if (vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED) != 0ull) { goto fail; }
    if (vibeos_frame_free_count() != 0ull) { goto fail; }
    /* Reserving after an allocation is refused rather than half-applied. */
    if (vibeos_frame_reserve(TEST_BASE + 4096ull * 8ull, 4096ull) == 0) { goto fail; }

    /* ---- contiguous allocation ----------------------------------------
     *
     * The case that matters is not the happy one: it is that a run broken by a
     * reserved frame is not handed out anyway. Two allocators disagreeing about
     * what is free is the whole defect this layer exists to end, and a
     * contiguous allocator that walks over a reservation is exactly that. */
    if (setup() != 0) { goto fail; }
    if (vibeos_frame_reserve(TEST_BASE + 4096ull * 2ull, 4096ull) != 0) { goto fail; }
    /* Frames 0 and 1 are free, 2 is reserved, 3.. are free. A run of four must
     * therefore start at 3, not at 0. */
    if (vibeos_frame_alloc_contig(4u, VIBEOS_FRAME_ALLOCATED)
        != TEST_BASE + 4096ull * 3ull) { goto fail; }
    if (vibeos_frame_owners(TEST_BASE + 4096ull * 6ull) != 1u) { goto fail; }
    if (vibeos_frame_free_count() != TEST_FRAMES - 5u) { goto fail; }
    /* Each frame of the run is independently owned, so releasing one releases
     * one - a contiguous allocation is not a unit that comes back together. */
    if (vibeos_frame_put(TEST_BASE + 4096ull * 3ull) != 1) { goto fail; }
    if (vibeos_frame_free_count() != TEST_FRAMES - 4u) { goto fail; }
    /* A run longer than anything free changes nothing (I5). */
    if (vibeos_frame_alloc_contig(TEST_FRAMES, VIBEOS_FRAME_ALLOCATED) != 0ull) { goto fail; }
    if (vibeos_frame_free_count() != TEST_FRAMES - 4u) { goto fail; }
    /* ...and the frames it walked past are still free and still allocatable,
     * which is the half of "changes nothing" that a partial scan breaks. */
    if (vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED) == 0ull) { goto fail; }

    free(g_ram);
    g_ram = 0;
    return 0;

fail:
    free(g_ram);
    g_ram = 0;
    return -1;
}
