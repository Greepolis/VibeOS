/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ext4, written (docs/filesystem-design/ext4.md step 2; no journal yet).
 *
 * The reader in ext2.c knows the format; this file changes it, and the whole
 * of what it promises is that the volume it leaves is one `e2fsck -fn` calls
 * clean - which scripts/dev/verify-ext4.sh asks. So everything the format
 * keeps twice is kept twice here: the free counts in the superblock and in
 * every group descriptor beside the bitmaps they count, the checksums over
 * the superblock, descriptors, bitmaps, inodes, extent blocks and directory
 * blocks, the link counts beside the directory entries, i_blocks beside the
 * extents.
 *
 * Only volumes with extents and metadata_csum are written, and only through
 * this file; ext2 stays read-only. A volume is "writable" for exactly what
 * this file keeps true (ext4_w_mount); anything else is mounted for reading.
 *
 * Scratch. The driver has no allocator and a volume is written while another
 * is (the VFS serialises calls into ONE volume, .serialize), so the scratch is
 * the volume's own state: wbuf is the operation's block - a directory block, a
 * data block being merged, an extent leaf being written - and wbuf2 is the
 * allocator's, for a bitmap. wbuf2 is never held across a call, so any
 * function may allocate or free with wbuf in use; and nothing that holds wbuf
 * calls something else that fills it. The list of a file's extents (fs->ext)
 * is the third piece: a file's tree is read into it, changed there, and
 * written back whole, which keeps merging and splitting out of the on-disk
 * format.
 *
 * The kernel stack is four pages, so nothing here puts a block on it; the
 * reader's own helpers that do (a directory scan, a tree descent) are called
 * one at a time and never from inside each other.
 */

/* The compiler's own, not <string.h>'s: with fortify on, a memset into a buffer
 * the compiler can size becomes a call to __memset_chk, which the freestanding
 * kernel image does not have. The plain calls it lowers these to are the
 * runtime's (boot/freestanding_runtime.c). */
#define memset(d, v, n) __builtin_memset((d), (v), (n))
#define memcpy(d, s, n) __builtin_memcpy((d), (s), (n))
#define memcmp(a, b, n) __builtin_memcmp((a), (b), (n))

#include "vibeos/storage.h"
#include "vibeos/ext2.h"
#include "vibeos/mbz.h"
#include "vibeos/abi_linux.h"
#include "ext2_internal.h"

#define BG_INODE_UNINIT 0x0001u
#define BG_BLOCK_UNINIT 0x0002u
#define GD_MAX 64u            /* a descriptor is 32 or 64 bytes here */
#define NLINK_MAX 65000u      /* where ext4 stops counting a file's names (and, without dir_nlink, a directory's) */
#define DIR_TAIL 12u          /* the checksum entry that ends a directory block */
#define EXT_CAP_MAX 1360u     /* the volume state's extent list: four leaves of 340 */
#define INODE_FLAG_HUGE 0x00040000u
#define S_ISDIR_(m) (((m) & VIBEOS_S_IFMT) == VIBEOS_S_IFDIR)

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void st16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void st32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int bit_get(const uint8_t *b, uint32_t i) {
    return (b[i >> 3] >> (i & 7u)) & 1;
}

static void bit_set(uint8_t *b, uint32_t i) {
    b[i >> 3] = (uint8_t)(b[i >> 3] | (1u << (i & 7u)));
}

static void bit_clr(uint8_t *b, uint32_t i) {
    b[i >> 3] = (uint8_t)(b[i >> 3] & ~(1u << (i & 7u)));
}

static uint32_t crc32c(uint32_t seed, const void *p, uint32_t n) {
    return vibeos_ext2_crc32c(seed, p, n);
}

/* ---- mount ---------------------------------------------------------------- */

/* What the writer keeps true is a short list, and a volume outside it is read
 * and not written: a feature the writer does not know is a structure it would
 * leave stale (a quota file, a project id, the second superblock of
 * sparse_super2). The compat and ro_compat words are the ones that matter for
 * a writer; the incompat ones are already the reader's. */
void ext4_w_mount(vibeos_ext2_t *fs) {
    uint32_t compat, known_ro;

    fs->writable = 0;
    if (vibeos_blockcache_read(fs->cache, fs->part_lba + 2u, fs->sb) != 0 ||
        vibeos_blockcache_read(fs->cache, fs->part_lba + 3u, fs->sb + VIBEOS_BLOCK_SIZE) != 0) {
        return;
    }
    compat = le32(fs->sb + 0x5Cu);
    known_ro = RO_COMPAT_SPARSE_SUPER | RO_COMPAT_LARGE_FILE | RO_COMPAT_HUGE_FILE | RO_COMPAT_DIR_NLINK |
               RO_COMPAT_EXTRA_ISIZE | RO_COMPAT_METADATA_CSUM;
    if ((fs->incompat & INCOMPAT_EXTENTS) == 0u || (fs->incompat & INCOMPAT_FILETYPE) == 0u || !fs->csum ||
        (fs->ro_compat & ~known_ro) != 0u ||
        (compat & ~(COMPAT_HAS_JOURNAL | COMPAT_EXT_ATTR | COMPAT_RESIZE_INODE | COMPAT_DIR_INDEX)) != 0u ||
        (fs->incompat & INCOMPAT_RECOVER) != 0u ||
        fs->inode_size != 256u || fs->desc_size > GD_MAX || fs->block_size > 4096u ||
        (le16(fs->sb + 0x3Au) & 0x2u) != 0u ||   /* the volume says it has errors: e2fsck first */
        fs->cache == 0 || fs->cache->dev == 0 || fs->cache->dev->write == 0) {
        return;
    }
    fs->writable = 1;
}

/* ---- the superblock ------------------------------------------------------- */

static int sb_store(vibeos_ext2_t *fs) {
    st32(fs->sb + 0x3FCu, crc32c(~0u, fs->sb, 0x3FCu));
    if (vibeos_blockcache_write(fs->cache, fs->part_lba + 2u, fs->sb) != 0 ||
        vibeos_blockcache_write(fs->cache, fs->part_lba + 3u, fs->sb + VIBEOS_BLOCK_SIZE) != 0) {
        return -VIBEOS_EIO;
    }
    return 0;
}

static uint64_t sb_free_blocks(const vibeos_ext2_t *fs) {
    uint64_t v = le32(fs->sb + 0x0Cu);

    if ((fs->incompat & INCOMPAT_64BIT) != 0u) {
        v |= (uint64_t)le32(fs->sb + 0x158u) << 32;
    }
    return v;
}

static int sb_free_add(vibeos_ext2_t *fs, int64_t blocks, int32_t inodes) {
    uint64_t fb = sb_free_blocks(fs) + (uint64_t)blocks;

    st32(fs->sb + 0x0Cu, (uint32_t)fb);
    if ((fs->incompat & INCOMPAT_64BIT) != 0u) {
        st32(fs->sb + 0x158u, (uint32_t)(fb >> 32));
    }
    st32(fs->sb + 0x10u, le32(fs->sb + 0x10u) + (uint32_t)inodes);
    return sb_store(fs);
}

/* ---- blocks --------------------------------------------------------------- */

static int blk_write(vibeos_ext2_t *fs, uint64_t block, const uint8_t *buf) {
    uint32_t sectors = fs->block_size / VIBEOS_BLOCK_SIZE, i;

    if (block >= fs->blocks_count || block < fs->first_data_block) {
        vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, block);
        return -VIBEOS_EIO;
    }
    for (i = 0; i < sectors; i++) {
        if (vibeos_blockcache_write(fs->cache, fs->part_lba + block * sectors + i,
                                    buf + (uint64_t)i * VIBEOS_BLOCK_SIZE) != 0) {
            return -VIBEOS_EIO;
        }
    }
    return 0;
}

/* ---- group descriptors ---------------------------------------------------- */

static uint16_t gd_csum(const vibeos_ext2_t *fs, uint32_t g, const uint8_t *d) {
    static const uint8_t zero[2] = { 0, 0 };
    uint32_t c = ext2_le32_crc(fs->csum_seed, g);

    c = crc32c(c, d, 0x1Eu);
    c = crc32c(c, zero, 2u);
    if (fs->desc_size > 0x20u) {
        c = crc32c(c, d + 0x20u, fs->desc_size - 0x20u);
    }
    return (uint16_t)c;
}

static int gd_read(vibeos_ext2_t *fs, uint32_t g, uint8_t *d) {
    uint64_t at = (uint64_t)g * fs->desc_size;

    if (g >= fs->group_count ||
        ext2_read_part(fs, fs->group_desc_block + at / fs->block_size, (uint32_t)(at % fs->block_size), d,
                       fs->desc_size) != 0) {
        return -VIBEOS_EIO;
    }
    if (gd_csum(fs, g, d) != le16(d + 0x1Eu)) {
        ext2_csum_bad(g);
        return -VIBEOS_EIO;
    }
    return 0;
}

static int gd_write(vibeos_ext2_t *fs, uint32_t g, uint8_t *d) {
    uint8_t sector[VIBEOS_BLOCK_SIZE];
    uint64_t at = (uint64_t)g * fs->desc_size, lba;
    uint32_t off = (uint32_t)(at % fs->block_size), i;

    st16(d + 0x1Eu, gd_csum(fs, g, d));
    lba = fs->part_lba + (fs->group_desc_block + at / fs->block_size) * (fs->block_size / VIBEOS_BLOCK_SIZE) +
          off / VIBEOS_BLOCK_SIZE;
    if (vibeos_blockcache_read(fs->cache, lba, sector) != 0) {
        return -VIBEOS_EIO;
    }
    for (i = 0; i < fs->desc_size; i++) {
        sector[off % VIBEOS_BLOCK_SIZE + i] = d[i];
    }
    return vibeos_blockcache_write(fs->cache, lba, sector) == 0 ? 0 : -VIBEOS_EIO;
}

/* The descriptor's counts are split in two halves with 64-bit volumes. */
static int gd_wide(const vibeos_ext2_t *fs) {
    return fs->desc_size >= 64u;
}

static uint32_t gd_get16x2(const vibeos_ext2_t *fs, const uint8_t *d, uint32_t lo, uint32_t hi) {
    uint32_t v = le16(d + lo);

    if (gd_wide(fs)) {
        v |= (uint32_t)le16(d + hi) << 16;
    }
    return v;
}

static void gd_put16x2(const vibeos_ext2_t *fs, uint8_t *d, uint32_t lo, uint32_t hi, uint32_t v) {
    st16(d + lo, v);
    if (gd_wide(fs)) {
        st16(d + hi, v >> 16);
    }
}

#define GD_FREE_BLOCKS(fs, d)     gd_get16x2(fs, d, 0x0Cu, 0x2Cu)
#define GD_FREE_INODES(fs, d)     gd_get16x2(fs, d, 0x0Eu, 0x2Eu)
#define GD_USED_DIRS(fs, d)       gd_get16x2(fs, d, 0x10u, 0x30u)
#define GD_ITABLE_UNUSED(fs, d)   gd_get16x2(fs, d, 0x1Cu, 0x32u)

static uint64_t gd_get64(const vibeos_ext2_t *fs, const uint8_t *d, uint32_t lo, uint32_t hi) {
    uint64_t v = le32(d + lo);

    if (gd_wide(fs)) {
        v |= (uint64_t)le32(d + hi) << 32;
    }
    return v;
}

