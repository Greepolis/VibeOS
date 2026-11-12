/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef VIBEOS_FS_EXT2_INTERNAL_H
#define VIBEOS_FS_EXT2_INTERNAL_H

/* What ext2.c (the reader) shares with ext4_write.c (the writer): the same
 * volume, so one definition of every on-disk number and one set of read
 * helpers. Not a public interface - nothing outside kernel/fs/ includes this,
 * and vibeos/ext2.h is what the rest of the kernel and the tests see. */

#include <stdint.h>

#include "vibeos/ext2.h"
#include "vibeos/ext4_write.h"
#include "vibeos/vfs.h"

/* Blocks are up to 4 KiB here. A larger block size is legal in ext2 but rare,
 * and refusing it is honest: reading with the wrong size returns bytes from
 * the wrong place rather than failing. */
#define EXT2_MAX_BLOCK 4096u
#define EXT2_MAX_INODE_SIZE 256u

/* The superblock's feature words (docs/filesystem-design/ext4.md). */
#define COMPAT_HAS_JOURNAL 0x0004u
#define COMPAT_EXT_ATTR    0x0008u
#define COMPAT_RESIZE_INODE 0x0010u
#define COMPAT_DIR_INDEX   0x0020u
#define INCOMPAT_FILETYPE  0x0002u
#define INCOMPAT_RECOVER   0x0004u
#define INCOMPAT_EXTENTS   0x0040u
#define INCOMPAT_64BIT     0x0080u
#define INCOMPAT_FLEX_BG   0x0200u
#define INCOMPAT_CSUM_SEED 0x2000u
#define INCOMPAT_LARGEDIR  0x4000u
#define RO_COMPAT_SPARSE_SUPER 0x0001u
#define RO_COMPAT_LARGE_FILE   0x0002u
#define RO_COMPAT_HUGE_FILE    0x0008u
#define RO_COMPAT_DIR_NLINK    0x0020u
#define RO_COMPAT_EXTRA_ISIZE  0x0040u
#define RO_COMPAT_METADATA_CSUM 0x0400u

#define INODE_FLAG_INDEX   0x00001000u   /* a directory with an htree */
#define INODE_FLAG_EXTENTS 0x00080000u
#define EXTENT_MAGIC 0xF30Au
#define EXTENT_MAX_DEPTH 5u   /* what ext4 itself allows; deeper is a loop or garbage */

/* ---- the reader's helpers, shared (ext2.c) ---- */
int ext2_read_block(vibeos_ext2_t *fs, uint64_t block, uint8_t *out);
int ext2_inode_table(vibeos_ext2_t *fs, uint32_t group, uint64_t *out);
uint32_t ext2_inode_seed(const vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode);
int ext2_inode_csum_ok(const vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode);
int ext2_read_inode(vibeos_ext2_t *fs, uint32_t ino, uint8_t *out);
int ext2_map_block(vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode, uint32_t n, uint64_t *out);
uint64_t ext2_inode_size(const uint8_t *inode);
int ext2_inode_is_dir(const uint8_t *inode);
uint32_t ext2_dir_nblocks(const vibeos_ext2_t *fs, uint64_t size);
int ext2_dir_block(vibeos_ext2_t *fs, uint32_t ino, const uint8_t *dir, uint64_t phys, uint8_t *block);
uint32_t ext2_dir_find(vibeos_ext2_t *fs, uint32_t dir_ino, const uint8_t *dir, const char *name,
                       uint32_t name_len);
uint32_t ext2_resolve(vibeos_ext2_t *fs, const char *path, uint8_t *inode);
uint32_t ext2_le32_crc(uint32_t crc, uint32_t v);
int ext2_csum_bad(uint64_t what);
int ext2_op_lookup(void *fsv, const char *path, vibeos_fs_node_t *out);
long ext2_op_read_at(void *fsv, const vibeos_fs_node_t *node, uint64_t offset, void *buf, uint32_t len);
int ext2_op_list(void *fsv, const char *path, uint32_t index, char *name, uint32_t name_cap,
                 uint64_t *out_size, int *out_is_dir);
