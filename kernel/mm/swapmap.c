/* L3b: the swap map. See include/vibeos/swapmap.h for what it guarantees.
 *
 * A bitmap and a cursor. Deliberately not a free list: a list stores its links
 * in the thing it is tracking, and this is tracking space on a disk, so the
 * links would have to live somewhere else anyway. A bitmap over a few thousand
 * slots is one cache line's worth of scanning in the worst case and it can be
 * checked against itself - "is this slot allocated" is answerable without
 * walking anything, which is what makes the refusals below cheap enough to
 * keep on every operation rather than only in debug builds.
 */

#include "vibeos/swapmap.h"

static uint8_t *g_bitmap;
static uint32_t g_slots;
static uint32_t g_cursor;          /* where the last search stopped */
static vibeos_swap_io_fn g_io;
static void *g_ctx;
static int g_ready;

static vibeos_swap_stats_t g_stats;

vibeos_swap_stats_t *vibeos_swap_stats(void) {
    return &g_stats;
}

static void (*g_lock)(void);
static void (*g_unlock)(void);

void vibeos_swapmap_set_lock(void (*lock)(void), void (*unlock)(void)) {
    g_lock = lock;
    g_unlock = unlock;
}

static void sm_lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void sm_unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

static int bit_test(uint32_t slot) {
    return (g_bitmap[slot >> 3] >> (slot & 7u)) & 1u;
}

static void bit_set(uint32_t slot) {
    g_bitmap[slot >> 3] |= (uint8_t)(1u << (slot & 7u));
}

static void bit_clear(uint32_t slot) {
    g_bitmap[slot >> 3] &= (uint8_t)~(1u << (slot & 7u));
}

int vibeos_swapmap_init(uint8_t *bitmap, uint32_t slots,
                        vibeos_swap_io_fn io, void *ctx) {
    uint32_t i;

    if (!bitmap || slots == 0u || !io) {
        return -1;
    }
    g_bitmap = bitmap;
    g_slots = slots;
    g_io = io;
    g_ctx = ctx;
    g_cursor = 0;
    for (i = 0; i < (slots + 7u) / 8u; i++) {
        g_bitmap[i] = 0;
    }
    g_stats.allocated = 0;
    g_stats.peak = 0;
    g_stats.full = 0;
    g_stats.double_free = 0;
    g_stats.bad_slot = 0;
    g_stats.unallocated_io = 0;
    g_stats.io_errors = 0;
    g_stats.writes = 0;
    g_stats.reads = 0;
    g_ready = 1;
    return 0;
}

uint32_t vibeos_swap_slots(void) {
    return g_ready ? g_slots : 0u;
}

int vibeos_swap_alloc(uint32_t *out_slot) {
    uint32_t i;
    int rc = -1;

    if (!out_slot) {
        return -1;
    }
    sm_lock();
    if (!g_ready) {
        sm_unlock();
        return -1;
    }
    /* From where the last search stopped, once round.
     *
     * A cursor rather than always from zero: with a full-from-the-front area
     * the scan would be O(allocated) on every allocation, and page-out is on
     * the path that runs when memory is short. Once round and no further, so
     * "full" is answered rather than spun on. */
    for (i = 0; i < g_slots; i++) {
        uint32_t s = g_cursor;

        g_cursor = (g_cursor + 1u) % g_slots;
        if (!bit_test(s)) {
            bit_set(s);
            g_stats.allocated++;
            if (g_stats.allocated > g_stats.peak) {
                g_stats.peak = g_stats.allocated;
            }
            *out_slot = s;
            rc = 0;
            break;
        }
    }
    if (rc != 0) {
        /* Full is a condition, not a failure: the caller decides whether to
         * give up on evicting this page or try something else. Counted so a
         * machine that is quietly thrashing against a small swap area can be
         * seen to be doing it. */
        g_stats.full++;
    }
    sm_unlock();
    return rc;
}

int vibeos_swap_free(uint32_t slot) {
    int rc = 0;

    sm_lock();
    if (!g_ready || slot >= g_slots) {
        g_stats.bad_slot++;
        sm_unlock();
        return -1;
    }
    if (!bit_test(slot)) {
        /* Two things believe they own this page of swap, and without this
         * neither would find out: the second write simply wins. Counted and
         * refused. */
        g_stats.double_free++;
        rc = -1;
    } else {
        bit_clear(slot);
        if (g_stats.allocated > 0u) {
            g_stats.allocated--;
        }
    }
    sm_unlock();
    return rc;
}

int vibeos_swap_is_allocated(uint32_t slot) {
    int set;

    sm_lock();
    set = (g_ready && slot < g_slots) ? bit_test(slot) : 0;
    sm_unlock();
    return set;
}

/* One path for both directions, because the checks are the same ones and
 * writing them twice is how the read side comes to be missing the one the
 * write side has. */
static int swap_io(uint32_t slot, void *page, int write) {
    int rc;

    if (!page) {
        return -1;
    }
    sm_lock();
    if (!g_ready || slot >= g_slots) {
        g_stats.bad_slot++;
        sm_unlock();
        return -1;
    }
    if (!bit_test(slot)) {
        /* Reading an unallocated slot would return whatever the last tenant
         * left there, which is the disclosure this layer exists to prevent;
         * writing one would put a page somewhere the map believes is free and
         * will hand out again. */
        g_stats.unallocated_io++;
        sm_unlock();
        return -1;
    }
    sm_unlock();

    /* Outside the lock: this is a device transfer and may be slow, and every
     * other layer here has learned the same lesson about holding a lock across
     * one. The slot is allocated and stays allocated - nothing frees a slot it
     * is in the middle of using - so the bit does not need to be held. */
    rc = g_io(g_ctx, slot, page, write);

    sm_lock();
    if (rc != 0) {
        g_stats.io_errors++;
    } else if (write) {
        g_stats.writes++;
    } else {
        g_stats.reads++;
    }
    sm_unlock();
    return rc;
}

int vibeos_swap_write(uint32_t slot, void *page) {
    return swap_io(slot, page, 1);
}

int vibeos_swap_read(uint32_t slot, void *page) {
    return swap_io(slot, page, 0);
}
