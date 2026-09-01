#ifndef VIBEOS_JOURNAL_H
#define VIBEOS_JOURNAL_H

/* Write-ahead journal for metadata.
 *
 * The exit criterion for this layer is not "writes are journalled", it is
 * "cutting the power during a write leaves a mountable volume". Those are
 * different claims and only the second one is testable, so the design is
 * arranged around what a reader finds afterwards rather than around what the
 * writer intended.
 *
 * A transaction is a set of blocks that must all land or none of them. It is
 * written twice: first into a reserved region of the volume, then to where it
 * actually belongs. Between the two there is a commit record, and the whole
 * scheme rests on that record being written *after* the copy it describes is
 * durable, and *before* anything is overwritten in place.
 *
 * Nothing here can order two writes by issuing them in order. The block cache
 * is write-back, so the only barrier that exists is a flush, and the phases
 * below are separated by one for that reason. Within a phase the order is
 * arbitrary and must not matter - which is why a half-written data region has
 * no commit record to point at it, and a half-finished checkpoint is simply
 * replayed again.
 *
 * Recovery therefore has three cases and they are all the same two questions:
 * is there a descriptor, and is there a commit record that matches it. No
 * descriptor, or a descriptor without its commit, means the transaction never
 * happened and the volume still holds the old contents everywhere. Both
 * present means it did happen, and every block is copied to its target again -
 * possibly for the second time, which is harmless because the copy is the
 * same bytes.
 *
 * The commit record names the descriptor it belongs to by checksum rather
 * than by sequence number alone. The counter restarts whenever the journal is
 * attached, so a record a finished transaction left in the region can carry
 * the same number as the descriptor of an unfinished one - and replaying an
 * unfinished transaction is exactly the outcome this layer promises will not
 * happen.
 *
 * One transaction is in flight at a time. Batching several would be faster and
 * would make the replay ordering a real problem; this is the version whose
 * failure modes can be enumerated.
 */

#include <stdint.h>

#include "vibeos/blockdev.h"

/* Reserved region layout, in blocks from `base`:
 *   0            descriptor: which targets, and the sequence number
 *   1 .. count   the new contents of those targets, in the same order
 *   1 + count    commit record
 * so a region of N blocks carries at most N - 2 targets. */
#define VIBEOS_JOURNAL_OVERHEAD 2u

/* A descriptor block holds 16 bytes of header and then one 64-bit LBA per
 * target, which is what caps a transaction. */
#define VIBEOS_JOURNAL_MAX_TARGETS ((VIBEOS_BLOCK_SIZE - 16u) / 8u)

typedef struct {
    vibeos_blockcache_t *bc;
    uint64_t base;             /* first block of the reserved region */
    uint32_t capacity;         /* blocks reserved, including the overhead */
    uint32_t max_targets;      /* derived from capacity and the cap above */

    uint64_t *targets;         /* max_targets entries, caller-supplied */
    uint8_t *staging;          /* max_targets * VIBEOS_BLOCK_SIZE, ditto */
    uint32_t staged;
    uint8_t open;

    uint64_t seq;              /* incremented per committed transaction */

    /* Counters, for the same reason the cache has them: a journal that is
     * never exercised and one that is not wired in look identical. */
    uint64_t commits;
    uint64_t replays;
} vibeos_journal_t;

/* Attach to a region and recover whatever the last run left behind. Returns 0
 * if the volume is consistent afterwards, non-zero if recovery could not be
 * completed - in which case the volume must not be mounted, because the
 * failure means it is not known which of the two states it is in.
 *
 * `targets` and `staging` must be sized as described in the struct above. */
int vibeos_journal_init(vibeos_journal_t *j, vibeos_blockcache_t *bc,
                        uint64_t base, uint32_t capacity,
                        uint64_t *targets, uint8_t *staging);

/* Start collecting. Fails if one is already open. */
int vibeos_journal_begin(vibeos_journal_t *j);

/* Add the intended new contents of one block. Staging the same LBA twice
 * replaces the earlier version rather than recording it twice, so a caller
 * that touches a bitmap repeatedly does not exhaust the region. */
int vibeos_journal_stage(vibeos_journal_t *j, uint64_t lba, const void *buf);

/* Make the whole set durable, or leave the volume exactly as it was. Returns 0
 * only when every target has reached the device. */
int vibeos_journal_commit(vibeos_journal_t *j);

/* Throw the collected blocks away. Nothing was written, so there is nothing to
 * undo. */
void vibeos_journal_abort(vibeos_journal_t *j);

#endif