/* ---- bitmaps -------------------------------------------------------------- */

static uint64_t g_start(const vibeos_ext2_t *fs, uint32_t g) {
    return (uint64_t)fs->first_data_block + (uint64_t)g * fs->blocks_per_group;
}

/* How many blocks the group really has: the last one is short. */
static uint32_t g_blocks(const vibeos_ext2_t *fs, uint32_t g) {
    uint64_t left = fs->blocks_count - g_start(fs, g);

    return left < fs->blocks_per_group ? (uint32_t)left : fs->blocks_per_group;
}

/* sparse_super keeps a copy of the superblock and the descriptors in groups 0,
 * 1 and the powers of 3, 5 and 7. */
static int group_has_super(const vibeos_ext2_t *fs, uint32_t g) {
    uint32_t p;

    if ((fs->ro_compat & RO_COMPAT_SPARSE_SUPER) == 0u || g <= 1u) {
        return 1;
    }
    for (p = 3u; p <= 7u; p += 2u) {
        uint64_t x = p;

        while (x < g) {
            x *= p;
        }
        if (x == g) {
            return 1;
        }
    }
    return 0;
}

static void bitmap_pad(const vibeos_ext2_t *fs, uint8_t *buf, uint32_t valid_bits, uint32_t group_bits) {
    uint32_t i;

    /* Bits for blocks or inodes the group does not have are set, and so is
     * everything in the block after the bitmap's own bytes. */
    for (i = valid_bits; i < group_bits; i++) {
        bit_set(buf, i);
    }
    for (i = group_bits / 8u; i < fs->block_size; i++) {
        buf[i] = 0xFFu;
    }
}

/* The block bitmap of a group nobody has allocated from (BLOCK_UNINIT): what
 * mkfs left unwritten and Linux builds on first use - the copy of the
 * superblock and descriptors the group carries, and whatever of ANY group's
 * bitmaps and inode table lies inside it (flex_bg puts sixteen groups' worth
 * in group 0, and a group can hold another's). */
static int build_block_bitmap(vibeos_ext2_t *fs, uint32_t g, uint8_t *buf) {
    uint8_t d[GD_MAX];
    uint64_t gs = g_start(fs, g);
    uint32_t nb = g_blocks(fs, g), h, i;
    uint32_t itable_blocks = (fs->inodes_per_group * fs->inode_size + fs->block_size - 1u) / fs->block_size;

    memset(buf, 0, fs->block_size);
    if (group_has_super(fs, g)) {
        uint32_t gdt = (uint32_t)(((uint64_t)fs->group_count * fs->desc_size + fs->block_size - 1u) / fs->block_size);
        uint32_t n = 1u + gdt + le16(fs->sb + 0xCEu);

        for (i = 0; i < n && i < nb; i++) {
            bit_set(buf, i);
        }
    }
    for (h = 0; h < fs->group_count; h++) {
        uint64_t at[3];
        uint32_t k;

        if (gd_read(fs, h, d) != 0) {
            return -VIBEOS_EIO;
        }
        at[0] = gd_get64(fs, d, 0x00u, 0x20u);
        at[1] = gd_get64(fs, d, 0x04u, 0x24u);
        at[2] = gd_get64(fs, d, 0x08u, 0x28u);
        for (k = 0; k < 3u; k++) {
            uint32_t len = k == 2u ? itable_blocks : 1u;

            for (i = 0; i < len; i++) {
                uint64_t b = at[k] + i;

                if (b >= gs && b < gs + nb) {
                    bit_set(buf, (uint32_t)(b - gs));
                }
            }
        }
    }
    bitmap_pad(fs, buf, nb, fs->blocks_per_group);
    return 0;
}

#ifdef VIBEOS_EXT2_FUZZ_CANARY
/* Test hook for the exact production helper used when an ext4 group bitmap
 * is marked BLOCK_UNINIT. It is compiled only into the libFuzzer target. */
int vibeos_ext4_fuzz_build_block_bitmap(vibeos_ext2_t *fs, uint32_t group) {
    if (!fs || group >= fs->group_count) {
        return -1;
    }
    return build_block_bitmap(fs, group, fs->wbuf2);
}
#endif

static uint32_t bitmap_csum_get(const vibeos_ext2_t *fs, const uint8_t *d, uint32_t lo, uint32_t hi) {
    return gd_get16x2(fs, d, lo, hi);
}

/* A group's block bitmap into `buf` (wbuf2), checked against the descriptor. */
static int bb_load(vibeos_ext2_t *fs, uint32_t g, const uint8_t *d, uint8_t *buf) {
    uint32_t want, got;

    if ((le16(d + 0x12u) & BG_BLOCK_UNINIT) != 0u) {
        return build_block_bitmap(fs, g, buf);
    }
    if (ext2_read_block(fs, gd_get64(fs, d, 0x00u, 0x20u), buf) != 0) {
        return -VIBEOS_EIO;
    }
    got = crc32c(fs->csum_seed, buf, fs->blocks_per_group / 8u);
    want = bitmap_csum_get(fs, d, 0x18u, 0x38u);
    if (!gd_wide(fs)) {
        got &= 0xFFFFu;
    }
    if (got != want) {
        ext2_csum_bad(g);
        return -VIBEOS_EIO;
    }
    return 0;
}

static int bb_store(vibeos_ext2_t *fs, uint8_t *d, const uint8_t *buf) {
    uint32_t c = crc32c(fs->csum_seed, buf, fs->blocks_per_group / 8u);

    gd_put16x2(fs, d, 0x18u, 0x38u, c);
    return blk_write(fs, gd_get64(fs, d, 0x00u, 0x20u), buf);
}

static int ib_load(vibeos_ext2_t *fs, uint32_t g, const uint8_t *d, uint8_t *buf) {
    uint32_t want, got;

    if ((le16(d + 0x12u) & BG_INODE_UNINIT) != 0u) {
        memset(buf, 0, fs->block_size);
        bitmap_pad(fs, buf, fs->inodes_per_group, fs->inodes_per_group);
        return 0;
    }
    if (ext2_read_block(fs, gd_get64(fs, d, 0x04u, 0x24u), buf) != 0) {
        return -VIBEOS_EIO;
    }
    got = crc32c(fs->csum_seed, buf, fs->inodes_per_group / 8u);
    want = bitmap_csum_get(fs, d, 0x1Au, 0x3Au);
    if (!gd_wide(fs)) {
        got &= 0xFFFFu;
    }
    if (got != want) {
        ext2_csum_bad(g);
        return -VIBEOS_EIO;
    }
    return 0;
}

static int ib_store(vibeos_ext2_t *fs, uint8_t *d, const uint8_t *buf) {
    uint32_t c = crc32c(fs->csum_seed, buf, fs->inodes_per_group / 8u);

    gd_put16x2(fs, d, 0x1Au, 0x3Au, c);
    return blk_write(fs, gd_get64(fs, d, 0x04u, 0x24u), buf);
}

/* ---- allocating and freeing blocks ---------------------------------------- */

/* A run of free blocks in group `g` at or after `start_bit`, up to `want`.
 * 0 with the run; 1 when the group has none there; negative on a failure. */
static int balloc_group(vibeos_ext2_t *fs, uint32_t g, uint32_t start_bit, uint32_t want, uint64_t *first,
                        uint32_t *got) {
    uint8_t d[GD_MAX];
    uint8_t *bm = fs->wbuf2;
    uint32_t nb = g_blocks(fs, g), b, n;

    if (gd_read(fs, g, d) != 0) {
        return -VIBEOS_EIO;
    }
    if (GD_FREE_BLOCKS(fs, d) == 0u) {
        return 1;
    }
    if (bb_load(fs, g, d, bm) != 0) {
        return -VIBEOS_EIO;
    }
    for (b = start_bit; b < nb && bit_get(bm, b); b++) {
    }
    if (b >= nb) {
        return 1;
    }
    for (n = 0; n < want && b + n < nb && !bit_get(bm, b + n); n++) {
        bit_set(bm, b + n);
    }
    if (bb_store(fs, d, bm) != 0) {
        return -VIBEOS_EIO;
    }
    gd_put16x2(fs, d, 0x0Cu, 0x2Cu, GD_FREE_BLOCKS(fs, d) - n);
    /* The bitmap is on the volume now, so the group is no longer one whose
     * bitmap is implied. */
    st16(d + 0x12u, le16(d + 0x12u) & ~BG_BLOCK_UNINIT);
    if (gd_write(fs, g, d) != 0) {
        return -VIBEOS_EIO;
    }
    *first = g_start(fs, g) + b;
    *got = n;
    return sb_free_add(fs, -(int64_t)n, 0) == 0 ? 0 : -VIBEOS_EIO;
}

/* Up to `want` contiguous blocks, as near `goal` as there are: the group of the
 * goal from the goal on, then the groups after it, then the start of its own.
 * A file's next block is asked for at the end of its last, so an append that
 * can extend the extent does. */
static int balloc(vibeos_ext2_t *fs, uint64_t goal, uint32_t want, uint64_t *start, uint32_t *got) {
    uint32_t g0, b0, k;

    if (sb_free_blocks(fs) == 0u) {
        return -VIBEOS_ENOSPC;
    }
    if (want == 0u || want > 32768u) {
        want = want == 0u ? 1u : 32768u;
    }
    if (goal < fs->first_data_block || goal >= fs->blocks_count) {
        goal = fs->first_data_block;
    }
    g0 = (uint32_t)((goal - fs->first_data_block) / fs->blocks_per_group);
    b0 = (uint32_t)((goal - fs->first_data_block) % fs->blocks_per_group);
    for (k = 0; k <= fs->group_count; k++) {
        uint32_t g = (g0 + k) % fs->group_count;
        int r;

        if (k == fs->group_count && b0 == 0u) {
            break;
        }
        r = balloc_group(fs, g, k == 0u ? b0 : 0u, want, start, got);
        if (r <= 0) {
            return r;
        }
    }
    return -VIBEOS_ENOSPC;
}

static int bfree(vibeos_ext2_t *fs, uint64_t start, uint64_t count) {
    uint8_t d[GD_MAX];
    uint8_t *bm = fs->wbuf2;

    while (count > 0u) {
        uint32_t g, bit, nb, n, freed = 0, i;

        if (start < fs->first_data_block || start >= fs->blocks_count) {
            vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, start);
            return -VIBEOS_EIO;
        }
        g = (uint32_t)((start - fs->first_data_block) / fs->blocks_per_group);
        bit = (uint32_t)((start - fs->first_data_block) % fs->blocks_per_group);
        nb = g_blocks(fs, g);
        n = count < (uint64_t)(nb - bit) ? (uint32_t)count : nb - bit;
        if (gd_read(fs, g, d) != 0) {
            return -VIBEOS_EIO;
        }
        if ((le16(d + 0x12u) & BG_BLOCK_UNINIT) != 0u) {
            /* Nothing was ever allocated in this group: a block named inside
             * it is a pointer from a corrupt tree, and freeing it would write
             * a bitmap the group was never meant to have. */
            vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, start);
            return -VIBEOS_EIO;
        }
        if (bb_load(fs, g, d, bm) != 0) {
            return -VIBEOS_EIO;
        }
        for (i = 0; i < n; i++) {
            if (bit_get(bm, bit + i)) {
                bit_clr(bm, bit + i);
                freed++;
            }
        }
        if (bb_store(fs, d, bm) != 0) {
            return -VIBEOS_EIO;
        }
        gd_put16x2(fs, d, 0x0Cu, 0x2Cu, GD_FREE_BLOCKS(fs, d) + freed);
        if (gd_write(fs, g, d) != 0 || sb_free_add(fs, (int64_t)freed, 0) != 0) {
            return -VIBEOS_EIO;
        }
        start += n;
        count -= n;
    }
    return 0;
}

