/* L3: backing stores. See include/vibeos/backing.h for why.
 *
 * Two implementations behind one interface, and a page cache underneath the
 * second. Swap is P5 and is deliberately absent rather than stubbed: an
 * interface with a member nobody implements reads as a feature that exists.
 */

#include "vibeos/backing.h"
#include "vibeos/frame.h"
#include "vibeos/mm_stats.h"

/* ---- anonymous ---------------------------------------------------------- */

static int anon_fault_in(void *ctx, uint64_t offset, uint64_t *out_phys) {
    uint64_t phys;

    (void)ctx;
    (void)offset;   /* every offset of anonymous memory is the same: zeroes */
    phys = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (!phys) {
        return -1;
    }
    *out_phys = phys;
    return 0;
}

static int anon_write_back(void *ctx, uint64_t offset, uint64_t phys) {
    (void)ctx; (void)offset; (void)phys;
    /* Nowhere to write it. Anonymous memory that must survive being reclaimed
     * is what swap is for, at P5. */
    return -1;
}

static int anon_release(void *ctx, uint64_t offset) {
    (void)ctx; (void)offset;
    return 0;
}

static const vibeos_backing_ops_t g_anon_ops = {
    anon_fault_in, anon_write_back, anon_release
};

const vibeos_backing_ops_t *vibeos_backing_anon(void) {
    return &g_anon_ops;
}

/* ---- the page cache ----------------------------------------------------- */

static vibeos_cache_entry_t *g_table;
static uint32_t g_entries;
static vibeos_cache_read_fn g_read;
static void *g_read_ctx;
static uint32_t g_hand;         /* the clock hand */
static uint32_t g_resident;

void vibeos_cache_init(vibeos_cache_entry_t *table, uint32_t entries,
                       vibeos_cache_read_fn read, void *ctx) {
    uint32_t i;

    g_table = table;
    g_entries = entries;
    g_read = read;
    g_read_ctx = ctx;
    g_hand = 0;
    g_resident = 0;
    if (!table) {
        return;
    }
    for (i = 0; i < entries; i++) {
        table[i].file_id = 0;
        table[i].used = 0;
        table[i].offset = 0;
        table[i].phys = 0;
    }
}

uint32_t vibeos_cache_resident(void) {
    return g_resident;
}

/* Where a key would live. Open addressing with linear probing: the table is
 * small and fixed, and a chain would need an allocator underneath a layer whose
 * whole job is to avoid needing one. */
static uint32_t cache_slot(uint32_t file_id, uint64_t offset) {
    uint64_t h = ((uint64_t)file_id * 0x9E3779B97F4A7C15ull) ^
                 (offset >> 12) * 0xC2B2AE3D27D4EB4Full;
    return (uint32_t)((h >> 32) % g_entries);
}

static vibeos_cache_entry_t *cache_lookup(uint32_t file_id, uint64_t offset) {
    uint32_t i, slot = cache_slot(file_id, offset);

    for (i = 0; i < g_entries; i++) {
        vibeos_cache_entry_t *e = &g_table[(slot + i) % g_entries];
        if (e->file_id == 0u) {
            return 0;           /* an empty slot ends the probe */
        }
        if (e->file_id == file_id && e->offset == offset) {
            return e;
        }
    }
    return 0;
}

/* Make room. A clock: give every entry one second chance, then take the first
 * one that has not been touched since the hand last passed.
 *
 * Not LRU. LRU needs a list to be kept on every hit, which is a write on the
 * hot path to buy an eviction decision that is barely better - and this table
 * has tens of entries, not thousands. */
static vibeos_cache_entry_t *cache_evict(void) {
    uint32_t spins;

    for (spins = 0; spins < g_entries * 2u; spins++) {
        vibeos_cache_entry_t *e = &g_table[g_hand];
        g_hand = (g_hand + 1u) % g_entries;
        if (e->file_id == 0u) {
            return e;
        }
        if (e->used) {
            e->used = 0;
            continue;
        }
        /* The frame goes back with the entry. A cache that forgets an entry and
         * keeps its frame is a leak that grows with every eviction, and the
         * frame layer would have no idea who to blame. */
        (void)vibeos_frame_put(e->phys);
        e->file_id = 0;
        e->phys = 0;
        if (g_resident > 0u) {
            g_resident--;
        }
        vibeos_mm_stats()->reclaim_freed++;
        return e;
    }
    return 0;
}

/* Insert a key at its slot, probing forward. Only called after eviction has
 * guaranteed a free slot exists, but the probe still has a bound: a table with
 * no empty slot must return a failure rather than loop. */
static vibeos_cache_entry_t *cache_place(uint32_t file_id, uint64_t offset) {
    uint32_t i, slot = cache_slot(file_id, offset);

    for (i = 0; i < g_entries; i++) {
        vibeos_cache_entry_t *e = &g_table[(slot + i) % g_entries];
        if (e->file_id == 0u) {
            e->file_id = file_id;
            e->offset = offset;
            return e;
        }
    }
    return 0;
}

int vibeos_cache_get(uint32_t file_id, uint64_t offset, uint64_t *out_phys) {
    vibeos_cache_entry_t *e;
    uint64_t phys;

    if (!g_table || !out_phys || file_id == 0u) {
        return -1;
    }

    e = cache_lookup(file_id, offset);
    if (e) {
        e->used = 1;
        *out_phys = e->phys;
        vibeos_mm_stats()->cache_hits++;
        return 0;
    }
    vibeos_mm_stats()->cache_misses++;

    phys = vibeos_frame_alloc(VIBEOS_FRAME_CACHE);
    if (!phys) {
        return -1;
    }
    if (!g_read || g_read(g_read_ctx, file_id, offset, phys) != 0) {
        (void)vibeos_frame_put(phys);
        return -1;
    }

    /* Only now is there anything worth a slot. Placing the entry before the
     * read means a failed read leaves a key pointing at a frame holding
     * whatever was there - which the next hit would hand out as the file's
     * contents. */
    if (!cache_evict()) {
        (void)vibeos_frame_put(phys);
        return -1;
    }
    e = cache_place(file_id, offset);
    if (!e) {
        (void)vibeos_frame_put(phys);
        return -1;
    }
    e->phys = phys;
    e->used = 1;
    g_resident++;
    *out_phys = phys;
    return 0;
}

void vibeos_cache_forget(uint32_t file_id) {
    uint32_t i;

    if (!g_table || file_id == 0u) {
        return;
    }
    for (i = 0; i < g_entries; i++) {
        vibeos_cache_entry_t *e = &g_table[i];
        if (e->file_id != file_id) {
            continue;
        }
        (void)vibeos_frame_put(e->phys);
        e->file_id = 0;
        e->phys = 0;
        e->used = 0;
        if (g_resident > 0u) {
            g_resident--;
        }
    }
}