/* `n` bytes at byte `off` inside a block, read through the sectors under them: a piece
 * of a block, for what needs a descriptor or an inode and not 4 KiB. */
int ext2_read_part(vibeos_ext2_t *fs, uint64_t block, uint32_t off, uint8_t *out, uint32_t n);
/* 1 for a symbolic link whose target is in i_block (docs fs_ext4.md E4-001). */
int ext2_inode_is_fast_symlink(const vibeos_ext2_t *fs, const uint8_t *inode);

/* ---- the journal (ext4_journal.c) ----
 * Every sector the reader or the writer reads goes through ext2_sec_read, and
 * every sector of metadata the writer writes through ext4_j_meta_write: while
 * a transaction is open they are the journal's copy of the block. A file's
 * data goes through ext4_j_data_write, which writes it in place (ordered
 * mode) unless the block was freed by this same transaction - then what was
 * there on the disk may still be metadata the last commit names, and the data
 * is logged instead. */
int ext2_sec_read(vibeos_ext2_t *fs, uint64_t lba, void *buf);
int ext4_j_meta_write(vibeos_ext2_t *fs, uint64_t lba, const void *buf);
int ext4_j_data_write(vibeos_ext2_t *fs, uint64_t block, const uint8_t *buf);
void ext4_j_freed(vibeos_ext2_t *fs, uint64_t start, uint64_t count);
/* At mount, for the writer: find the journal and decide whether writes go
 * through it. 0, or -1 for a journal this driver will not write (the volume
 * is then read-only). Leaves fs->j.on 0 for a volume without one. */
int ext4_j_load(vibeos_ext2_t *fs);
/* At mount, for a volume whose superblock says RECOVER: replay what was
 * committed and leave the journal empty and the flag clear. */
int ext4_j_recover(vibeos_ext2_t *fs);
/* Around every operation that changes the volume. end commits whatever the
 * operation logged, whether it succeeded or not, and returns 0 or -EIO. */
int ext4_j_begin(vibeos_ext2_t *fs);
int ext4_j_end(vibeos_ext2_t *fs);

/* A node found before its inode was freed, or before the inode went to
 * another file (E4-012): the inode has no links or no type, or its
 * i_generation is not the one the node was found with. A node with gen 0 was
 * made by somebody who did not ask (an id built by hand) and is judged by the
 * links and type alone. */
static inline int ext2_is_orphan(const vibeos_ext2_t *fs, uint64_t ino) {
    uint32_t i;

    for (i = 0; i < fs->orphan_n; i++) {
        if (fs->orphan[i] == ino) {
            return 1;
        }
    }
    return 0;
}

/* A node is stale when its inode is free or another file's. No links is free
 * unless the file is one this mount kept for an open description (E4-019). */
static inline int ext2_node_stale(const vibeos_ext2_t *fs, const vibeos_fs_node_t *node, const uint8_t *inode) {
    uint32_t gen = (uint32_t)inode[0x64] | ((uint32_t)inode[0x65] << 8) |
                   ((uint32_t)inode[0x66] << 16) | ((uint32_t)inode[0x67] << 24);
    uint32_t links = (uint32_t)inode[0x1A] | ((uint32_t)inode[0x1B] << 8);
    uint32_t mode = (uint32_t)inode[0] | ((uint32_t)inode[1] << 8);

    return (links == 0u && !ext2_is_orphan(fs, node->id)) || mode == 0u || (node->gen != 0u && node->gen != gen);
}

static inline uint32_t ext2_inode_gen(const uint8_t *inode) {
    return (uint32_t)inode[0x64] | ((uint32_t)inode[0x65] << 8) |
           ((uint32_t)inode[0x66] << 16) | ((uint32_t)inode[0x67] << 24);
}

#endif
