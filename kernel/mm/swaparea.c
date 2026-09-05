/* Where swap lives. See include/vibeos/swaparea.h for the property this exists
 * to guarantee.
 *
 * Small, and it has to stay small: it is the only code that turns a slot number
 * into a block address, and every line of it is between a page of somebody's
 * memory and a filesystem on the same disk.
 */

#include "vibeos/swaparea.h"
#include "vibeos/swapmap.h"

static vibeos_swap_area_t g_area;
static vibeos_swap_block_fn g_block;
static void *g_ctx;
static uint32_t g_slots;

static vibeos_swaparea_stats_t g_stats;

vibeos_swaparea_stats_t *vibeos_swaparea_stats(void) {
    return &g_stats;
}

uint32_t vibeos_swaparea_slots(void) {
    return g_slots;
}

uint64_t vibeos_swaparea_slot_sector(uint32_t slot) {
    if (g_slots == 0u || slot >= g_slots) {
        return 0;
    }
    return g_area.first_sector +
           (uint64_t)slot * (uint64_t)VIBEOS_SWAP_SECTORS_PER_SLOT;
}

/* The transfer, and the bound.
 *
 * The check is `slot >= g_slots` rather than an arithmetic comparison against
 * the end sector, and that is deliberate: the slot count was computed once, by
 * division, at configuration time, and comparing against it cannot disagree
 * with the division. Recomputing the end here would be a second piece of
 * arithmetic that has to match the first, which is exactly how two structures
 * come to disagree about the same fact.
 */
static int area_io(void *ctx, uint32_t slot, void *page, int write) {
    uint64_t lba;
    int rc;

    (void)ctx;
    if (!g_block || g_slots == 0u) {
        return -1;
    }
    if (slot >= g_slots) {
        /* Counted and refused before the device sees it. A driver would accept
         * this address quite happily - the filesystem's blocks are perfectly
         * valid LBAs - so this is the only place it can be stopped. */
        g_stats.out_of_range++;
        return -1;
    }
    lba = g_area.first_sector +
          (uint64_t)slot * (uint64_t)VIBEOS_SWAP_SECTORS_PER_SLOT;

    rc = g_block(g_ctx, g_area.device, lba,
                 (uint32_t)VIBEOS_SWAP_SECTORS_PER_SLOT, page, write);
    if (rc == 0) {
        if (write) {
            g_stats.sectors_written += VIBEOS_SWAP_SECTORS_PER_SLOT;
        } else {
            g_stats.sectors_read += VIBEOS_SWAP_SECTORS_PER_SLOT;
        }
    }
    return rc;
}

uint32_t vibeos_swaparea_configure(const vibeos_swap_area_t *area,
                                   vibeos_swap_block_fn block, void *ctx,
                                   uint8_t *bitmap, uint32_t bitmap_bytes) {
    uint64_t slots;

    g_slots = 0;
    g_block = 0;

    if (!area || !block || !bitmap) {
        g_stats.refused_no_area++;
        return 0;
    }
    if (area->kind == VIBEOS_SWAP_NONE) {
        /* Not a mistake. A kernel with no swap area is the normal case, and
         * this counter separates it from an area that was offered and
         * rejected - only one of those is somebody's error. */
        g_stats.refused_no_area++;
        return 0;
    }
    if (!area->contiguous) {
        /* A file whose blocks are not one run. Writing a page across the gap
         * would put it into whatever lies between the extents, which on a
         * filesystem is somebody's data. Refused rather than approximated. */
        g_stats.refused_fragmented++;
        return 0;
    }

    slots = area->sectors / (uint64_t)VIBEOS_SWAP_SECTORS_PER_SLOT;
    if (slots == 0ull) {
        g_stats.refused_too_small++;
        return 0;
    }
    /* Whatever is left over at the end is not used. A partial slot cannot hold
     * a page, and rounding up would put half of one outside the area. */
    if (slots > 0xFFFFFFFFull) {
        slots = 0xFFFFFFFFull;
    }
    if ((uint64_t)bitmap_bytes * 8ull < slots) {
        /* The caller sized its bitmap for fewer slots than the area holds.
         * Refused rather than silently using the smaller number: a swap map
         * that believes it has fewer slots than the area is harmless, but the
         * asymmetry is the kind that gets "fixed" later by trusting whichever
         * number is bigger. */
        g_stats.refused_no_bitmap++;
        return 0;
    }

    g_area = *area;
    g_block = block;
    g_ctx = ctx;
    g_slots = (uint32_t)slots;

    if (vibeos_swapmap_init(bitmap, g_slots, area_io, 0) != 0) {
        g_slots = 0;
        g_block = 0;
        return 0;
    }
    return g_slots;
}
