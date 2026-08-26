#ifndef VIBEOS_NTFS_H
#define VIBEOS_NTFS_H

/* NTFS, read-only.
 *
 * The most involved driver here, and the one with the most ways to be subtly
 * wrong. Three of its ideas have no counterpart in the filesystems above.
 *
 * Everything is a file, including the index of files. The master file table is
 * itself a file described by a record inside itself, so mounting means reading
 * the table's own record before the table can be read.
 *
 * Records carry fixups. The last two bytes of every sector in a record are
 * replaced by a sequence number, and the real bytes are kept in an array at
 * the start. This exists so a torn write is detectable, and it means a reader
 * that does not undo it gets two wrong bytes per sector - not a crash, just
 * quietly corrupt data at 510-byte intervals.
 *
 * A file's contents are an attribute, and an attribute is either resident -
 * stored inside the record - or described by a run list of extents. Small
 * files have no clusters at all, which a reader expecting a cluster chain
 * cannot represent.
 *
 * Read-only, deliberately and permanently for now: see docs/storage_plan.md
 * for why writing is a different size of problem.
 */

#include <stdint.h>

#include "vibeos/blockdev.h"
#include "vibeos/vfs.h"

#define VIBEOS_NTFS_MFT_RECORD_MAX 4096u

typedef struct {
    vibeos_blockcache_t *cache;
    uint64_t part_lba;

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t mft_record_bytes;
    uint64_t mft_lcn;           /* first cluster of the master file table */
    int mounted;
} vibeos_ntfs_t;

int vibeos_ntfs_mount(vibeos_ntfs_t *fs, vibeos_blockcache_t *cache,
                      uint64_t part_lba);

const vibeos_fs_ops_t *vibeos_ntfs_ops(void);

#endif
