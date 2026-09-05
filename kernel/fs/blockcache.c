/* Block cache: fixed slots, least-recently-used eviction, write-back.
 *
 * Portable on purpose. Everything here is bookkeeping over a function pair, so
 * it can be driven from host tests against an array instead of a disk - which
 * is where eviction and write-back ordering bugs are actually findable. The
 * same move on the FAT chain walker is what turned an intermittent boot
 * failure into an exhaustive sweep.
 */

#include "vibeos/blockdev.h"

/* The lock, registered rather than assumed. See the header for why it is one
 * lock for every instance and why it is a registration function.
 *
 * Null until somebody registers one, and that is the host-test arrangement:
 * the tests drive this layer from a single thread against an array, so there
 * is nothing to serialise and nothing to deadlock. On a machine, kmain
 * registers before any filesystem mounts. */
static void (*g_lock)(void);
static void (*g_unlock)(void);

void vibeos_blockcache_set_lock(void (*lock)(void), void (*unlock)(void)) {
    g_lock = lock;
    g_unlock = unlock;
}

static void bc_lock(void) {
    if (g_lock) {
        g_lock();
    }
}

static void bc_unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

static void block_copy(uint8_t *dst, const uint8_t *src) {
    uint32_t i;
    for (i = 0; i < VIBEOS_BLOCK_SIZE; i++) {
        dst[i] = src[i];
    }
}

int vibeos_blockcache_init(vibeos_blockcache_t *bc, const vibeos_blockdev_t *dev,
                           vibeos_block_slot_t *slots, uint32_t slot_count) {
    uint32_t i;

    if (!bc || !dev || !dev->read || !slots || slot_count == 0u) {
        return -1;
    }
    for (i = 0; i < slot_count; i++) {
        if (!slots[i].data) {
            return -1;   /* a slot with nowhere to put a block is not a slot */
        }
        slots[i].lba = 0;
        slots[i].stamp = 0;
        slots[i].valid = 0;
        slots[i].dirty = 0;
    }
    bc->dev = dev;
    bc->slots = slots;
    bc->slot_count = slot_count;
    bc->clock = 0;
    bc->hits = 0;
    bc->misses = 0;
    bc->evictions = 0;
    bc->writebacks = 0;
    bc->evict_failed = 0;
    return 0;
}

static vibeos_block_slot_t *find(vibeos_blockcache_t *bc, uint64_t lba) {
    uint32_t i;
    for (i = 0; i < bc->slot_count; i++) {
        if (bc->slots[i].valid && bc->slots[i].lba == lba) {
            return &bc->slots[i];
        }
    }
    return 0;
}

static int writeback(vibeos_blockcache_t *bc, vibeos_block_slot_t *s) {
    if (!s->valid || !s->dirty) {
        return 0;
    }
    if (!bc->dev->write) {
        return -1;   /* dirty on a read-only device: someone made a mistake */
    }
    if (bc->dev->write(bc->dev->ctx, s->lba, s->data) != 0) {
        return -1;
    }
    s->dirty = 0;
    bc->writebacks++;
    return 0;
}

/* Pick a slot to reuse: a free one if there is any, otherwise the one used
 * longest ago. An unwritten dirty block is written out first - dropping it
 * would silently discard a write the caller believes succeeded. */
static vibeos_block_slot_t *evict(vibeos_blockcache_t *bc) {
    vibeos_block_slot_t *oldest = &bc->slots[0];
    uint32_t i;

    for (i = 0; i < bc->slot_count; i++) {
        if (!bc->slots[i].valid) {
            return &bc->slots[i];
        }
        if (bc->slots[i].stamp < oldest->stamp) {
            oldest = &bc->slots[i];
        }
    }
    if (writeback(bc, oldest) != 0) {
        /* Counted, because returning null here looks from above like an
         * ordinary miss that could not be served, and it is not: it is a block
         * the caller was told had been written and which is still only in this
         * cache. The gate asserts it is zero. */
        bc->evict_failed++;
        return 0;
    }
    /* Counted only when a slot was actually taken from another block. The
     * early return above hands back a slot that was never in use, and calling
     * that an eviction would make a cold cache look like a thrashing one. */
    bc->evictions++;
    return oldest;
}

static vibeos_block_slot_t *fetch(vibeos_blockcache_t *bc, uint64_t lba, int for_write) {
    vibeos_block_slot_t *s = find(bc, lba);

    if (s) {
        bc->hits++;
        s->stamp = ++bc->clock;
        return s;
    }
    bc->misses++;
    s = evict(bc);
    if (!s) {
        return 0;
    }
    /* A full-block write does not need the old contents, and reading them
     * first would cost a device round trip to fetch bytes about to be
     * overwritten. A partial write would be a different matter - this cache
     * deliberately has no partial write. */
    if (!for_write) {
        if (bc->dev->read(bc->dev->ctx, lba, s->data) != 0) {
            s->valid = 0;
            return 0;
        }
    }
    s->lba = lba;
    s->valid = 1;
    s->dirty = 0;
    s->stamp = ++bc->clock;
    return s;
}

int vibeos_blockcache_read(vibeos_blockcache_t *bc, uint64_t lba, void *buf) {
    vibeos_block_slot_t *s;

    if (!bc || !bc->dev || !buf) {
        return -1;
    }
    if (bc->dev->sectors != 0u && lba >= bc->dev->sectors) {
        return -1;
    }
    bc_lock();
    s = fetch(bc, lba, 0);
    if (!s) {
        bc_unlock();
        return -1;
    }
    /* Copied out under the lock, not after it. The slot can be evicted and
     * refilled with another block the moment the lock is released, so a copy
     * outside it is the page cache's old defect wearing different clothes: a
     * lookup that hit and handed back somebody else's bytes. */
    block_copy((uint8_t *)buf, s->data);
    bc_unlock();
    return 0;
}

int vibeos_blockcache_write(vibeos_blockcache_t *bc, uint64_t lba, const void *buf) {
    vibeos_block_slot_t *s;

    if (!bc || !bc->dev || !buf) {
        return -1;
    }
    if (!bc->dev->write) {
        return -1;
    }
    if (bc->dev->sectors != 0u && lba >= bc->dev->sectors) {
        return -1;
    }
    bc_lock();
    s = fetch(bc, lba, 1);
    if (!s) {
        bc_unlock();
        return -1;
    }
    block_copy(s->data, (const uint8_t *)buf);
    s->dirty = 1;
    bc_unlock();
    return 0;
}

int vibeos_blockcache_flush(vibeos_blockcache_t *bc) {
    uint32_t i;
    int rc = 0;

    if (!bc || !bc->dev) {
        return -1;
    }
    /* Every dirty block is attempted even after one fails. Stopping at the
     * first error would leave later blocks dirty with no record of why, and
     * the caller cannot act on a partial result it cannot see. */
    bc_lock();
    for (i = 0; i < bc->slot_count; i++) {
        if (writeback(bc, &bc->slots[i]) != 0) {
            rc = -1;
        }
    }
    /* Handing the blocks to the device is not the same as the device having
     * kept them. Skipping this when a write failed would report a barrier the
     * caller did not get. */
    if (bc->dev->flush != 0 && bc->dev->flush(bc->dev->ctx) != 0) {
        rc = -1;
    }
    bc_unlock();
    return rc;
}

void vibeos_blockcache_invalidate(vibeos_blockcache_t *bc) {
    uint32_t i;

    if (!bc) {
        return;
    }
    bc_lock();
    for (i = 0; i < bc->slot_count; i++) {
        bc->slots[i].valid = 0;
        bc->slots[i].dirty = 0;
    }
    bc_unlock();
}