/* ---- inodes: allocating, reading, writing --------------------------------- */

static int ialloc(vibeos_ext2_t *fs, uint32_t parent, int is_dir, uint32_t *out) {
    uint8_t d[GD_MAX];
    uint8_t *bm = fs->wbuf2;
    uint32_t g0, k;

    if (le32(fs->sb + 0x10u) == 0u) {
        return -VIBEOS_ENOSPC;
    }
    g0 = parent > 0u ? (parent - 1u) / fs->inodes_per_group : 0u;
    if (g0 >= fs->group_count) {
        g0 = 0u;
    }
    for (k = 0; k < fs->group_count; k++) {
        uint32_t g = (g0 + k) % fs->group_count, i, used_to;

        if (gd_read(fs, g, d) != 0) {
            return -VIBEOS_EIO;
        }
        if (GD_FREE_INODES(fs, d) == 0u) {
            continue;
        }
        if (ib_load(fs, g, d, bm) != 0) {
            return -VIBEOS_EIO;
        }
        for (i = 0; i < fs->inodes_per_group && bit_get(bm, i); i++) {
        }
        if (i >= fs->inodes_per_group) {
            continue;
        }
        bit_set(bm, i);
        if (ib_store(fs, d, bm) != 0) {
            return -VIBEOS_EIO;
        }
        gd_put16x2(fs, d, 0x0Eu, 0x2Eu, GD_FREE_INODES(fs, d) - 1u);
        if (is_dir) {
            gd_put16x2(fs, d, 0x10u, 0x30u, GD_USED_DIRS(fs, d) + 1u);
        }
        /* The tail of the table nobody has used is not read by Linux or
         * e2fsck, so the count of it moves down past this inode. */
        used_to = fs->inodes_per_group - GD_ITABLE_UNUSED(fs, d);
        if (i + 1u > used_to) {
            gd_put16x2(fs, d, 0x1Cu, 0x32u, fs->inodes_per_group - (i + 1u));
        }
        st16(d + 0x12u, le16(d + 0x12u) & ~BG_INODE_UNINIT);
        if (gd_write(fs, g, d) != 0 || sb_free_add(fs, 0, -1) != 0) {
            return -VIBEOS_EIO;
        }
        *out = g * fs->inodes_per_group + i + 1u;
        return 0;
    }
    return -VIBEOS_ENOSPC;
}

static int ifree(vibeos_ext2_t *fs, uint32_t ino, int is_dir) {
    uint8_t d[GD_MAX];
    uint8_t *bm = fs->wbuf2;
    uint32_t g = (ino - 1u) / fs->inodes_per_group, i = (ino - 1u) % fs->inodes_per_group;

    if (g >= fs->group_count || gd_read(fs, g, d) != 0 || ib_load(fs, g, d, bm) != 0) {
        return -VIBEOS_EIO;
    }
    if (!bit_get(bm, i)) {
        vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, ino);   /* freeing an inode that was free */
        return -VIBEOS_EIO;
    }
    bit_clr(bm, i);
    if (ib_store(fs, d, bm) != 0) {
        return -VIBEOS_EIO;
    }
    gd_put16x2(fs, d, 0x0Eu, 0x2Eu, GD_FREE_INODES(fs, d) + 1u);
    if (is_dir && GD_USED_DIRS(fs, d) > 0u) {
        gd_put16x2(fs, d, 0x10u, 0x30u, GD_USED_DIRS(fs, d) - 1u);
    }
    if (gd_write(fs, g, d) != 0 || sb_free_add(fs, 0, 1) != 0) {
        return -VIBEOS_EIO;
    }
    return 0;
}

/* The inode's checksum, over the whole inode with both halves read as zero
 * (the reader's piecewise sum is exactly this). */
static void inode_csum_set(const vibeos_ext2_t *fs, uint32_t ino, uint8_t *inode) {
    uint32_t c;

    st16(inode + 0x7Cu, 0);
    st16(inode + 0x82u, 0);
    c = crc32c(ext2_inode_seed(fs, ino, inode), inode, fs->inode_size);
    st16(inode + 0x7Cu, c);
    st16(inode + 0x82u, c >> 16);
}

static int inode_put(vibeos_ext2_t *fs, uint32_t ino, uint8_t *inode) {
    uint8_t sector[VIBEOS_BLOCK_SIZE];
    uint64_t table, blk;
    uint32_t group = (ino - 1u) / fs->inodes_per_group, index = (ino - 1u) % fs->inodes_per_group;
    uint32_t per = fs->block_size / fs->inode_size, off, i;
    uint64_t lba;

    if (le16(inode + 0x80u) < 4u) {
        return -VIBEOS_EOPNOTSUPP;   /* an inode whose checksum is 16 bits wide */
    }
    if (group >= fs->group_count || ext2_inode_table(fs, group, &table) != 0) {
        return -VIBEOS_EIO;
    }
    inode_csum_set(fs, ino, inode);
    blk = table + index / per;
    off = (index % per) * fs->inode_size;
    if (blk >= fs->blocks_count) {
        return -VIBEOS_EIO;
    }
    lba = fs->part_lba + blk * (fs->block_size / VIBEOS_BLOCK_SIZE) + off / VIBEOS_BLOCK_SIZE;
    if (vibeos_blockcache_read(fs->cache, lba, sector) != 0) {
        return -VIBEOS_EIO;
    }
    for (i = 0; i < fs->inode_size; i++) {
        sector[off % VIBEOS_BLOCK_SIZE + i] = inode[i];
    }
    /* What the reader holds of this inode is the old one. */
    __atomic_store_n(&fs->cached_ino, 0u, __ATOMIC_RELEASE);
    return vibeos_blockcache_write(fs->cache, lba, sector) == 0 ? 0 : -VIBEOS_EIO;
}

static int inode_get(vibeos_ext2_t *fs, uint32_t ino, uint8_t *inode) {
    return ext2_read_inode(fs, ino, inode) == 0 ? 0 : -VIBEOS_EIO;
}

/* ---- times ---------------------------------------------------------------- */

#define NS 1000000000ull

/* i_atime, i_ctime, i_mtime and their _extra words (nanoseconds in the top 30
 * bits, two more bits of seconds below). The extra words exist when the
 * inode's extra size reaches them. */
static void time_set(uint8_t *inode, uint32_t off, uint32_t extra, uint64_t ns) {
    uint64_t secs = ns / NS;

    st32(inode + off, (uint32_t)secs);
    if (le16(inode + 0x80u) >= extra + 4u - 0x80u) {
        st32(inode + extra, (uint32_t)(((ns % NS) << 2) | ((secs >> 32) & 3u)));
    }
}

static uint64_t time_get(const uint8_t *inode, uint32_t off, uint32_t extra) {
    uint64_t secs = le32(inode + off), nsec = 0;

    if (le16(inode + 0x80u) >= extra + 4u - 0x80u) {
        uint32_t x = le32(inode + extra);

        secs |= (uint64_t)(x & 3u) << 32;
        nsec = x >> 2;
    }
    return secs * NS + nsec;
}

#define T_ATIME 0x08u, 0x8Cu
#define T_CTIME 0x0Cu, 0x84u
#define T_MTIME 0x10u, 0x88u

/* A change to a file's data or its directory entries: mtime and ctime. */
static void touch_mc(uint8_t *inode) {
    uint64_t now = vibeos_fs_now_ns();

    time_set(inode, T_MTIME, now);
    time_set(inode, T_CTIME, now);
}

static void touch_c(uint8_t *inode) {
    time_set(inode, T_CTIME, vibeos_fs_now_ns());
}

/* ---- extents: a file's tree as a list ------------------------------------- */

static uint32_t e_len(const vibeos_ext2_t *fs, uint32_t i) {
    uint32_t l = fs->ext[i].len;

    return l > 32768u ? l - 32768u : l;
}

static int e_uninit(const vibeos_ext2_t *fs, uint32_t i) {
    return fs->ext[i].len > 32768u;
}

static uint64_t e_start(const vibeos_ext2_t *fs, uint32_t i) {
    return ((uint64_t)fs->ext[i].start_hi << 32) | fs->ext[i].start_lo;
}

static void e_put(vibeos_ext2_t *fs, uint32_t i, uint32_t lblk, uint32_t len, int uninit, uint64_t start) {
    fs->ext[i].lblk = lblk;
    fs->ext[i].len = (uint16_t)(uninit ? len + 32768u : len);
    fs->ext[i].start_hi = (uint16_t)(start >> 32);
    fs->ext[i].start_lo = (uint32_t)start;
}

/* What fits: four extents in the inode, or four leaf blocks of (bs-12)/12
 * under it. A deeper tree is not written; one this long is EFBIG. */
static uint32_t ext_per_leaf(const vibeos_ext2_t *fs) {
    return (fs->block_size - 12u) / 12u;
}

static uint32_t ext_cap(const vibeos_ext2_t *fs) {
    uint32_t c = 4u * ext_per_leaf(fs);

    return c > EXT_CAP_MAX ? EXT_CAP_MAX : c;
}

static int leaf_csum_ok(vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode, const uint8_t *leaf) {
    uint32_t at = 12u + le16(leaf + 4u) * 12u;

    return at + 4u <= fs->block_size &&
           crc32c(ext2_inode_seed(fs, ino, inode), leaf, at) == le32(leaf + at);
}

/* Read the file's extents into fs->ext. Uses wbuf for the leaves. */
static int ext_load(vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode) {
    const uint8_t *r = inode + 40u;
    uint32_t entries = le16(r + 2u), depth = le16(r + 6u), n = 0, i, k;
    uint8_t *leaf = fs->wbuf;

    fs->ext_n = 0;
    if ((le32(inode + 0x20u) & INODE_FLAG_EXTENTS) == 0u) {
        return -VIBEOS_EOPNOTSUPP;
    }
    if (le16(r) != EXTENT_MAGIC || entries > 4u || le16(r + 4u) < entries) {
        return -VIBEOS_EIO;
    }
    if (depth > 1u) {
        return -VIBEOS_EFBIG;   /* a tree Linux grew deeper is read, not rewritten */
    }
    if (depth == 0u) {
        for (i = 0; i < entries; i++) {
            const uint8_t *e = r + 12u + i * 12u;

            fs->ext[n].lblk = le32(e);
            fs->ext[n].len = le16(e + 4u);
            fs->ext[n].start_hi = le16(e + 6u);
            fs->ext[n].start_lo = le32(e + 8u);
            n++;
        }
    } else {
        for (k = 0; k < entries; k++) {
            const uint8_t *x = r + 12u + k * 12u;
            uint64_t child = le32(x + 4u) | ((uint64_t)le16(x + 8u) << 32);
            uint32_t cnt;

            if (ext2_read_block(fs, child, leaf) != 0) {
                return -VIBEOS_EIO;
            }
            cnt = le16(leaf + 2u);
            if (le16(leaf) != EXTENT_MAGIC || le16(leaf + 6u) != 0u || cnt > le16(leaf + 4u) ||
                12u + (uint32_t)le16(leaf + 4u) * 12u + 4u > fs->block_size || !leaf_csum_ok(fs, ino, inode, leaf)) {
                ext2_csum_bad(child);
                return -VIBEOS_EIO;
            }
            for (i = 0; i < cnt; i++) {
                const uint8_t *e = leaf + 12u + i * 12u;

                if (n >= ext_cap(fs)) {
                    return -VIBEOS_EFBIG;
                }
                fs->ext[n].lblk = le32(e);
                fs->ext[n].len = le16(e + 4u);
                fs->ext[n].start_hi = le16(e + 6u);
                fs->ext[n].start_lo = le32(e + 8u);
                n++;
            }
        }
    }
    /* Sorted and not overlapping, or the list is not a file's. */
    for (i = 0; i < n; i++) {
        if (e_len(fs, i) == 0u || (i > 0u && fs->ext[i].lblk < fs->ext[i - 1u].lblk + e_len(fs, i - 1u))) {
            vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, ino);
            return -VIBEOS_EIO;
        }
    }
    fs->ext_n = n;
    return 0;
}

