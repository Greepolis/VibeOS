/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_EXT2_H
#define VIBEOS_EXT2_H

/* ext2 and ext4, read-only (ext4 since docs/filesystem-design/ext4.md step 1).
 *
 * ext4 is ext2 with extent trees for a file's blocks, 64-bit block numbers
 * and group descriptors, and crc32c over every piece of metadata. The driver
 * reads both: what decides is the volume's feature flags, refused by name
 * when they ask for something it does not do.
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
    uint64_t blocks_count;        /* the volume's length in blocks: every block read is bounded by it */
    uint32_t incompat;            /* the superblock's feature words, as the volume says them */
    uint32_t ro_compat;
    uint32_t desc_size;           /* 32, or 64 and more with the 64bit feature */
    uint32_t csum_seed;           /* metadata_csum's seed: crc32c of the uuid, or the stored seed */
    int csum;                     /* metadata_csum: every metadata read is checked */
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
    uint8_t cached_inode[256];    /* a whole inode: ext4's are 256 bytes, its checksum in both halves */
    uint8_t cached_ptrs[4096];
} vibeos_ext2_t;

/* How many inode and pointer-block reads the caches answered: the counter
 * that says they are used at all. */
uint64_t vibeos_ext2_cache_hits(void);

/* How many metadata blocks or inodes failed their checksum and were refused:
 * a count that must stay zero on a healthy volume, and the only way a host
 * test can tell a refusal for a checksum from one for any other reason. */
uint64_t vibeos_ext2_csum_failures(void);

/* The incompat feature bits the last refused mount did not know - what a
 * refusal for a feature names; zero when the last refusal was for something
 * else. */
uint32_t vibeos_ext2_last_refused_features(void);

/* The castagnoli crc32 as ext4 uses it - no inversion before or after, so
 * crc32c(~0, ...) is what the kernel's ext4_chksum computes. Exposed for the
 * host test that checks it against a known value. */
uint32_t vibeos_ext2_crc32c(uint32_t crc, const void *data, uint32_t len);

/* Read the superblock and remember the geometry. Returns 0 on success.
 * Refuses anything it does not fully understand rather than guessing: a
 * filesystem read with the wrong block size returns plausible bytes from the
 * wrong place, which is worse than refusing to mount. */
int vibeos_ext2_mount(vibeos_ext2_t *fs, vibeos_blockcache_t *cache,
                      uint64_t part_lba);

/* The operations table, for vibeos_fs_mount. */
const vibeos_fs_ops_t *vibeos_ext2_ops(void);

#endif
