#ifndef VIBEOS_EXT2_H
#define VIBEOS_EXT2_H

/* ext2, read-only.
 *
 * The first filesystem here that is not FAT, and chosen deliberately ahead of
 * easier ones. FAT is a chain of clusters and a flat-ish directory; ext2 has a
 * superblock, block groups, inodes, and a file's blocks reached through one,
 * two or three levels of indirection. If the filesystem interface from stage
 * two only fits FAT, this is where that shows.
 *
 * It already has: FAT's node identity - the first cluster - is enough to read
 * the file, so a lookup result can be read from directly. An ext2 inode number
 * is not; the driver has to fetch the inode again for every read. The
 * interface survives that, at the cost of a re-read this driver could avoid if
 * a node could carry driver-private bytes. That is a real finding about the
 * interface rather than a complaint about ext2, and it is written down here
 * rather than fixed in the same step that discovered it.
 *
 * Read-only on purpose. Writing means allocation bitmaps, link counts and
 * ordering, and roadmap M16 is where ordering gets built once, for everyone.
 */

#include <stdint.h>

#include "vibeos/blockdev.h"
#include "vibeos/vfs.h"

#define VIBEOS_EXT2_MAGIC 0xEF53u
#define VIBEOS_EXT2_ROOT_INO 2u

typedef struct {
    vibeos_blockcache_t *cache;   /* where blocks come from */
    uint64_t part_lba;            /* first sector of the partition */

    uint32_t block_size;
    uint32_t inodes_per_group;
    uint32_t blocks_per_group;
    uint32_t inode_size;
    uint32_t group_count;
    uint32_t first_data_block;
    uint32_t group_desc_block;
    uint32_t blocks_count;        /* the volume's length in blocks: every block read is bounded by it */
    int mounted;
    /* What sequential reads ask for again and again (#108, #115): the inode of
     * the file being read - the regular-file layer reads 4 KiB at a time and
     * each call fetched it anew - and the last pointer block a block number
     * was mapped through, which consecutive blocks share. The volume is
     * read-only, so neither goes stale; 0 is "nothing held". Reads of one
     * volume come from several cores, so each slot has a sequence: odd
     * while a writer fills it, and a reader that saw it move takes a miss. */
    uint32_t cached_ino;
    uint32_t cached_ptr_block;
    uint32_t ino_seq;
    uint32_t ptr_seq;
    uint8_t cached_inode[128];
    uint8_t cached_ptrs[4096];
} vibeos_ext2_t;

/* How many inode and pointer-block reads the caches answered: the counter
 * that says they are used at all. */
uint64_t vibeos_ext2_cache_hits(void);

/* Read the superblock and remember the geometry. Returns 0 on success.
 * Refuses anything it does not fully understand rather than guessing: a
 * filesystem read with the wrong block size returns plausible bytes from the
 * wrong place, which is worse than refusing to mount. */
int vibeos_ext2_mount(vibeos_ext2_t *fs, vibeos_blockcache_t *cache,
                      uint64_t part_lba);

/* The operations table, for vibeos_fs_mount. */
const vibeos_fs_ops_t *vibeos_ext2_ops(void);

#endif