static void set_blocks(uint8_t *inode, uint64_t blocks512) {
    st32(inode + 0x1Cu, (uint32_t)blocks512);
    st16(inode + 0x74u, (uint32_t)(blocks512 >> 32));
}

/* Write the list back as the file's tree: in the inode if it is four extents or
 * fewer, else as leaf blocks under a root of index entries. The new leaves are
 * allocated and written before the inode names them, and the old ones freed
 * after, so a failure to find room leaves the tree as it was. Sets i_blocks
 * from the list (data, leaves, the attribute block if there is one). The caller
 * writes the inode. Uses wbuf. */
static int ext_store(vibeos_ext2_t *fs, uint32_t ino, uint8_t *inode) {
    uint32_t bs = fs->block_size, per = ext_per_leaf(fs), n = fs->ext_n, nleaves = 0, nold = 0, i, k;
    uint64_t oldl[4], newl[4], data = 0, goal, blocks;
    uint8_t *r = inode + 40u, *leaf = fs->wbuf;

    if (le16(r + 6u) == 1u) {
        nold = le16(r + 2u);
        if (nold > 4u) {
            return -VIBEOS_EIO;
        }
        for (i = 0; i < nold; i++) {
            oldl[i] = le32(r + 12u + i * 12u + 4u) | ((uint64_t)le16(r + 12u + i * 12u + 8u) << 32);
        }
    }
    if (n > 4u) {
        nleaves = (n + per - 1u) / per;
        if (nleaves > 4u) {
            return -VIBEOS_EFBIG;
        }
    }
    goal = n > 0u ? e_start(fs, 0) : g_start(fs, (ino - 1u) / fs->inodes_per_group);
    for (k = 0; k < nleaves; k++) {
        uint32_t got;
        int rc = balloc(fs, goal, 1u, &newl[k], &got);

        if (rc != 0) {
            uint32_t j;

            for (j = 0; j < k; j++) {   /* the leaves already taken go back */
                (void)bfree(fs, newl[j], 1u);
            }
            return rc;
        }
        goal = newl[k] + 1u;
    }
    for (k = 0; k < nleaves; k++) {
        uint32_t first = k * per, cnt = n - first < per ? n - first : per;
        uint32_t at = 12u + per * 12u;

        memset(leaf, 0, bs);
        st16(leaf, EXTENT_MAGIC);
        st16(leaf + 2u, cnt);
        st16(leaf + 4u, per);
        for (i = 0; i < cnt; i++) {
            uint8_t *e = leaf + 12u + i * 12u;

            st32(e, fs->ext[first + i].lblk);
            st16(e + 4u, fs->ext[first + i].len);
            st16(e + 6u, fs->ext[first + i].start_hi);
            st32(e + 8u, fs->ext[first + i].start_lo);
        }
        st32(leaf + at, crc32c(ext2_inode_seed(fs, ino, inode), leaf, at));
        if (blk_write(fs, newl[k], leaf) != 0) {
            for (i = 0; i < nleaves; i++) {
                (void)bfree(fs, newl[i], 1u);
            }
            return -VIBEOS_EIO;
        }
    }
    memset(r, 0, 60u);
    st16(r, EXTENT_MAGIC);
    st16(r + 4u, 4u);
    if (nleaves > 0u) {
        st16(r + 2u, nleaves);
        st16(r + 6u, 1u);
        for (k = 0; k < nleaves; k++) {
            uint8_t *x = r + 12u + k * 12u;

            st32(x, fs->ext[k * per].lblk);
            st32(x + 4u, (uint32_t)newl[k]);
            st16(x + 8u, (uint32_t)(newl[k] >> 32));
        }
    } else {
        st16(r + 2u, n);
        for (i = 0; i < n; i++) {
            uint8_t *e = r + 12u + i * 12u;

            st32(e, fs->ext[i].lblk);
            st16(e + 4u, fs->ext[i].len);
            st16(e + 6u, fs->ext[i].start_hi);
            st32(e + 8u, fs->ext[i].start_lo);
        }
    }
    for (i = 0; i < n; i++) {
        data += e_len(fs, i);
    }
    blocks = (data + nleaves + (le32(inode + 0x68u) != 0u ? 1u : 0u)) * (bs / VIBEOS_BLOCK_SIZE);
    set_blocks(inode, blocks);
    for (i = 0; i < nold; i++) {
        if (bfree(fs, oldl[i], 1u) != 0) {
            return -VIBEOS_EIO;
        }
    }
    return 0;
}

/* The first extent whose end is above `lblk`: the one that holds it, or the next
 * one after the hole it is in. */
static uint32_t ext_search(const vibeos_ext2_t *fs, uint32_t lblk) {
    uint32_t lo = 0, hi = fs->ext_n;

    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2u;

        if ((uint64_t)fs->ext[mid].lblk + e_len(fs, mid) > lblk) {
            hi = mid;
        } else {
            lo = mid + 1u;
        }
    }
    return lo;
}

/* The volume block behind a file block, 0 for a hole; *uninit for an extent
 * that has not been written (it reads as zeros and holds whatever was there). */
static uint64_t ext_map(const vibeos_ext2_t *fs, uint32_t lblk, int *uninit) {
    uint32_t i = ext_search(fs, lblk);

    *uninit = 0;
    if (i < fs->ext_n && fs->ext[i].lblk <= lblk) {
        *uninit = e_uninit(fs, i);
        return e_start(fs, i) + (lblk - fs->ext[i].lblk);
    }
    return 0;
}

static void ext_remove(vibeos_ext2_t *fs, uint32_t i) {
    for (; i + 1u < fs->ext_n; i++) {
        fs->ext[i] = fs->ext[i + 1u];
    }
    fs->ext_n--;
}

/* Join extent `i` with its neighbours where they run on: the same kind
 * (written or not), contiguous in the file and on the volume, and not past what
 * one extent can say. This is what keeps a file written block by block one
 * extent. */
static void ext_merge_at(vibeos_ext2_t *fs, uint32_t i) {
    if (i + 1u < fs->ext_n && !e_uninit(fs, i) == !e_uninit(fs, i + 1u) &&
        fs->ext[i].lblk + e_len(fs, i) == fs->ext[i + 1u].lblk &&
        e_start(fs, i) + e_len(fs, i) == e_start(fs, i + 1u) &&
        e_len(fs, i) + e_len(fs, i + 1u) <= (e_uninit(fs, i) ? 32767u : 32768u)) {
        e_put(fs, i, fs->ext[i].lblk, e_len(fs, i) + e_len(fs, i + 1u), e_uninit(fs, i), e_start(fs, i));
        ext_remove(fs, i + 1u);
    }
    if (i > 0u) {
        ext_merge_at(fs, i - 1u);
    }
}

/* A new written run into a hole. */
static int ext_add(vibeos_ext2_t *fs, uint32_t lblk, uint32_t count, uint64_t phys) {
    uint32_t i = ext_search(fs, lblk), k;

    if (fs->ext_n >= ext_cap(fs)) {
        /* Full: it still fits if it runs onto a neighbour. */
        int joins = (i > 0u && !e_uninit(fs, i - 1u) && fs->ext[i - 1u].lblk + e_len(fs, i - 1u) == lblk &&
                     e_start(fs, i - 1u) + e_len(fs, i - 1u) == phys && e_len(fs, i - 1u) + count <= 32768u) ||
                    (i < fs->ext_n && !e_uninit(fs, i) && lblk + count == fs->ext[i].lblk &&
                     phys + count == e_start(fs, i) && count + e_len(fs, i) <= 32768u);

        if (!joins) {
            return -VIBEOS_EFBIG;
        }
    }
    for (k = fs->ext_n; k > i; k--) {
        fs->ext[k] = fs->ext[k - 1u];
    }
    fs->ext_n++;
    e_put(fs, i, lblk, count, 0, phys);
    ext_merge_at(fs, i);
    return 0;
}

/* Make the one block `lblk` of an unwritten extent a written one: the extent
 * becomes up to three, and the middle - a block this write is about to fill,
 * from zeros - runs on into a written neighbour if there is one. The rest of
 * the unwritten extent keeps its blocks, and keeps reading as zeros. */
static int ext_make_init(vibeos_ext2_t *fs, uint32_t lblk) {
    uint32_t i = ext_search(fs, lblk), f, len, before, after, extra, k;
    uint64_t s;

    if (i >= fs->ext_n || fs->ext[i].lblk > lblk || !e_uninit(fs, i)) {
        return -VIBEOS_EIO;
    }
    f = fs->ext[i].lblk;
    len = e_len(fs, i);
    s = e_start(fs, i);
    before = lblk - f;
    after = len - before - 1u;
    extra = (before > 0u ? 1u : 0u) + (after > 0u ? 1u : 0u);
    if (fs->ext_n + extra > ext_cap(fs)) {
        return -VIBEOS_EFBIG;
    }
    for (k = fs->ext_n + extra; k > i + extra; k--) {
        fs->ext[k] = fs->ext[k - extra];
    }
    fs->ext_n += extra;
    k = i;
    if (before > 0u) {
        e_put(fs, k++, f, before, 1, s);
    }
    e_put(fs, k, lblk, 1u, 0, s + before);
    if (after > 0u) {
        e_put(fs, k + 1u, lblk + 1u, after, 1, s + before + 1u);
    }
    ext_merge_at(fs, k);
    return 0;
}

/* Drop every block of the file from `keep` on, freeing them. */
static int ext_truncate(vibeos_ext2_t *fs, uint32_t keep) {
    while (fs->ext_n > 0u) {
        uint32_t i = fs->ext_n - 1u, len = e_len(fs, i);

        if (fs->ext[i].lblk >= keep) {
            if (bfree(fs, e_start(fs, i), len) != 0) {
                return -VIBEOS_EIO;
            }
            fs->ext_n--;
            continue;
        }
        if (fs->ext[i].lblk + len > keep) {
            uint32_t kept = keep - fs->ext[i].lblk;

            if (bfree(fs, e_start(fs, i) + kept, len - kept) != 0) {
                return -VIBEOS_EIO;
            }
            e_put(fs, i, fs->ext[i].lblk, kept, e_uninit(fs, i), e_start(fs, i));
        }
        break;
    }
    return 0;
}

/* ---- the node as the VFS sees it ------------------------------------------ */

