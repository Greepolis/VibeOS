/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_EXT2_H
#define VIBEOS_EXT2_H

/* ext2 and ext4, read (ext4 since docs/filesystem-design/ext4.md step 1) and ext4
 * written (step 2).
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
 * ext2 is read-only on purpose. Writing means allocation bitmaps, link counts
 * and ordering, and roadmap M16 is where ordering gets built once, for
 * everyone; ext4 is written, without a journal, by kernel/fs/ext4_write.c
 * (docs/filesystem-design/ext4.md step 2).
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

    /* ---- the writer's (docs/filesystem-design/ext4.md step 2) ----
     * Kept here, in the volume's state, so the driver needs no allocator and
     * two volumes are written at once; the VFS serialises calls into one
     * (vibeos_fs_ops_t.serialize), which is what makes one set enough. */
    int writable;                 /* ext4 with metadata_csum and nothing it does not write */
    uint8_t sb[1024];             /* the superblock as on the volume: counts and checksum kept in it */
    uint8_t wbuf[4096];           /* one block, for whatever the operation is changing */
    uint8_t wbuf2[4096];          /* the allocator's, for a bitmap: never held across a call */
#ifdef VIBEOS_EXT2_FUZZ_CANARY
    /* Test-only red zone immediately follows the production bitmap scratch
     * buffer, so an overrun in the real writer is observable by the harness. */
    uint8_t fuzz_wbuf2_guard[16];
#endif
    uint32_t ext_n;               /* the file being changed: its extents, in order */
    /* When the file's tree has an index level, ext[] holds a window of it and
     * not the whole file (E4-008): up to three neighbouring leaves under one
     * parent - the root, or an index block under it - and the file blocks
     * [lo, hi) those leaves may hold. A store writes those leaves back and
     * the parent's entries for them, so a file's extents are not bounded by
     * what ext[] holds. */
    struct {
        int on;                   /* ext[] is a window, not the whole file */
        uint32_t ino;             /* whose */
        uint32_t depth;           /* of the tree: 1 or 2 */
        uint64_t parent;          /* the index block holding the leaves; 0: the root */
        uint32_t rslot;           /* depth 2: the root entry naming `parent` */
        uint32_t first, count;    /* the leaves: the parent's entries [first, first + count) */
        uint32_t first_key;       /* the parent's key for leaf `first` as loaded */
        uint64_t lo, hi;          /* file blocks the window holds */
        uint64_t data;            /* data blocks its extents held when loaded */
        uint64_t leaf[3];         /* its leaves as loaded */
    } win;
    struct {
        uint32_t lblk;            /* first block of the file it covers */
        uint16_t len;             /* blocks, 1..32768 */
        uint16_t start_hi;        /* the volume block it starts at, high 16 bits */
        uint32_t start_lo;
    } ext[1360];                  /* four leaves of 340: depth 1 at most */
    uint8_t cached_ptrs[4096];
    /* Files whose last name went while a description held them (E4-019):
     * kept, on the volume's orphan list, until the last description goes.
     * Full, a file is freed at once, as before - and counted. */
    uint32_t orphan_n;
    uint32_t orphan[32];
    /* Bytes this mount has written, not yet added to s_kbytes_written: the
     * superblock takes them whenever it is next written (E4-013). */
    uint64_t written_pending;

    /* ---- the journal's (docs/filesystem-design/ext4.md step 3) ----
     * One operation is one transaction. What it changes of the metadata is
     * written into the journal's blocks, not over the metadata - the block
     * cache is write-back, so a block written in place reaches the disk
     * whenever it is evicted, which may be before the commit - and every read
     * of such a block while the transaction is open is answered from there
     * (kernel/fs/ext4_journal.c). */
    struct {
        int on;                   /* has_journal and a journal this driver writes */
        int open;                 /* a transaction is being built */
        int dead;                 /* a commit failed half-way: no more writes */
        int csum3;                /* the journal's checksums are v3 */
        int is64;                 /* tags carry 64-bit block numbers */
        uint32_t first;           /* s_first: the log's first block */
        uint32_t maxlen;          /* s_maxlen: the journal's length in blocks */
        uint32_t seq;             /* the next transaction's sequence number */
        uint32_t start;           /* s_start as found: 0 is an empty log */
        uint32_t seed;            /* crc32c of the journal's uuid */
        uint32_t tag_bytes;       /* one descriptor tag, uuid excluded */
        uint32_t tags_per_desc;   /* what one descriptor block holds */
        uint32_t cap;             /* blocks one transaction may log */
        uint32_t n;               /* blocks this one has logged */
        uint32_t nx;              /* the journal inode's extents */
        struct {
            uint32_t lblk, len;
            uint64_t phys;
        } x[16];
        uint64_t home[384];       /* where each logged block belongs */
        uint8_t escaped[48];      /* which logged blocks began with the journal's magic */
        uint32_t nfreed;          /* blocks freed by this transaction, as runs */
        int freed_overflow;       /* more runs than the table: every data write is logged */
        struct {
            uint64_t start, count;
        } freed[16];
    } j;
} vibeos_ext2_t;

/* The journal's counters are in vibeos/ext4_journal.h. */

/* How many inode and pointer-block reads the caches answered: the counter
 * that says they are used at all. */
uint64_t vibeos_ext2_cache_hits(void);

/* How many metadata blocks or inodes failed their checksum and were refused:
 * a count that must stay zero on a healthy volume, and the only way a host
 * test can tell a refusal for a checksum from one for any other reason. */
uint64_t vibeos_ext2_csum_failures(void);
/* Host tests only: the n-th data write from now fails once (E4-009). */
void vibeos_ext4_fail_data_after(uint32_t n);

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
