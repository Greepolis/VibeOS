#ifndef VIBEOS_BLOCKDEV_H
#define VIBEOS_BLOCKDEV_H

/* Block device abstraction and cache.
 *
 * Two ideas, kept apart on purpose.
 *
 * A *device* is a pair of functions: put this sector in this buffer, put this
 * buffer in that sector. Nothing above it knows whether it is virtio, and
 * nothing inside it knows what the bytes mean. Expressing it as a function
 * pair rather than a call into the driver is what lets the layer above be
 * tested against an array on the host, which is where its bugs are actually
 * findable.
 *
 * A *cache* sits on top and holds a fixed number of recently used blocks.
 * VibeOS reaches the device for every read today, which FAT survives because
 * its structures are small and read mostly in order. Nothing more complex
 * would: an inode-based filesystem walks the same superblock and group
 * descriptors on every path lookup.
 *
 * Write-back, not write-through, because the point is to make repeated
 * metadata updates affordable. Durability is therefore something a caller
 * asks for, at a moment it chooses, by flushing - not a property it gets by
 * accident. A journal will be built on exactly that distinction.
 */

#include <stdint.h>

#define VIBEOS_BLOCK_SIZE 512u

/* Return 0 on success, non-zero on failure. `ctx` is the driver's own. */
typedef int (*vibeos_blockdev_read_fn)(void *ctx, uint64_t lba, void *buf);
typedef int (*vibeos_blockdev_write_fn)(void *ctx, uint64_t lba, const void *buf);
/* Make everything already written durable. A real drive acknowledges a write
 * as soon as it reaches its own volatile cache, so a write-back cache above it
 * that never issues this is ordering blocks it has no ability to order: after
 * a power cut the drive may have kept the last write and dropped an earlier
 * one. Everything the journal claims rests on this call existing. */
typedef int (*vibeos_blockdev_flush_fn)(void *ctx);

typedef struct {
    vibeos_blockdev_read_fn read;
    vibeos_blockdev_write_fn write;   /* may be NULL: a read-only device */
    vibeos_blockdev_flush_fn flush;   /* may be NULL: no volatile cache */
    void *ctx;
    uint64_t sectors;                 /* 0 when the size is unknown */
} vibeos_blockdev_t;

/* One cached block. The caller supplies the storage, so this header imposes no
 * allocator and no fixed cache size on the kernel. */
typedef struct {
    uint64_t lba;
    uint8_t *data;      /* VIBEOS_BLOCK_SIZE bytes, supplied by the caller */
    uint32_t stamp;     /* last use, for eviction */
    uint8_t valid;
    uint8_t dirty;
} vibeos_block_slot_t;

typedef struct {
    const vibeos_blockdev_t *dev;
    vibeos_block_slot_t *slots;
    uint32_t slot_count;
    uint32_t clock;     /* monotonic use counter feeding `stamp` */
    /* Counters, so a caller can show that the cache is doing something rather
     * than assume it. A cache that never hits and a cache that is not wired in
     * look identical from outside. */
    uint64_t hits;
    uint64_t misses;
    uint64_t writebacks;
} vibeos_blockcache_t;

/* `slots` must have `slot_count` entries, each with `data` already pointing at
 * VIBEOS_BLOCK_SIZE bytes. Returns 0 on success. */
int vibeos_blockcache_init(vibeos_blockcache_t *bc, const vibeos_blockdev_t *dev,
                           vibeos_block_slot_t *slots, uint32_t slot_count);

/* Copy one block into `buf`. */
int vibeos_blockcache_read(vibeos_blockcache_t *bc, uint64_t lba, void *buf);

/* Replace one block. The device is not touched until the block is evicted or
 * flushed - which is the whole point, and also the whole risk. */
int vibeos_blockcache_write(vibeos_blockcache_t *bc, uint64_t lba, const void *buf);

/* Write every dirty block out. Returns 0 only if all of them succeeded: a
 * partial flush that reported success would be worse than no flush at all,
 * because a journal would then trust an ordering that did not happen.
 *
 * This is also where the device is told to empty its own cache, so a caller
 * that returns from here has a real barrier and not merely an empty slot
 * table. */
int vibeos_blockcache_flush(vibeos_blockcache_t *bc);

/* Forget everything, dirty blocks included. For a volume being unmounted after
 * a flush, or one that has gone away. */
void vibeos_blockcache_invalidate(vibeos_blockcache_t *bc);

#endif