static void node_fill(uint32_t ino, const uint8_t *inode, vibeos_fs_node_t *out) {
    uint32_t mode = le16(inode);

    out->id = ino;
    out->size = ext2_inode_size(inode);
    out->mode = mode;
    out->is_dir = S_ISDIR_(mode);
    out->nlink = le16(inode + 0x1Au);
    out->uid = le16(inode + 0x02u) | ((uint32_t)le16(inode + 0x78u) << 16);
    out->gid = le16(inode + 0x18u) | ((uint32_t)le16(inode + 0x7Au) << 16);
    out->atime_ns = time_get(inode, T_ATIME);
    out->mtime_ns = time_get(inode, T_MTIME);
    out->ctime_ns = time_get(inode, T_CTIME);
}

/* ---- paths ---------------------------------------------------------------- */

static uint32_t cstr_len(const char *s) {
    uint32_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/* `path` cut at its last name: the directory it is in is path[0..*plen) and the
 * name is *name[0..*nlen). A trailing slash is not a name. */
static int path_split(const char *path, uint32_t *plen, const char **name, uint32_t *nlen) {
    uint32_t n = cstr_len(path), i;

    while (n > 0u && path[n - 1u] == '/') {
        n--;
    }
    if (n == 0u) {
        return -VIBEOS_EINVAL;   /* the root has no name and no parent */
    }
    for (i = n; i > 0u && path[i - 1u] != '/'; i--) {
    }
    *name = path + i;
    *nlen = n - i;
    *plen = i;
    if (*nlen > 255u) {
        return -VIBEOS_ENAMETOOLONG;
    }
    return 0;
}

static int name_is_dot(const char *name, uint32_t nlen) {
    return (nlen == 1u && name[0] == '.') || (nlen == 2u && name[0] == '.' && name[1] == '.');
}

/* ext2_resolve for the first `len` bytes of a path. */
static uint32_t resolve_n(vibeos_ext2_t *fs, const char *path, uint32_t len, uint8_t *inode) {
    uint32_t ino = VIBEOS_EXT2_ROOT_INO, i = 0;

    if (ext2_read_inode(fs, ino, inode) != 0) {
        return 0;
    }
    while (i < len) {
        uint32_t start;

        while (i < len && path[i] == '/') {
            i++;
        }
        start = i;
        while (i < len && path[i] != '/') {
            i++;
        }
        if (i > start) {
            if (!ext2_inode_is_dir(inode)) {
                return 0;
            }
            ino = ext2_dir_find(fs, ino, inode, path + start, i - start);
            if (ino == 0u || ext2_read_inode(fs, ino, inode) != 0) {
                return 0;
            }
        }
    }
    return ino;
}

/* ---- directories ---------------------------------------------------------- */

static uint32_t rec_size(uint32_t nlen) {
    return (8u + nlen + 3u) & ~3u;
}

static void dirblk_csum_set(const vibeos_ext2_t *fs, uint32_t dir_ino, const uint8_t *dir, uint8_t *b) {
    uint8_t *t = b + fs->block_size - DIR_TAIL;

    st32(t, 0);
    st16(t + 4u, DIR_TAIL);
    t[6] = 0;
    t[7] = 0xDEu;
    st32(t + 8u, crc32c(ext2_inode_seed(fs, dir_ino, dir), b, fs->block_size - DIR_TAIL));
}

/* One block of a directory, by its number in the directory; 0 phys is a hole. */
static int dir_phys(vibeos_ext2_t *fs, uint32_t dir_ino, const uint8_t *dir, uint32_t bi, uint64_t *phys) {
    return ext2_map_block(fs, dir_ino, dir, bi, phys) == 0 ? 0 : -VIBEOS_EIO;
}

static void set_size(uint8_t *inode, uint64_t size) {
    st32(inode + 4u, (uint32_t)size);
    st32(inode + 0x6Cu, (uint32_t)(size >> 32));
}

/* A directory with an htree is a directory with index blocks the writer does not
 * keep. The first change to one turns it back into the linear directory every
 * reader accepts: block 0, which holds "." and ".." with the index root inside
 * ".."'s record, becomes a leaf; each interior index block becomes an empty
 * leaf; and the flag goes. Leaves are untouched - they were leaves already, and
 * carry their checksums. e2fsck -D rebuilds the index when somebody wants one. */
static int dir_dedex(vibeos_ext2_t *fs, uint32_t dir_ino, uint8_t *dir) {
    uint32_t bs = fs->block_size, nblocks, bi;
    uint8_t *b = fs->wbuf;

    if ((le32(dir + 0x20u) & INODE_FLAG_INDEX) == 0u) {
        return 0;
    }
    nblocks = (uint32_t)(ext2_inode_size(dir) / bs);
    for (bi = 0; bi < nblocks; bi++) {
        uint64_t phys;

        if (dir_phys(fs, dir_ino, dir, bi, &phys) != 0 || phys == 0u || ext2_read_block(fs, phys, b) != 0) {
            return -VIBEOS_EIO;
        }
        if (bi == 0u) {
            /* "." is 12 bytes; ".." runs to the end of the block with the
             * index root inside its record. */
            memset(b + 24u, 0, bs - 24u);
            st16(b + 12u + 4u, bs - 12u - DIR_TAIL);
        } else if (le32(b) == 0u && le16(b + 4u) == bs) {
            memset(b, 0, bs);
            st16(b + 4u, bs - DIR_TAIL);
        } else {
            continue;
        }
        dirblk_csum_set(fs, dir_ino, dir, b);
        if (blk_write(fs, phys, b) != 0) {
            return -VIBEOS_EIO;
        }
    }
    st32(dir + 0x20u, le32(dir + 0x20u) & ~INODE_FLAG_INDEX);
    return inode_put(fs, dir_ino, dir);
}

/* Walk the live entries of directory block `b` that hold names: calls `fn`-less,
 * so the callers below each spell the loop. This is the check they share: the
 * record is whole, aligned, and holds its name. */
static int rec_ok(const vibeos_ext2_t *fs, const uint8_t *b, uint32_t off, uint32_t limit) {
    uint32_t reclen = le16(b + off + 4u);

    (void)fs;
    return reclen >= 8u && (reclen & 3u) == 0u && off + reclen <= limit && b[off + 6u] <= reclen - 8u;
}

static int dir_add_entry(vibeos_ext2_t *fs, uint32_t dir_ino, const char *name, uint32_t nlen, uint32_t ino,
                         uint32_t ftype) {
    uint8_t dir[EXT2_MAX_INODE_SIZE];
    uint8_t *b = fs->wbuf;
    uint32_t bs = fs->block_size, need = rec_size(nlen), nblocks, bi, limit = bs - DIR_TAIL;
    uint64_t goal, nb;
    uint32_t got;
    int rc;

    if (inode_get(fs, dir_ino, dir) != 0) {
        return -VIBEOS_EIO;
    }
    if (!ext2_inode_is_dir(dir)) {
        return -VIBEOS_ENOTDIR;
    }
    rc = dir_dedex(fs, dir_ino, dir);
    if (rc != 0) {
        return rc;
    }
    nblocks = (uint32_t)(ext2_inode_size(dir) / bs);
    for (bi = 0; bi < nblocks; bi++) {
        uint64_t phys;
        uint32_t off = 0;

        if (dir_phys(fs, dir_ino, dir, bi, &phys) != 0 || phys == 0u) {
            return -VIBEOS_EIO;
        }
        if (ext2_dir_block(fs, dir_ino, dir, phys, b) != 0) {
            return -VIBEOS_EIO;
        }
        while (off < limit) {
            uint32_t reclen, used, at, room;

            if (!rec_ok(fs, b, off, limit)) {
                return -VIBEOS_EIO;
            }
            reclen = le16(b + off + 4u);
            used = le32(b + off) != 0u ? rec_size(b[off + 6u]) : 0u;
            if (reclen - used >= need) {
                at = off + used;
                room = reclen - used;
                if (used > 0u) {
                    st16(b + off + 4u, used);
                }
                st32(b + at, ino);
                st16(b + at + 4u, room);
                b[at + 6u] = (uint8_t)nlen;
                b[at + 7u] = (uint8_t)ftype;
                memcpy(b + at + 8u, name, nlen);
                dirblk_csum_set(fs, dir_ino, dir, b);
                if (blk_write(fs, phys, b) != 0) {
                    return -VIBEOS_EIO;
                }
                touch_mc(dir);
                return inode_put(fs, dir_ino, dir);
            }
            off += reclen;
        }
    }
    /* Every block is full: the directory grows by one. */
    rc = ext_load(fs, dir_ino, dir);
    if (rc != 0) {
        return rc;
    }
    goal = fs->ext_n > 0u ? e_start(fs, fs->ext_n - 1u) + e_len(fs, fs->ext_n - 1u)
                          : g_start(fs, (dir_ino - 1u) / fs->inodes_per_group);
    rc = balloc(fs, goal, 1u, &nb, &got);
    if (rc != 0) {
        return rc;
    }
    rc = ext_add(fs, nblocks, 1u, nb);
    if (rc == 0) {
        rc = ext_store(fs, dir_ino, dir);
    }
    if (rc != 0) {
        (void)bfree(fs, nb, 1u);
        return rc;
    }
    memset(b, 0, bs);
    st32(b, ino);
    st16(b + 4u, limit);
    b[6] = (uint8_t)nlen;
    b[7] = (uint8_t)ftype;
    memcpy(b + 8u, name, nlen);
    dirblk_csum_set(fs, dir_ino, dir, b);
    if (blk_write(fs, nb, b) != 0) {
        return -VIBEOS_EIO;
    }
    set_size(dir, ext2_inode_size(dir) + bs);
    touch_mc(dir);
    return inode_put(fs, dir_ino, dir);
}

/* Find `name` in a directory and, with `mode` 1, take its record out (merged
 * into the one before it, or emptied when it starts a block), or with 2 point it
 * at another inode. 0 and the inode it named, or -ENOENT. */
static int dir_edit(vibeos_ext2_t *fs, uint32_t dir_ino, const char *name, uint32_t nlen, int mode,
                    uint32_t new_ino, uint32_t new_type, uint32_t *found) {
    uint8_t dir[EXT2_MAX_INODE_SIZE];
    uint8_t *b = fs->wbuf;
    uint32_t bs = fs->block_size, nblocks, bi, limit = bs - DIR_TAIL;
    int rc;

    if (inode_get(fs, dir_ino, dir) != 0) {
        return -VIBEOS_EIO;
    }
    if (mode != 0) {
        rc = dir_dedex(fs, dir_ino, dir);
        if (rc != 0) {
            return rc;
        }
    }
    nblocks = (uint32_t)(ext2_inode_size(dir) / bs);
    for (bi = 0; bi < nblocks; bi++) {
        uint64_t phys;
        uint32_t off = 0, prev = 0xFFFFFFFFu;

        if (dir_phys(fs, dir_ino, dir, bi, &phys) != 0 || phys == 0u ||
            ext2_dir_block(fs, dir_ino, dir, phys, b) != 0) {
            return -VIBEOS_EIO;
        }
        while (off < limit) {
            uint32_t reclen;

            if (!rec_ok(fs, b, off, limit)) {
                return -VIBEOS_EIO;
            }
            reclen = le16(b + off + 4u);
            if (le32(b + off) != 0u && b[off + 6u] == nlen && memcmp(b + off + 8u, name, nlen) == 0) {
                *found = le32(b + off);
                if (mode == 0) {
                    return 0;
                }
                if (mode == 1) {
                    if (prev != 0xFFFFFFFFu) {
                        st16(b + prev + 4u, le16(b + prev + 4u) + reclen);
                    } else {
                        st32(b + off, 0);
                    }
                } else {
                    st32(b + off, new_ino);
                    b[off + 7u] = (uint8_t)new_type;
                }
                dirblk_csum_set(fs, dir_ino, dir, b);
                if (blk_write(fs, phys, b) != 0) {
                    return -VIBEOS_EIO;
                }
                touch_mc(dir);
                return inode_put(fs, dir_ino, dir);
            }
            prev = off;
            off += reclen;
        }
    }
    return -VIBEOS_ENOENT;
}

/* 1 when a directory holds nothing but "." and "..". */
static int dir_empty(vibeos_ext2_t *fs, uint32_t dir_ino) {
    uint8_t dir[EXT2_MAX_INODE_SIZE];
    uint8_t *b = fs->wbuf;
    uint32_t bs = fs->block_size, nblocks, bi;

    if (inode_get(fs, dir_ino, dir) != 0) {
        return 0;
    }
    nblocks = (uint32_t)(ext2_inode_size(dir) / bs);
    for (bi = 0; bi < nblocks; bi++) {
        uint64_t phys;
        uint32_t off = 0;

        if (dir_phys(fs, dir_ino, dir, bi, &phys) != 0 || phys == 0u ||
            ext2_dir_block(fs, dir_ino, dir, phys, b) != 0) {
            return 0;
        }
        while (off < bs) {
            if (!rec_ok(fs, b, off, bs)) {
                return 0;
            }
            if (le32(b + off) != 0u && !name_is_dot((const char *)b + off + 8u, b[off + 6u])) {
                return 0;
            }
            off += le16(b + off + 4u);
        }
    }
    return 1;
}

/* A directory's ".." (its second record, in block 0), read or pointed elsewhere. */
static int dir_dotdot(vibeos_ext2_t *fs, uint32_t dir_ino, uint32_t set_to, uint32_t *parent) {
    uint8_t dir[EXT2_MAX_INODE_SIZE];
    uint8_t *b = fs->wbuf;
    uint64_t phys;
    uint32_t off;

    if (inode_get(fs, dir_ino, dir) != 0 || dir_phys(fs, dir_ino, dir, 0, &phys) != 0 || phys == 0u ||
        ext2_dir_block(fs, dir_ino, dir, phys, b) != 0) {
        return -VIBEOS_EIO;
    }
    off = le16(b + 4u);
    if (le32(b) != dir_ino || off < 12u || off + 12u > fs->block_size || b[off + 6u] != 2u || b[off + 8u] != '.' ||
        b[off + 9u] != '.') {
        return -VIBEOS_EIO;
    }
    if (set_to == 0u) {
        *parent = le32(b + off);
        return 0;
    }
    st32(b + off, set_to);
    dirblk_csum_set(fs, dir_ino, dir, b);
    return blk_write(fs, phys, b) == 0 ? 0 : -VIBEOS_EIO;
}

/* A directory's link count: one for its own name, one for ".", one for each
 * subdirectory's "..". Past what 16 bits count, ext4 with dir_nlink stops
 * counting and says 1; without it, the subdirectory is refused. */
static int dir_links_adjust(vibeos_ext2_t *fs, uint32_t dir_ino, int delta) {
    uint8_t dir[EXT2_MAX_INODE_SIZE];
    uint32_t links;

    if (inode_get(fs, dir_ino, dir) != 0) {
        return -VIBEOS_EIO;
    }
    links = le16(dir + 0x1Au);
    if (delta > 0) {
        if (links >= NLINK_MAX || links == 1u) {
            if ((fs->ro_compat & RO_COMPAT_DIR_NLINK) == 0u) {
                return -VIBEOS_EMLINK;
            }
            links = 1u;
        } else {
            links++;
        }
    } else if (links > 2u) {
        links--;
    }
    st16(dir + 0x1Au, links);
    touch_c(dir);
    return inode_put(fs, dir_ino, dir);
}

/* ---- new inodes ----------------------------------------------------------- */

static void inode_init(vibeos_ext2_t *fs, uint32_t ino, uint8_t *inode, uint32_t mode, int extents) {
    uint64_t now = vibeos_fs_now_ns();

    memset(inode, 0, fs->inode_size);
    st16(inode, mode);
    st16(inode + 0x1Au, 1u);
    st16(inode + 0x80u, 32u);   /* i_extra_isize: through i_projid */
    time_set(inode, T_ATIME, now);
    time_set(inode, T_CTIME, now);
    time_set(inode, T_MTIME, now);
    time_set(inode, 0x90u, 0x94u, now);   /* i_crtime */
    st32(inode + 0x64u, ino * 2654435761u + (uint32_t)(now / 1000u));
    if (extents) {
        st32(inode + 0x20u, INODE_FLAG_EXTENTS);
        st16(inode + 40u, EXTENT_MAGIC);
        st16(inode + 44u, 4u);
    }
}

/* The file type a directory record carries. */
static uint32_t ftype_of(uint32_t mode) {
    switch (mode & VIBEOS_S_IFMT) {
    case VIBEOS_S_IFREG: return 1u;
    case VIBEOS_S_IFDIR: return 2u;
    case VIBEOS_S_IFCHR: return 3u;
    case VIBEOS_S_IFBLK: return 4u;
    case VIBEOS_S_IFIFO: return 5u;
    case VIBEOS_S_IFSOCK: return 6u;
    case VIBEOS_S_IFLNK: return 7u;
    default: return 0u;
    }
}

/* Take away what an inode owns and give the inode back: its blocks (and the
 * tree's), then the inode itself, with the deletion time e2fsck wants to see on
 * an inode that is free but not zero. */
static int inode_release(vibeos_ext2_t *fs, uint32_t ino, uint8_t *inode) {
    int is_dir = ext2_inode_is_dir(inode), rc;

    if ((le32(inode + 0x20u) & INODE_FLAG_EXTENTS) != 0u) {
        rc = ext_load(fs, ino, inode);
        if (rc == 0) {
            rc = ext_truncate(fs, 0);
        }
        if (rc == 0) {
            rc = ext_store(fs, ino, inode);
        }
        if (rc != 0) {
            return rc;
        }
    }
    st16(inode + 0x1Au, 0);
    set_size(inode, 0);
    set_blocks(inode, 0);
    st32(inode + 0x14u, (uint32_t)(vibeos_fs_now_ns() / NS));
    rc = inode_put(fs, ino, inode);
    if (rc != 0) {
        return rc;
    }
    return ifree(fs, ino, is_dir);
}

/* mkdir, create and symlink: a name in a directory for a new inode of `mode`. */
static int node_create(vibeos_ext2_t *fs, const char *path, uint32_t mode, const char *target, uint32_t tlen,
                       uint32_t *out_ino) {
    uint8_t parent[EXT2_MAX_INODE_SIZE], inode[EXT2_MAX_INODE_SIZE];
    uint32_t plen, nlen, pino, ino, bs = fs->block_size, got;
    const char *name;
    int rc, is_dir = S_ISDIR_(mode), fast = 0;
    uint64_t blk = 0;

    rc = path_split(path, &plen, &name, &nlen);
    if (rc != 0) {
        return rc;
    }
    if (name_is_dot(name, nlen)) {
        return -VIBEOS_EEXIST;
    }
    pino = resolve_n(fs, path, plen, parent);
    if (pino == 0u) {
        return -VIBEOS_ENOENT;
    }
    if (!ext2_inode_is_dir(parent)) {
        return -VIBEOS_ENOTDIR;
    }
    if (ext2_dir_find(fs, pino, parent, name, nlen) != 0u) {
        return -VIBEOS_EEXIST;
    }
    if (is_dir && le16(parent + 0x1Au) >= NLINK_MAX && (fs->ro_compat & RO_COMPAT_DIR_NLINK) == 0u) {
        return -VIBEOS_EMLINK;
    }
    if ((mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK) {
        if (tlen == 0u) {
            return -VIBEOS_ENOENT;
        }
        if (tlen >= bs) {
            return -VIBEOS_ENAMETOOLONG;
        }
        fast = tlen < 60u;
    }
    rc = ialloc(fs, pino, is_dir, &ino);
    if (rc != 0) {
        return rc;
    }
    inode_init(fs, ino, inode, mode, !fast);
    if (is_dir || (!fast && (mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK)) {
        /* The one block it has: ". " and ".." and the checksum entry, or the
         * target's bytes. */
        rc = balloc(fs, g_start(fs, (ino - 1u) / fs->inodes_per_group), 1u, &blk, &got);
        if (rc == 0) {
            uint8_t *b = fs->wbuf;

            memset(b, 0, bs);
            if (is_dir) {
                st32(b, ino);
                st16(b + 4u, 12u);
                b[6] = 1;
                b[7] = 2;
                b[8] = '.';
                st32(b + 12u, pino);
                st16(b + 16u, bs - 12u - DIR_TAIL);
                b[18] = 2;
                b[19] = 2;
                b[20] = '.';
                b[21] = '.';
                dirblk_csum_set(fs, ino, inode, b);
                st16(inode + 0x1Au, 2u);
                set_size(inode, bs);
            } else {
                memcpy(b, target, tlen);
                set_size(inode, tlen);
            }
            rc = blk_write(fs, blk, b);
            if (rc == 0) {
                fs->ext_n = 0;
                rc = ext_add(fs, 0, 1u, blk);
            }
            if (rc == 0) {
                rc = ext_store(fs, ino, inode);
            }
        }
    } else if (fast) {
        memcpy(inode + 40u, target, tlen);
        set_size(inode, tlen);
    }
    if (rc == 0) {
        rc = inode_put(fs, ino, inode);
    }
    if (rc == 0) {
        rc = dir_add_entry(fs, pino, name, nlen, ino, ftype_of(mode));
    }
    if (rc == 0 && is_dir) {
        rc = dir_links_adjust(fs, pino, 1);
    }
    if (rc != 0) {
        /* Best effort: give back what was taken. A failure on this path is a
         * volume that cannot be written, and the checker will say what is left. */
        if (blk != 0u) {
            (void)bfree(fs, blk, 1u);
        }
        (void)ifree(fs, ino, is_dir);
        return rc;
    }
    *out_ino = ino;
    return 0;
}

/* ---- the filesystem interface --------------------------------------------- */

static int w_lookup(void *fsv, const char *path, vibeos_fs_node_t *out) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];
    uint32_t ino;

    if (!fs || !fs->mounted) {
        return -1;
    }
    ino = ext2_resolve(fs, path, inode);
    if (ino == 0u) {
        return -1;
    }
    node_fill(ino, inode, out);
    return 0;
}

static int w_getattr(void *fsv, const vibeos_fs_node_t *node, vibeos_fs_node_t *out) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];

    if (!fs || !fs->mounted || node->id == 0u || node->id > 0xFFFFFFFFull) {
        return -VIBEOS_ENOENT;
    }
    if (ext2_read_inode(fs, (uint32_t)node->id, inode) != 0) {
        return -VIBEOS_EIO;
    }
    /* A node whose last name has gone: the inode is free, and the number may
     * name something else by the time anyone asks (no inode generation in the
     * node; the same gap as FAT's). */
    if (le16(inode + 0x1Au) == 0u || le16(inode) == 0u) {
        return -VIBEOS_ENOENT;
    }
    node_fill((uint32_t)node->id, inode, out);
    return 0;
}

