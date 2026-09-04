#ifndef VIBEOS_BACKING_H
#define VIBEOS_BACKING_H

#include <stdint.h>

#include "vibeos/mm_model.h"

/* L3: backing stores. Where a page comes from when it is not there.
 *
 * A region says what an address *is*; a backing store says how to make it
 * exist. Anonymous memory answers with a zeroed frame, and a file answers with
 * its contents - which is the same question with a different answer, so it is
 * the same interface.
 *
 * The point of the cache half is not speed for its own sake. Every exec here
 * reads a whole program off the disk through a staging buffer, so running
 * BusyBox twice reads BusyBox twice; a shell that runs twenty commands reads
 * twenty programs it has already seen. Caching by (file, offset) is what makes
 * the second read free, and it is the prerequisite for faulting pages in
 * instead of copying them through a buffer at all.
 */

typedef struct vibeos_backing_ops {
    /* Produce the frame holding `offset` of this object. Returns 0 and sets
     * *out_phys on success. */
    int (*fault_in)(void *ctx, uint64_t offset, uint64_t *out_phys);
    int (*write_back)(void *ctx, uint64_t offset, uint64_t phys);
    int (*release)(void *ctx, uint64_t offset);
} vibeos_backing_ops_t;

/* ---- anonymous ---------------------------------------------------------- */

/* Zero-filled on first touch. What P3's regions already do implicitly, given a
 * name so that the fault path has one shape rather than a special case. */
const vibeos_backing_ops_t *vibeos_backing_anon(void);

/* ---- the page cache ----------------------------------------------------- */

/* One entry per resident page. The caller supplies the table, as everywhere
 * else in this subsystem, so the same code is host-tested against a static
 * array and used in the kernel against real memory. */
typedef struct vibeos_cache_entry {
    uint32_t file_id;          /* 0 means the slot is empty */
    uint32_t used;             /* clock hand's second chance */
    uint64_t offset;
    uint64_t phys;
} vibeos_cache_entry_t;

/* How a cache miss reaches the disk. Returns 0 on success, having filled the
 * frame. Supplied by the kernel; a host test supplies its own. */
typedef int (*vibeos_cache_read_fn)(void *ctx, uint32_t file_id,
                                    uint64_t offset, uint64_t phys);

void vibeos_cache_init(vibeos_cache_entry_t *table, uint32_t entries,
                       vibeos_cache_read_fn read, void *ctx);

/* The frame holding (file_id, offset), reading it in if it is not resident.
 * Returns 0 and sets *out_phys, or negative when the read fails or no frame can
 * be had.
 *
 * `file_id` must not be zero: zero marks an empty slot, and letting it through
 * would make "not cached" and "cached under file zero" the same state. */
/* Serialise this layer against itself, exactly as the frame layer and the
 * region pool do. Both may be null, which is what a host test passes.
 *
 * The fourth time this project has needed this sentence, and the third layer to
 * have shipped without it. A table with linear probing and a clock hand is not
 * a set of independent slots: two cores placing entries at once can leave one
 * entry's frame beside another entry's key, and then a lookup that "hits"
 * returns the pages of a different file. Nothing about that reads as a cache
 * bug from the outside - it reads as a program that was handed somebody else's
 * text.
 *
 * It stayed invisible for as long as the cache had exactly one caller,
 * `hw_read_file_cached`, which runs under the exec lock. The moment a second
 * caller appeared - a loader mapping image pages instead of copying them,
 * reachable from a boot-time spawn that holds no such lock - the table became
 * concurrent and nothing said so. */
void vibeos_cache_set_lock(void (*lock)(void), void (*unlock)(void));

int vibeos_cache_get(uint32_t file_id, uint64_t offset, uint64_t *out_phys);

/* Drop everything belonging to a file - what a write or a delete must do, or
 * the next read returns what the file used to contain. */
void vibeos_cache_forget(uint32_t file_id);

/* Resident pages, for meminfo and the gate. */
uint32_t vibeos_cache_resident(void);

/* Give back up to `want` resident pages, and return how many were actually
 * dropped. This is the clean tier reclaim evicts first: dropping one costs
 * nothing because the file still has it and the next fault reads it back.
 *
 * Exposed here rather than reimplemented in reclaim, because this layer
 * already owns the clock hand and the eviction, and a second copy of that
 * knowledge elsewhere is how two structures come to disagree about the same
 * fact - which is the family of defect this subsystem has spent four
 * investigations on. */
uint32_t vibeos_cache_reclaim(uint32_t want);

#endif /* VIBEOS_BACKING_H */
