#ifndef VIBEOS_PARTTAB_H
#define VIBEOS_PARTTAB_H

/* Writing a partition table.
 *
 * ## Why this is a separate file from partition.c
 *
 * Reading a table wrongly gives a machine that cannot boot, which is loud and
 * immediate and gets fixed. Writing one wrongly destroys data that was never
 * this machine's to lose, silently, and the loss is discovered by somebody
 * else later. They are not the same risk and they do not belong in the same
 * file.
 *
 * ## The four things that make a write safe
 *
 * **1. A stale view may not overwrite a newer one.** Every write states the
 * checksum of the table it believes is there, and is refused if the disk holds
 * something else. Without it, two things that both read the table, and one of
 * which then writes, silently discards the other's work - and on a partition
 * table "the other's work" is where somebody's filesystem begins.
 *
 * **2. A partition in use is not touched.** Mounted, or named by the swap
 * area. The caller supplies what is in use rather than this layer guessing,
 * because guessing wrong here is the whole failure mode.
 *
 * **3. Nothing here knows what a filesystem looks like.** Formatting is the
 * filesystem's own operation. A volume layer that wrote a FAT boot sector
 * would be a second place that has to be right about FAT, and this project has
 * spent whole phases removing second places.
 *
 * **4. The backup goes down before the primary.** GPT keeps a copy at the end
 * of the disk. Interrupt a write after the primary and before the backup and
 * the disk has two tables that disagree; interrupt it the other way round and
 * the disk still has its original primary and a newer backup, which a reader
 * can reconcile. The ordering costs nothing and is the difference between
 * recoverable and not.
 *
 * ## Entries may not overlap
 *
 * Checked here rather than trusted from the caller. Two partitions that share
 * a sector is not a table that is slightly wrong - it is two filesystems that
 * will each believe they own the same blocks, and the damage appears as file
 * corruption in one of them long after the table was written.
 */

#include <stdint.h>

#include "vibeos/blockdev.h"
#include "vibeos/partition.h"

/* What the caller says is in use, so this layer refuses rather than guesses.
 *
 * A range rather than an index: a mounted volume and a swap area are both
 * "these sectors are spoken for", and expressing them the same way means the
 * check has one shape instead of two. */
#define VIBEOS_PARTTAB_MAX_INUSE 8u

typedef struct {
    uint64_t first_lba;
    uint64_t sectors;
    const char *why;     /* "mounted", "swap", ... - for the refusal message */
} vibeos_parttab_inuse_t;

typedef struct {
    vibeos_parttab_inuse_t range[VIBEOS_PARTTAB_MAX_INUSE];
    uint32_t count;
} vibeos_parttab_guard_t;

typedef enum {
    VIBEOS_PARTTAB_OK = 0,
    VIBEOS_PARTTAB_BAD_ARGS,
    VIBEOS_PARTTAB_STALE,          /* the disk no longer holds what you read */
    VIBEOS_PARTTAB_IN_USE,         /* a range is mounted or is swap          */
    VIBEOS_PARTTAB_OVERLAP,        /* two entries share a sector             */
    VIBEOS_PARTTAB_PAST_END,       /* an entry runs off the disk             */
    VIBEOS_PARTTAB_IO,             /* the medium refused                     */
    VIBEOS_PARTTAB_RESULT_COUNT
} vibeos_parttab_result_t;

const char *vibeos_parttab_result_name(vibeos_parttab_result_t r);

/* The checksum of the table currently on the disk.
 *
 * Over sector 0 for an MBR, and over the GPT header plus its entry array for a
 * GPT. A caller reads the table, does its thinking, and hands this back with
 * the write; anything that changed the disk in between makes them differ.
 *
 * `bc` is read through, so it sees the same blocks everything else does. A
 * checksum taken around the cache would be checking a different disk from the
 * one being written.
 */
int vibeos_parttab_checksum(vibeos_blockcache_t *bc, uint64_t disk_sectors,
                            uint32_t *out_checksum);

/* Replace the MBR with `table`.
 *
 * `expect_checksum` must equal what vibeos_parttab_checksum returns now.
 * `guard` names what must not be disturbed; pass a guard with count 0 only
 * when the caller genuinely knows nothing is in use, which on a running
 * machine is never.
 */
vibeos_parttab_result_t vibeos_parttab_write_mbr(
    vibeos_blockcache_t *bc, uint64_t disk_sectors,
    const vibeos_parttable_t *table, const vibeos_parttab_guard_t *guard,
    uint32_t expect_checksum);

#endif /* VIBEOS_PARTTAB_H */