static int w_statfs(void *fsv, vibeos_fs_statfs_t *out) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;

    out->magic = VIBEOS_EXT2_MAGIC;
    out->block_size = fs->block_size;
    out->blocks = fs->blocks_count;
    out->blocks_free = sb_free_blocks(fs);
    out->files = le32(fs->sb);
    out->files_free = le32(fs->sb + 0x10u);
    out->name_max = 255u;
    out->read_only = !fs->writable;
    return 0;
}

static int w_sync(void *fsv) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;

    return vibeos_blockcache_flush(fs->cache) == 0 ? 0 : -VIBEOS_EIO;
}

static long w_readlink(void *fsv, const char *path, char *buf, uint32_t cap) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];
    vibeos_fs_node_t node;
    uint32_t ino;
    long got;

    ino = ext2_resolve(fs, path, inode);
    if (ino == 0u) {
        return -VIBEOS_ENOENT;
    }
    if ((le16(inode) & VIBEOS_S_IFMT) != VIBEOS_S_IFLNK) {
        return -VIBEOS_EINVAL;
    }
    node_fill(ino, inode, &node);
    if (node.size < cap) {
        cap = (uint32_t)node.size;
    }
    got = ext2_op_read_at(fs, &node, 0, buf, cap);
    return got < 0 ? -VIBEOS_EIO : got;
}

