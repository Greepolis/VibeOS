#ifndef VIBEOS_EXFAT_H
#define VIBEOS_EXFAT_H

/* exFAT, read-only.
 *
 * Conceptually FAT with a 32-bit table and an allocation bitmap, which makes
 * it sound like a small step from the FAT driver. The directory format is
 * where that stops being true: a single file is described by a *set* of 32-byte
 * entries - one for the file, one for its stream, and one or more carrying the
 * name in UTF-16 - and a reader that treats entries individually sees three
 * unrelated records instead of one file.
 *
 * The other difference that matters: a file may be contiguous, in which case
 * the allocation table holds nothing for it and following the chain would read
 * whatever the table happens to contain. The stream entry carries a flag for
 * that, and honouring it is not an optimisation - ignoring it reads the wrong
 * clusters for every contiguous file, which is most of them on a freshly
 * written volume.
 */

#include <stdint.h>

#include "vibeos/blockdev.h"
#include "vibeos/vfs.h"

typedef struct {
    vibeos_blockcache_t *cache;
    uint64_t part_lba;
    uint64_t part_sectors;   /* partition length in 512B sectors, from the
                             * table; 0 = unbounded (no table). H-029. */

    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t fat_offset;        /* in sectors, from the partition start */
    uint32_t cluster_heap;      /* in sectors, from the partition start */
    uint32_t cluster_count;
    uint32_t root_cluster;
    int mounted;
    /* Where the last walk down a fragmented file's chain stopped: its first
     * cluster, the index reached and the cluster there. A sequential read is
     * 4 KiB at a time, and each piece walked the chain from the start again
     * (#106); from here it walks on. The volume is read-only, so the chain
     * does not change. Reads come from several cores: `cur_seq` is odd while
     * a writer fills the three, and a reader that sees it move walks from the
     * start, which is never wrong. */
    uint32_t cur_first;
    uint32_t cur_index;
    uint32_t cur_cluster;
    uint32_t cur_seq;
} vibeos_exfat_t;

int vibeos_exfat_mount(vibeos_exfat_t *fs, vibeos_blockcache_t *cache,
                       uint64_t part_lba);

const vibeos_fs_ops_t *vibeos_exfat_ops(void);

#endif