/* A regular file the writer can change: an extent inode, in units of blocks. */
static int file_check(const uint8_t *inode) {
    uint32_t mode = le16(inode) & VIBEOS_S_IFMT;

    if (mode == VIBEOS_S_IFDIR) {
        return -VIBEOS_EISDIR;
    }
    if (mode != VIBEOS_S_IFREG) {
        return -VIBEOS_EINVAL;
    }
    if ((le32(inode + 0x20u) & INODE_FLAG_EXTENTS) == 0u || (le32(inode + 0x20u) & INODE_FLAG_HUGE) != 0u) {
        return -VIBEOS_EOPNOTSUPP;
    }
    return 0;
}

/* write_at: `len` bytes at `off`, the file growing as needed.
 *
 * The file's extents are loaded into the volume's list once, and each block of
 * the write is mapped through it:
 *  - a hole takes, in one allocation, as many blocks as the rest of the write
 *    reaches before the next extent, asked for right after the extent before -
 *    so a large or appending write stays one extent instead of one per block;
 *  - a block inside an unwritten extent is split out of it and made written,
 *    and only that block - the rest still reads as zeros;
 *  - a block the write covers whole is written straight from the caller; a
 *    part of one is read, patched and written back, or zero-filled first when
 *    the block is new, since a fresh block holds whatever its last owner left.
 * The tree is written back only if the list changed, and the inode last, so it
 * never names a block that is not there yet. A failure part-way returns the
 * bytes done, as write(2) does; the blocks taken for the part not written stay
 * the file's (E4-009). */
static long w_write_at(void *fsv, const vibeos_fs_node_t *node, uint64_t off, const void *buf, uint32_t len) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t bs = fs->block_size, ino = (uint32_t)node->id, done = 0;
    uint64_t size, end, fresh_from = 0, fresh_to = 0;
    int rc = 0, dirty = 0;

    if (node->id == 0u || node->id > 0xFFFFFFFFull || inode_get(fs, ino, inode) != 0) {
        return -VIBEOS_ENOENT;
    }
    rc = file_check(inode);
    if (rc != 0) {
        return rc;
    }
    if (len == 0u) {
        return 0;
    }
    if (off > 0xFFFFFFFFull * bs || len > 0xFFFFFFFFull * bs - off) {
        return -VIBEOS_EFBIG;
    }
    size = ext2_inode_size(inode);
    end = off + len;
    if (end > 0x7FFFFFFFull && (fs->ro_compat & RO_COMPAT_LARGE_FILE) == 0u) {
        return -VIBEOS_EFBIG;
    }
    rc = ext_load(fs, ino, inode);
    if (rc != 0) {
        return rc;
    }
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t bi = (uint32_t)(pos / bs), within = (uint32_t)(pos % bs), chunk = bs - within;
        uint64_t phys;
        int uninit, fresh;

        if (chunk > len - done) {
            chunk = len - done;
        }
        phys = ext_map(fs, bi, &uninit);
        if (phys == 0u) {
            /* A hole: take as many blocks as the rest of this write reaches
             * before the next extent, so a big write is a few extents. */
            uint32_t last = (uint32_t)((end - 1u) / bs), want, got, nexti = ext_search(fs, bi);
            uint64_t goal, start;

            want = last - bi + 1u;
            if (nexti < fs->ext_n && fs->ext[nexti].lblk - bi < want) {
                want = fs->ext[nexti].lblk - bi;
            }
            if (want > 32768u) {
                want = 32768u;
            }
            goal = nexti > 0u ? e_start(fs, nexti - 1u) + e_len(fs, nexti - 1u) : g_start(fs, (ino - 1u) / fs->inodes_per_group);
            rc = balloc(fs, goal, want, &start, &got);
            if (rc == 0) {
                rc = ext_add(fs, bi, got, start);
                if (rc != 0) {
                    (void)bfree(fs, start, got);
                }
            }
            if (rc != 0) {
                break;
            }
            dirty = 1;
            fresh_from = bi;
            fresh_to = (uint64_t)bi + got;
            phys = start;
        } else if (uninit) {
            rc = ext_make_init(fs, bi);
            if (rc != 0) {
                break;
            }
            dirty = 1;
            fresh_from = bi;
            fresh_to = (uint64_t)bi + 1u;
        }
        fresh = bi >= fresh_from && bi < fresh_to;
        if (chunk == bs) {
            rc = blk_write(fs, phys, src + done);
        } else {
            if (fresh) {
                memset(fs->wbuf, 0, bs);
            } else if (ext2_read_block(fs, phys, fs->wbuf) != 0) {
                rc = -VIBEOS_EIO;
                break;
            }
            memcpy(fs->wbuf + within, src + done, chunk);
            rc = blk_write(fs, phys, fs->wbuf);
        }
        if (rc != 0) {
            break;
        }
        done += chunk;
    }
    if (dirty) {
        int rs = ext_store(fs, ino, inode);

        if (rs != 0) {
            return rs;
        }
    }
    if (done > 0u) {
        if (off + done > size) {
            set_size(inode, off + done);
        }
        touch_mc(inode);
        if (inode_put(fs, ino, inode) != 0) {
            return -VIBEOS_EIO;
        }
    }
    return done > 0u ? (long)done : rc;
}

static int w_truncate(void *fsv, const vibeos_fs_node_t *node, uint64_t new_size) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];
    uint32_t bs = fs->block_size, ino = (uint32_t)node->id;
    uint64_t size;
    int rc;

    if (node->id == 0u || node->id > 0xFFFFFFFFull || inode_get(fs, ino, inode) != 0) {
        return -VIBEOS_ENOENT;
    }
    rc = file_check(inode);
    if (rc != 0) {
        return rc;
    }
    if (new_size > 0xFFFFFFFFull * bs || (new_size > 0x7FFFFFFFull && (fs->ro_compat & RO_COMPAT_LARGE_FILE) == 0u)) {
        return -VIBEOS_EFBIG;
    }
    size = ext2_inode_size(inode);
    if (new_size < size) {
        uint32_t keep = (uint32_t)((new_size + bs - 1u) / bs);

        rc = ext_load(fs, ino, inode);
        if (rc != 0) {
            return rc;
        }
        /* The bytes of the last block past the new end must read as zeros when
         * the file grows again: a block can be partly kept. */
        if (new_size % bs != 0u) {
            int uninit;
            uint64_t phys = ext_map(fs, (uint32_t)(new_size / bs), &uninit);

            if (phys != 0u && !uninit) {
                if (ext2_read_block(fs, phys, fs->wbuf) != 0) {
                    return -VIBEOS_EIO;
                }
                memset(fs->wbuf + new_size % bs, 0, bs - (uint32_t)(new_size % bs));
                if (blk_write(fs, phys, fs->wbuf) != 0) {
                    return -VIBEOS_EIO;
                }
            }
        }
        rc = ext_truncate(fs, keep);
        if (rc == 0) {
            rc = ext_store(fs, ino, inode);
        }
        if (rc != 0) {
            return rc;
        }
    }
    set_size(inode, new_size);
    touch_mc(inode);
    return inode_put(fs, ino, inode);
}

static int w_create(void *fsv, const char *path, uint32_t mode, vibeos_fs_node_t *out) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];
    uint32_t ino = 0;
    int rc = node_create(fs, path, VIBEOS_S_IFREG | (mode & 07777u), 0, 0, &ino);

    if (rc != 0) {
        return rc;
    }
    if (inode_get(fs, ino, inode) != 0) {
        return -VIBEOS_EIO;
    }
    node_fill(ino, inode, out);
    return 0;
}

static int w_mkdir(void *fsv, const char *path) {
    uint32_t ino;

    return node_create((vibeos_ext2_t *)fsv, path, VIBEOS_S_IFDIR | 0755u, 0, 0, &ino) == 0 ? 0 : -1;
}

static int w_symlink(void *fsv, const char *target, const char *path) {
    uint32_t ino;

    return node_create((vibeos_ext2_t *)fsv, path, VIBEOS_S_IFLNK | 0777u, target, cstr_len(target), &ino);
}

/* unlink and rmdir: the name goes, the link count drops, and a last name takes
 * the inode and its blocks with it. */
static int remove_name(vibeos_ext2_t *fs, const char *path, int want_dir) {
    uint8_t parent[EXT2_MAX_INODE_SIZE], inode[EXT2_MAX_INODE_SIZE];
    uint32_t plen, nlen, pino, ino = 0, links;
    const char *name;
    int rc;

    rc = path_split(path, &plen, &name, &nlen);
    if (rc != 0) {
        return want_dir ? -VIBEOS_EBUSY : rc;
    }
    if (name_is_dot(name, nlen)) {
        return nlen == 1u ? -VIBEOS_EINVAL : -VIBEOS_ENOTEMPTY;
    }
    pino = resolve_n(fs, path, plen, parent);
    if (pino == 0u) {
        return -VIBEOS_ENOENT;
    }
    ino = ext2_dir_find(fs, pino, parent, name, nlen);
    if (ino == 0u || ext2_read_inode(fs, ino, inode) != 0) {
        return -VIBEOS_ENOENT;
    }
    if (want_dir && !ext2_inode_is_dir(inode)) {
        return -VIBEOS_ENOTDIR;
    }
    if (!want_dir && ext2_inode_is_dir(inode)) {
        return -VIBEOS_EISDIR;
    }
    if (want_dir && !dir_empty(fs, ino)) {
        return -VIBEOS_ENOTEMPTY;
    }
    if ((le32(inode + 0x20u) & INODE_FLAG_EXTENTS) == 0u && !ext2_inode_is_fast_symlink(fs, inode) &&
        le32(inode + 0x1Cu) != 0u) {
        return -VIBEOS_EOPNOTSUPP;   /* block-mapped: this writer frees extents */
    }
    rc = dir_edit(fs, pino, name, nlen, 1, 0, 0, &links);
    if (rc != 0) {
        return rc;
    }
    if (want_dir) {
        rc = dir_links_adjust(fs, pino, -1);
        if (rc == 0) {
            rc = inode_release(fs, ino, inode);
        }
        return rc;
    }
    links = le16(inode + 0x1Au);
    if (links > 1u) {
        st16(inode + 0x1Au, links - 1u);
        touch_c(inode);
        return inode_put(fs, ino, inode);
    }
    return inode_release(fs, ino, inode);
}

static int w_unlink(void *fsv, const char *path) {
    return remove_name((vibeos_ext2_t *)fsv, path, 0) == 0 ? 0 : -1;
}

static int w_rmdir(void *fsv, const char *path) {
    return remove_name((vibeos_ext2_t *)fsv, path, 1);
}

static int w_link(void *fsv, const char *existing, const char *path) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t src[EXT2_MAX_INODE_SIZE], parent[EXT2_MAX_INODE_SIZE];
    uint32_t sino, plen, nlen, pino;
    const char *name;
    int rc;

    sino = ext2_resolve(fs, existing, src);
    if (sino == 0u) {
        return -VIBEOS_ENOENT;
    }
    if (ext2_inode_is_dir(src)) {
        return -VIBEOS_EPERM;
    }
    rc = path_split(path, &plen, &name, &nlen);
    if (rc != 0) {
        return rc;
    }
    if (name_is_dot(name, nlen)) {
        return -VIBEOS_EEXIST;
    }
    pino = resolve_n(fs, path, plen, parent);
    if (pino == 0u) {
        return -VIBEOS_ENOENT;
    }
    if (!ext2_inode_is_dir(parent)) {
        return -VIBEOS_ENOTDIR;
    }
    if (ext2_dir_find(fs, pino, parent, name, nlen) != 0u) {
        return -VIBEOS_EEXIST;
    }
    if (le16(src + 0x1Au) >= NLINK_MAX) {
        return -VIBEOS_EMLINK;
    }
    rc = dir_add_entry(fs, pino, name, nlen, sino, ftype_of(le16(src)));
    if (rc != 0) {
        return rc;
    }
    st16(src + 0x1Au, le16(src + 0x1Au) + 1u);
    touch_c(src);
    return inode_put(fs, sino, src);
}

/* rename: the target's name is pointed at the source (or a name made), the
 * source's name removed, and the counts and ".." that follow from a directory
 * having moved are put right. */
static int w_rename(void *fsv, const char *from, const char *to, uint32_t flags) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t p1[EXT2_MAX_INODE_SIZE], p2[EXT2_MAX_INODE_SIZE], src[EXT2_MAX_INODE_SIZE], dst[EXT2_MAX_INODE_SIZE];
    uint32_t plen1, plen2, nlen1, nlen2, pino1, pino2, sino, dino = 0, scratch;
    const char *name1, *name2;
    int rc, src_dir, have_dst;

    if (path_split(from, &plen1, &name1, &nlen1) != 0 || path_split(to, &plen2, &name2, &nlen2) != 0) {
        return -VIBEOS_EINVAL;
    }
    if (name_is_dot(name1, nlen1) || name_is_dot(name2, nlen2)) {
        return -VIBEOS_EINVAL;
    }
    pino1 = resolve_n(fs, from, plen1, p1);
    pino2 = resolve_n(fs, to, plen2, p2);
    if (pino1 == 0u || pino2 == 0u) {
        return -VIBEOS_ENOENT;
    }
    if (!ext2_inode_is_dir(p1) || !ext2_inode_is_dir(p2)) {
        return -VIBEOS_ENOTDIR;
    }
    sino = ext2_dir_find(fs, pino1, p1, name1, nlen1);
    if (sino == 0u || ext2_read_inode(fs, sino, src) != 0) {
        return -VIBEOS_ENOENT;
    }
    src_dir = ext2_inode_is_dir(src);
    dino = ext2_dir_find(fs, pino2, p2, name2, nlen2);
    have_dst = dino != 0u;
    if (have_dst && ext2_read_inode(fs, dino, dst) != 0) {
        return -VIBEOS_EIO;
    }
    if (have_dst && dino == sino) {
        return 0;   /* two names for one file: POSIX does nothing */
    }
    if (have_dst && (flags & VIBEOS_RENAME_NOREPLACE) != 0u) {
        return -VIBEOS_EEXIST;
    }
    if (src_dir) {
        /* A directory goes nowhere below itself: walk the target's parents up. */
        uint32_t x = pino2, steps = 0;

        while (x != VIBEOS_EXT2_ROOT_INO) {
            if (x == sino) {
                return -VIBEOS_EINVAL;
            }
            /* A volume whose ".." leads round in a circle never reaches the
             * root: no path is longer than the volume has directories. */
            if (++steps > le32(fs->sb) || dir_dotdot(fs, x, 0, &x) != 0) {
                return -VIBEOS_EIO;
            }
        }
        if (sino == VIBEOS_EXT2_ROOT_INO) {
            return -VIBEOS_EINVAL;
        }
    }
    if (have_dst) {
        if (src_dir && !ext2_inode_is_dir(dst)) {
            return -VIBEOS_ENOTDIR;
        }
        if (!src_dir && ext2_inode_is_dir(dst)) {
            return -VIBEOS_EISDIR;
        }
        if (src_dir && !dir_empty(fs, dino)) {
            return -VIBEOS_ENOTEMPTY;
        }
        if ((le32(dst + 0x20u) & INODE_FLAG_EXTENTS) == 0u && !ext2_inode_is_fast_symlink(fs, dst) &&
            le32(dst + 0x1Cu) != 0u) {
            return -VIBEOS_EOPNOTSUPP;
        }
        rc = dir_edit(fs, pino2, name2, nlen2, 2, sino, ftype_of(le16(src)), &scratch);
    } else {
        if (src_dir && pino1 != pino2 && le16(p2 + 0x1Au) >= NLINK_MAX && (fs->ro_compat & RO_COMPAT_DIR_NLINK) == 0u) {
            return -VIBEOS_EMLINK;
        }
        rc = dir_add_entry(fs, pino2, name2, nlen2, sino, ftype_of(le16(src)));
    }
    if (rc != 0) {
        return rc;
    }
    /* The old name goes. In one directory the add above has already moved
     * things in its block, which is why this looks the name up afresh. */
    rc = dir_edit(fs, pino1, name1, nlen1, 1, 0, 0, &scratch);
    if (rc != 0) {
        return rc;
    }
    if (src_dir && pino1 != pino2) {
        rc = dir_dotdot(fs, sino, pino2, &scratch);
        if (rc == 0) {
            rc = dir_links_adjust(fs, pino1, -1);
        }
        if (rc == 0 && !have_dst) {
            rc = dir_links_adjust(fs, pino2, 1);
        }
        if (rc != 0) {
            return rc;
        }
    }
    if (have_dst) {
        if (src_dir) {
            /* The replaced directory's ".." counted against pino2, and so
             * did the moved one's: the target's goes, the source's stays
             * (pino1 == pino2) or came in just above. */
            if (pino1 == pino2) {
                rc = dir_links_adjust(fs, pino2, -1);
            }
            if (rc == 0) {
                rc = inode_release(fs, dino, dst);
            }
        } else if (le16(dst + 0x1Au) > 1u) {
            st16(dst + 0x1Au, le16(dst + 0x1Au) - 1u);
            touch_c(dst);
            rc = inode_put(fs, dino, dst);
        } else {
            rc = inode_release(fs, dino, dst);
        }
        if (rc != 0) {
            return rc;
        }
    }
    if (ext2_read_inode(fs, sino, src) != 0) {
        return -VIBEOS_EIO;
    }
    touch_c(src);
    return inode_put(fs, sino, src);
}

static int attr_apply(vibeos_ext2_t *fs, uint32_t ino, uint8_t *inode, const vibeos_fs_attr_t *a) {
    if ((a->valid & VIBEOS_ATTR_MODE) != 0u) {
        st16(inode, (le16(inode) & VIBEOS_S_IFMT) | (a->mode & 07777u));
    }
    if ((a->valid & VIBEOS_ATTR_UID) != 0u) {
        st16(inode + 0x02u, a->uid);
        st16(inode + 0x78u, a->uid >> 16);
    }
    if ((a->valid & VIBEOS_ATTR_GID) != 0u) {
        st16(inode + 0x18u, a->gid);
        st16(inode + 0x7Au, a->gid >> 16);
    }
    if ((a->valid & VIBEOS_ATTR_ATIME) != 0u) {
        time_set(inode, T_ATIME, a->atime_ns);
    }
    if ((a->valid & VIBEOS_ATTR_MTIME) != 0u) {
        time_set(inode, T_MTIME, a->mtime_ns);
    }
    touch_c(inode);
    return inode_put(fs, ino, inode);
}

static int w_setattr(void *fsv, const char *path, const vibeos_fs_attr_t *attr) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];
    uint32_t ino = ext2_resolve(fs, path, inode);

    if (ino == 0u) {
        return -VIBEOS_ENOENT;
    }
    return attr_apply(fs, ino, inode, attr);
}

static int w_setattr_node(void *fsv, const vibeos_fs_node_t *node, const vibeos_fs_attr_t *attr) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];

    if (node->id == 0u || node->id > 0xFFFFFFFFull || inode_get(fs, (uint32_t)node->id, inode) != 0 ||
        le16(inode + 0x1Au) == 0u) {
        return -VIBEOS_ENOENT;
    }
    return attr_apply(fs, (uint32_t)node->id, inode, attr);
}

/* Replace a file's contents, creating it: the original shape, for callers that
 * have a whole file. */
static long w_write_file(void *fsv, const char *path, const void *buf, uint32_t len) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    vibeos_fs_node_t node;
    long r;

    if (w_lookup(fs, path, &node) != 0) {
        if (w_create(fs, path, 0644u, &node) != 0) {
            return -1;
        }
    } else if (node.is_dir || w_truncate(fs, &node, 0) != 0) {
        return -1;
    }
    r = w_write_at(fs, &node, 0, buf, len);
    return r < 0 ? -1 : r;
}

static const vibeos_fs_ops_t g_ext4_rw = {
    .lookup = w_lookup,
    .read_at = ext2_op_read_at,
    .write_file = w_write_file,
    .list = ext2_op_list,
    .unlink = w_unlink,
    .mkdir = w_mkdir,
    .write_at = w_write_at,
    .truncate = w_truncate,
    .create = w_create,
    .rmdir = w_rmdir,
    .rename = w_rename,
    .link = w_link,
    .symlink = w_symlink,
    .readlink = w_readlink,
    .setattr = w_setattr,
    .getattr = w_getattr,
    .setattr_node = w_setattr_node,
    .statfs = w_statfs,
    .sync = w_sync,
    .serialize = 1,
};

/* A volume the writer does not keep (a device that takes no writes, a feature
 * it would leave stale) is read through the same node description. */
static const vibeos_fs_ops_t g_ext4_ro = {
    .lookup = w_lookup,
    .read_at = ext2_op_read_at,
    .list = ext2_op_list,
    .readlink = w_readlink,
    .getattr = w_getattr,
    .statfs = w_statfs,
};

const vibeos_fs_ops_t *ext4_ops(const vibeos_ext2_t *fs) {
    return fs->writable ? &g_ext4_rw : &g_ext4_ro;
}
