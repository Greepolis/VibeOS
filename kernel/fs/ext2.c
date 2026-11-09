/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ext2, read-only; and the reader ext4_write.c's ext4 writer is built on.
 *
 * Portable: every byte comes through the block cache, which comes through a
 * function pair, so a fabricated image in an array exercises the same code the
 * kernel runs. Indirect block arithmetic is the part that is easy to get
 * subtly wrong and impossible to see wrong from a boot - a file just comes
 * back with the wrong contents somewhere past the twelfth block.
 */

#include "vibeos/storage.h"
#include "vibeos/ext2.h"
#include "vibeos/mbz.h"
#include "ext2_internal.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One filesystem block, assembled from the 512-byte sectors under it. The
 * caller supplies the buffer; this file has no allocator. */
int ext2_read_block(vibeos_ext2_t *fs, uint64_t block, uint8_t *out) {
    uint32_t sectors = fs->block_size / VIBEOS_BLOCK_SIZE;
    uint32_t i;

    /* A block this volume has, or nothing (H-013). Every read goes through here -
     * direct and indirect pointers from an inode, the inode table from a group
     * descriptor - and every one of those numbers comes from the volume. The
     * block layer bounds a read by the device, not the partition, so a pointer
     * past blocks_count but inside the device used to read another partition's
     * sectors. The mount keeps blocks_count for exactly this check. */
    if (fs->block_size == 0u || sectors == 0u || block >= fs->blocks_count) {
        vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, block);   /* a pointer off the volume */
        return -1;
    }
    for (i = 0; i < sectors; i++) {
        uint64_t lba = fs->part_lba + block * sectors + i;
        if (ext2_sec_read(fs, lba, out + i * VIBEOS_BLOCK_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

/* What this driver reads. Everything else in the incompat word changes where
 * or how something is stored - meta_bg moves the group descriptors, inline
 * data puts a file in its inode, casefold changes what a name matches - and
 * reading such a volume without knowing would return the wrong bytes, so it is
 * refused. RECOVER is not in the list: the journal holds metadata newer than
 * the volume, so the mount replays it first (ext4_j_recover, step 3) and
 * mounts what that leaves - or, on a device that takes no writes, refuses,
 * because reading past it reads the past. A ro_compat feature only matters
 * to a writer. */
#define INCOMPAT_KNOWN (INCOMPAT_FILETYPE | INCOMPAT_EXTENTS | INCOMPAT_64BIT | INCOMPAT_FLEX_BG | \
                        INCOMPAT_CSUM_SEED | INCOMPAT_LARGEDIR)

static uint64_t g_ext2_hits;
static uint64_t g_ext2_csum_bad;
static uint32_t g_ext2_refused;

uint64_t vibeos_ext2_csum_failures(void) {
    return __atomic_load_n(&g_ext2_csum_bad, __ATOMIC_RELAXED);
}

uint32_t vibeos_ext2_last_refused_features(void) {
    return __atomic_load_n(&g_ext2_refused, __ATOMIC_RELAXED);
}

uint32_t vibeos_ext2_crc32c(uint32_t crc, const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;

    while (len-- > 0u) {
        uint32_t k;

        crc ^= *p++;
        for (k = 0; k < 8u; k++) {
            crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
        }
    }
    return crc;
}

uint32_t ext2_le32_crc(uint32_t crc, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };

    return vibeos_ext2_crc32c(crc, b, 4u);
}

/* A checksum that did not match: counted, and the caller refuses what it
 * covers. Never used anyway - a metadata block with a wrong checksum is the
 * moment a corruption is cheapest to stop. */
int ext2_csum_bad(uint64_t what) {
    __atomic_add_fetch(&g_ext2_csum_bad, 1ull, __ATOMIC_RELAXED);
    vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, what);
    return -1;
}

uint64_t vibeos_ext2_cache_hits(void) {
    return __atomic_load_n(&g_ext2_hits, __ATOMIC_RELAXED);
}

/* Copy `n` bytes out of a cache slot if it holds `key`; 1 if it did. A slot
 * being written - its sequence odd, or moved while this copied - is a miss:
 * the caller reads the disk, which is never wrong. No lock, so nothing the
 * architecture has to register for a cache this small. */
static int ext2_cache_get(uint32_t *seq, const uint32_t *slot_key, uint32_t key,
                          const uint8_t *from, uint8_t *out, uint32_t n) {
    uint32_t s1, s2, i;

    if (key == 0u) {
        return 0;
    }
    s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if ((s1 & 1u) != 0u || __atomic_load_n(slot_key, __ATOMIC_RELAXED) != key) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        out[i] = __atomic_load_n(&from[i], __ATOMIC_RELAXED);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    s2 = __atomic_load_n(seq, __ATOMIC_RELAXED);
    if (s1 != s2) {
        return 0;
    }
    __atomic_add_fetch(&g_ext2_hits, 1ull, __ATOMIC_RELAXED);
    return 1;
}

/* Fill a slot, unless another core is filling it: then this one is not
 * cached, which costs a read later and nothing now. */
static void ext2_cache_put(uint32_t *seq, uint32_t *slot_key, uint32_t key, uint8_t *to,
                           const uint8_t *from, uint32_t n) {
    uint32_t s = __atomic_load_n(seq, __ATOMIC_RELAXED), i;

    if ((s & 1u) != 0u ||
        !__atomic_compare_exchange_n(seq, &s, s + 1u, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return;
    }
    __atomic_store_n(slot_key, key, __ATOMIC_RELAXED);
    for (i = 0; i < n; i++) {
        __atomic_store_n(&to[i], from[i], __ATOMIC_RELAXED);
    }
    __atomic_store_n(seq, s + 2u, __ATOMIC_RELEASE);
}

static int ext2_mount_once(vibeos_ext2_t *fs, vibeos_blockcache_t *cache, uint64_t part_lba, int *recover);

int vibeos_ext2_mount(vibeos_ext2_t *fs, vibeos_blockcache_t *cache,
                      uint64_t part_lba) {
    int recover = 0;

    if (ext2_mount_once(fs, cache, part_lba, &recover) != 0) {
        return -1;
    }
    if (!recover) {
        return 0;
    }
    /* Replayed, the volume is mounted again from its superblock as the
     * journal left it; one that still says RECOVER after that is refused. */
    if (ext4_j_recover(fs) != 0 || ext2_mount_once(fs, cache, part_lba, &recover) != 0 || recover) {
        fs->mounted = 0;
        return -1;
    }
    return 0;
}

static int ext2_mount_once(vibeos_ext2_t *fs, vibeos_blockcache_t *cache, uint64_t part_lba, int *recover) {
    uint8_t sb[VIBEOS_BLOCK_SIZE * 2u];
    uint32_t log_bs;
    uint64_t blocks_count, groups;

    if (!fs || !cache) {
        return -1;
    }
    fs->cache = cache;
    fs->part_lba = part_lba;
    fs->mounted = 0;
    fs->cached_ino = 0;
    fs->cached_ptr_block = 0;
    fs->ino_seq = 0;
    fs->ptr_seq = 0;
    fs->csum = 0;
    fs->writable = 0;
    __builtin_memset(&fs->j, 0, sizeof(fs->j));   /* no transaction: every read is of the volume */
    __atomic_store_n(&g_ext2_refused, 0u, __ATOMIC_RELAXED);

    /* The superblock is at byte 1024, whatever the block size, which is why it
     * is read as sectors rather than as a block: the block size is what we are
     * about to learn. */
    if (vibeos_blockcache_read(cache, part_lba + 2u, sb) != 0 ||
        vibeos_blockcache_read(cache, part_lba + 3u, sb + VIBEOS_BLOCK_SIZE) != 0) {
        return -1;
    }
    if (rd16(sb + 56) != VIBEOS_EXT2_MAGIC) {
        return -1;
    }
    log_bs = rd32(sb + 24);
    if (log_bs > 2u) {
        return -1;   /* larger than 4 KiB; see EXT2_MAX_BLOCK */
    }
    fs->block_size = 1024u << log_bs;
    fs->blocks_per_group = rd32(sb + 32);
    fs->inodes_per_group = rd32(sb + 40);
    fs->first_data_block = rd32(sb + 20);

    /* The feature words exist from revision 1; revision 0 has none. */
    fs->incompat = rd32(sb + 76) >= 1u ? rd32(sb + 0x60) : 0u;
    fs->ro_compat = rd32(sb + 76) >= 1u ? rd32(sb + 0x64) : 0u;
    *recover = (fs->incompat & INCOMPAT_RECOVER) != 0u;
    fs->incompat &= ~INCOMPAT_RECOVER;
    if ((fs->incompat & ~INCOMPAT_KNOWN) != 0u) {
        __atomic_store_n(&g_ext2_refused, fs->incompat & ~INCOMPAT_KNOWN, __ATOMIC_RELAXED);
        return -1;
    }
    /* metadata_csum: the superblock checks itself, and every other checksum
     * is seeded from the uuid - or from a seed stored beside it, so that the
     * uuid can change without rewriting every piece of metadata. */
    if ((fs->ro_compat & RO_COMPAT_METADATA_CSUM) != 0u) {
        if (vibeos_ext2_crc32c(~0u, sb, 0x3FCu) != rd32(sb + 0x3FC)) {
            return ext2_csum_bad(0);
        }
        fs->csum = 1;
        fs->csum_seed = (fs->incompat & INCOMPAT_CSUM_SEED) != 0u ? rd32(sb + 0x270)
                                                                 : vibeos_ext2_crc32c(~0u, sb + 0x68, 16u);
    }
    /* A 64-bit volume keeps the high half of its length beside the low one,
     * and its group descriptors grow to s_desc_size. */
    blocks_count = rd32(sb + 4);
    fs->desc_size = 32u;
    if ((fs->incompat & INCOMPAT_64BIT) != 0u) {
        blocks_count |= (uint64_t)rd32(sb + 0x150) << 32;
        fs->desc_size = rd16(sb + 0xFE);
        if (fs->desc_size < 64u || fs->desc_size > 1024u || (fs->desc_size & (fs->desc_size - 1u)) != 0u) {
            return -1;
        }
    }
    fs->blocks_count = blocks_count;

    /* s_inode_size only exists from revision 1; revision 0 always used 128. */
    fs->inode_size = (rd32(sb + 76) >= 1u) ? rd16(sb + 88) : 128u;
    if (fs->inode_size < 128u || fs->inode_size > EXT2_MAX_INODE_SIZE ||
        (fs->inode_size & (fs->inode_size - 1u)) != 0u) {
        return -1;
    }
    if (fs->blocks_per_group == 0u || fs->inodes_per_group == 0u) {
        return -1;
    }
    /* A group's bitmaps are one block each, so a group cannot have more
     * blocks or inodes than a block has bits - Linux's "blocks per group too
     * big". The writer sizes its bitmap scratch by the block and the
     * checksums and the padding of a fresh bitmap by these counts, so a
     * superblock claiming more had it read and write past that block
     * (issue #202). block_size is at most 4 KiB by the check on its log
     * above, so the product has room in 32 bits. */
    if (fs->blocks_per_group > fs->block_size * 8u || fs->inodes_per_group > fs->block_size * 8u) {
        return -1;
    }
    /* Checked before the subtraction, not after it.
     *
     * These are unsigned, so a superblock claiming a first data block past the
     * end of the volume does not produce a negative group count - it produces
     * an enormous one, and every later bound derived from it is then a bound
     * against a number that came out of a wrap. A corrupt or hostile image is
     * the only way to reach this, which is exactly the case a mount has to
     * survive: the block cache refuses the reads further down, so nothing has
     * gone wrong yet, but a refusal at the far end of a wrapped calculation is
     * a refusal that says the wrong thing about why.
     *
     * s_first_data_block is 1 on a 1 KiB filesystem and 0 on every larger one,
     * so it is always inside the volume on anything well-formed. */
    if (blocks_count == 0u || fs->first_data_block >= blocks_count) {
        return -1;
    }
    groups = (blocks_count - fs->first_data_block + fs->blocks_per_group - 1u) / fs->blocks_per_group;
    if (groups == 0u || groups > 0xFFFFFFFFull) {
        return -1;
    }
    fs->group_count = (uint32_t)groups;
    /* The group descriptors live in the block after the superblock, which is
     * block 1 for a 1 KiB filesystem and block 1 as well for larger ones -
     * because the superblock still starts at byte 1024, inside block 0. */
    fs->group_desc_block = (fs->block_size == 1024u) ? 2u : 1u;
    fs->mounted = 1;
    return 0;
}

/* `n` bytes at byte `off` inside a block, read through the sectors that hold
 * them and no more. A descriptor or an inode is a few dozen bytes, and the
 * kernel stack is four pages: the whole block on the stack for each of them
 * was a quarter of it, under every lookup the writer makes. */
int ext2_read_part(vibeos_ext2_t *fs, uint64_t block, uint32_t off, uint8_t *out, uint32_t n) {
    uint8_t sector[VIBEOS_BLOCK_SIZE];
    uint32_t sectors = fs->block_size / VIBEOS_BLOCK_SIZE, done = 0;

    if (fs->block_size == 0u || sectors == 0u || block >= fs->blocks_count || off >= fs->block_size ||
        n > fs->block_size - off) {
        vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, block);
        return -1;
    }
    while (done < n) {
        uint32_t at = off + done, in = at % VIBEOS_BLOCK_SIZE, take = VIBEOS_BLOCK_SIZE - in, i;

        if (ext2_sec_read(fs, fs->part_lba + block * sectors + at / VIBEOS_BLOCK_SIZE, sector) != 0) {
            return -1;
        }
        if (take > n - done) {
            take = n - done;
        }
        for (i = 0; i < take; i++) {
            out[done + i] = sector[in + i];
        }
        done += take;
    }
    return 0;
}

/* Where group `group`'s inode table starts. A descriptor is 32 bytes, or
 * s_desc_size with 64bit, which also keeps the high half of each block number
 * at +0x20 past the low one; with metadata_csum it checks itself. */
int ext2_inode_table(vibeos_ext2_t *fs, uint32_t group, uint64_t *out) {
    uint8_t d[1024];
    uint64_t at = (uint64_t)group * fs->desc_size;

    /* The mount makes it 32 or a power of two from 64; said here as well, so
     * that no path reads a descriptor of which nothing was copied. */
    if (fs->desc_size < 32u || fs->desc_size > sizeof(d) ||
        ext2_read_part(fs, fs->group_desc_block + at / fs->block_size, (uint32_t)(at % fs->block_size), d,
                       fs->desc_size) != 0) {
        return -1;
    }
    if (fs->csum) {
        static const uint8_t zero[2] = { 0, 0 };
        uint32_t c = ext2_le32_crc(fs->csum_seed, group);

        c = vibeos_ext2_crc32c(c, d, 0x1Eu);
        c = vibeos_ext2_crc32c(c, zero, 2u);
        c = vibeos_ext2_crc32c(c, d + 0x20u, fs->desc_size - 0x20u);
        if ((c & 0xFFFFu) != rd16(d + 0x1Eu)) {
            return ext2_csum_bad(group);
        }
    }
    *out = rd32(d + 8u);
    if (fs->desc_size >= 64u) {
        *out |= (uint64_t)rd32(d + 0x28u) << 32;
    }
    return 0;
}

/* metadata_csum's per-inode seed: the volume's seed, then the inode's number
 * and generation, so that a block moved to another file no longer checks. */
uint32_t ext2_inode_seed(const vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode) {
    return ext2_le32_crc(ext2_le32_crc(fs->csum_seed, ino), rd32(inode + 100u));
}

/* An inode's checksum: crc32c over the whole inode with its two checksum
 * halves read as zero - the low half at 0x7C, the high one at 0x82 when the
 * inode's extra size reaches it. */
int ext2_inode_csum_ok(const vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode) {
    static const uint8_t zero[2] = { 0, 0 };
    uint32_t c = ext2_inode_seed(fs, ino, inode), want = rd16(inode + 0x7Cu);
    int hi = 0;

    c = vibeos_ext2_crc32c(c, inode, 0x7Cu);
    c = vibeos_ext2_crc32c(c, zero, 2u);
    c = vibeos_ext2_crc32c(c, inode + 0x7Eu, 2u);
    if (fs->inode_size > 128u) {
        hi = rd16(inode + 0x80u) >= 4u;   /* i_extra_isize covers i_checksum_hi */
        c = vibeos_ext2_crc32c(c, inode + 0x80u, 2u);
        if (hi) {
            c = vibeos_ext2_crc32c(c, zero, 2u);
            want |= (uint32_t)rd16(inode + 0x82u) << 16;
        }
        c = vibeos_ext2_crc32c(c, inode + (hi ? 0x84u : 0x82u), fs->inode_size - (hi ? 0x84u : 0x82u));
    }
    return hi ? c == want : (c & 0xFFFFu) == want;
}

/* Fill `out` with one whole inode, inode_size bytes. */
int ext2_read_inode(vibeos_ext2_t *fs, uint32_t ino, uint8_t *out) {
    uint8_t raw[EXT2_MAX_INODE_SIZE];
    uint32_t group, index, offset, per_block, i;
    uint64_t table_block;

    if (ino == 0u) {
        return -1;   /* inode zero does not exist; it means "no entry" */
    }
    /* The whole buffer, a constant: the bytes past a 128-byte inode are zeros
     * the read below writes, so a hit returns exactly what a miss would. */
    if (ext2_cache_get(&fs->ino_seq, &fs->cached_ino, ino, fs->cached_inode, out, EXT2_MAX_INODE_SIZE)) {
        return 0;
    }
    group = (ino - 1u) / fs->inodes_per_group;
    index = (ino - 1u) % fs->inodes_per_group;
    if (group >= fs->group_count || ext2_inode_table(fs, group, &table_block) != 0) {
        return -1;
    }
    per_block = fs->block_size / fs->inode_size;
    if (per_block == 0u) {
        return -1;
    }
    offset = (index % per_block) * fs->inode_size;
    if (ext2_read_part(fs, table_block + index / per_block, offset, raw, fs->inode_size) != 0) {
        return -1;
    }
    for (i = 0; i < EXT2_MAX_INODE_SIZE; i++) {
        out[i] = i < fs->inode_size ? raw[i] : 0u;
    }
    if (fs->csum && !ext2_inode_csum_ok(fs, ino, out)) {
        return ext2_csum_bad(ino);
    }
    ext2_cache_put(&fs->ino_seq, &fs->cached_ino, ino, fs->cached_inode, out, EXT2_MAX_INODE_SIZE);
    return 0;
}

/* The last level of pointers a block number is mapped through: the block of
 * pointers that consecutive data blocks share, kept between calls. */
static int ext2_read_ptrs(vibeos_ext2_t *fs, uint32_t block, uint8_t *out) {
    if (fs->block_size > sizeof(fs->cached_ptrs)) {
        return ext2_read_block(fs, block, out);
    }
    if (ext2_cache_get(&fs->ptr_seq, &fs->cached_ptr_block, block, fs->cached_ptrs, out, fs->block_size)) {
        return 0;
    }
    if (ext2_read_block(fs, block, out) != 0) {
        return -1;
    }
    ext2_cache_put(&fs->ptr_seq, &fs->cached_ptr_block, block, fs->cached_ptrs, out, fs->block_size);
    return 0;
}

/* Which filesystem block holds a file's block number `n`.
 *
 * Twelve direct entries, then one, two and three levels of indirection. The
 * boundaries are where this goes wrong, so they are computed rather than
 * unrolled: an off-by-one here returns a real block from the wrong offset, and
 * the file reads back plausible and wrong. */
static int ext2_map_indirect(vibeos_ext2_t *fs, const uint8_t *inode, uint32_t n, uint32_t *out);

/* An extent tree's node: a 12-byte header, then `entries` 12-byte entries -
 * index entries above the leaves, extents in them. `room` is the bytes the node
 * occupies, which bounds `max` before anything is read through it. */
static int extent_header_ok(const uint8_t *node, uint32_t room, uint32_t *entries, uint32_t *depth) {
    uint32_t max = rd16(node + 4u);

    *entries = rd16(node + 2u);
    *depth = rd16(node + 6u);
    if (rd16(node) != EXTENT_MAGIC || max == 0u || 12u + max * 12u > room || *entries > max ||
        *depth > EXTENT_MAX_DEPTH) {
        vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, rd16(node));
        return -1;
    }
    return 0;
}

/* Which block holds a file's block `n`, through its extent tree. The root is the
 * inode's sixty bytes of i_block; each level down is a block with its own
 * checksum in a tail after its entries, seeded by the inode that owns it. A
 * block nobody wrote - past every extent, or in an extent marked
 * uninitialised - reads as a hole: 0.
 *
 * A level below the root is read a piece at a time - its header, the entries
 * one by one, its checksum a sector at a time - and never whole onto the
 * stack. It used to be a 4 KiB buffer in this frame, under a directory scan's
 * own 4 KiB, under exec: once ext4 was the root, a Debug build's NT process
 * creation ran into the guard page below its kernel stack here (E4-014). */
static int extent_entry(vibeos_ext2_t *fs, const uint8_t *root, uint64_t blk, uint32_t i, uint8_t e[12]) {
    uint32_t k;

    if (blk == 0u) {
        for (k = 0; k < 12u; k++) {
            e[k] = root[12u + i * 12u + k];
        }
        return 0;
    }
    return ext2_read_part(fs, blk, 12u + i * 12u, e, 12u);
}

static int extent_block_csum_ok(vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode, uint64_t blk,
                                uint32_t at) {
    uint8_t piece[VIBEOS_BLOCK_SIZE], tail[4];
    uint32_t crc = ext2_inode_seed(fs, ino, inode), off = 0;

    while (off < at) {
        uint32_t take = at - off < VIBEOS_BLOCK_SIZE ? at - off : VIBEOS_BLOCK_SIZE;

        if (ext2_read_part(fs, blk, off, piece, take) != 0) {
            return 0;
        }
        crc = vibeos_ext2_crc32c(crc, piece, take);
        off += take;
    }
    return ext2_read_part(fs, blk, at, tail, 4u) == 0 && crc == rd32(tail);
}

static int ext2_map_extent(vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode, uint32_t n,
                           uint64_t *out) {
    uint8_t hdr[12], e[12];
    const uint8_t *root = inode + 40u;
    uint64_t blk = 0;   /* the node being walked: 0 is the root in the inode */
    uint32_t entries, depth, level, i;

    if (extent_header_ok(root, 60u, &entries, &depth) != 0) {
        return -1;
    }
    for (level = 0; depth > 0u; level++) {
        uint64_t child;
        uint32_t want_depth = depth - 1u;

        if (entries == 0u || level >= EXTENT_MAX_DEPTH) {
            *out = 0;
            return 0;
        }
        /* The last index whose first block is at or below n. */
        if (extent_entry(fs, root, blk, 0u, e) != 0) {
            return -1;
        }
        child = rd32(e + 4u) | ((uint64_t)rd16(e + 8u) << 32);
        for (i = 1; i < entries; i++) {
            if (extent_entry(fs, root, blk, i, e) != 0) {
                return -1;
            }
            if (rd32(e) > n) {
                break;
            }
            child = rd32(e + 4u) | ((uint64_t)rd16(e + 8u) << 32);
        }
        if (child == 0u || child >= fs->blocks_count || ext2_read_part(fs, child, 0u, hdr, 12u) != 0) {
            return -1;
        }
        blk = child;
        if (extent_header_ok(hdr, fs->block_size, &entries, &depth) != 0 || depth != want_depth) {
            return -1;   /* a child that is not one level below its parent: a loop, or garbage */
        }
        if (fs->csum) {
            uint32_t at = 12u + rd16(hdr + 4u) * 12u;

            if (at + 4u > fs->block_size || !extent_block_csum_ok(fs, ino, inode, child, at)) {
                return ext2_csum_bad(child);
            }
        }
    }
    for (i = 0; i < entries; i++) {
        uint32_t first, len;

        if (extent_entry(fs, root, blk, i, e) != 0) {
            return -1;
        }
        first = rd32(e);
        len = rd16(e + 4u);
        if (len > 32768u) {
            len -= 32768u;   /* uninitialised: allocated, and reads as zeros */
            if (n >= first && n - first < len) {
                *out = 0;
                return 0;
            }
            continue;
        }
        if (n >= first && n - first < len) {
            *out = (((uint64_t)rd16(e + 6u) << 32) | rd32(e + 8u)) + (n - first);
            return 0;
        }
    }
    *out = 0;
    return 0;
}

/* Which filesystem block holds a file's block `n`: through the extent tree on
 * an inode that has one, through ext2's pointers otherwise. */
int ext2_map_block(vibeos_ext2_t *fs, uint32_t ino, const uint8_t *inode, uint32_t n,
                          uint64_t *out) {
    uint32_t b = 0;

    if ((rd32(inode + 32u) & INODE_FLAG_EXTENTS) != 0u) {
        if ((fs->incompat & INCOMPAT_EXTENTS) == 0u) {
            vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, ino);
            return -1;   /* an extent inode on a volume that says it has none */
        }
        return ext2_map_extent(fs, ino, inode, n, out);
    }
    if (ext2_map_indirect(fs, inode, n, &b) != 0) {
        return -1;
    }
    *out = b;
    return 0;
}

static int ext2_map_indirect(vibeos_ext2_t *fs, const uint8_t *inode, uint32_t n,
                             uint32_t *out) {
    uint32_t per = fs->block_size / 4u;
    uint8_t block[EXT2_MAX_BLOCK];

    if (n < 12u) {
        *out = rd32(inode + 40u + n * 4u);
        return 0;
    }
    n -= 12u;
    if (n < per) {
        uint32_t ind = rd32(inode + 40u + 12u * 4u);
        if (ind == 0u || ext2_read_ptrs(fs, ind, block) != 0) {
            return -1;
        }
        *out = rd32(block + n * 4u);
        return 0;
    }
    n -= per;
    if (n < per * per) {
        uint32_t dind = rd32(inode + 40u + 13u * 4u);
        uint32_t mid;
        if (dind == 0u || ext2_read_block(fs, dind, block) != 0) {
            return -1;
        }
        mid = rd32(block + (n / per) * 4u);
        if (mid == 0u || ext2_read_ptrs(fs, mid, block) != 0) {
            return -1;
        }
        *out = rd32(block + (n % per) * 4u);
        return 0;
    }
    n -= per * per;
    /* The triple-indirect tree holds per^3 blocks and no more. Without this
     * an index past it read the pointer block's neighbours on the stack -
     * 256 KB past it with 1 KiB blocks, through the guard page - and used
     * what it found as a block number (review v9, point 1). */
    if ((uint64_t)n >= (uint64_t)per * per * per) {
        return -1;
    }
    {
        uint32_t tind = rd32(inode + 40u + 14u * 4u);
        uint32_t a, b;
        if (tind == 0u || ext2_read_block(fs, tind, block) != 0) {
            return -1;
        }
        a = rd32(block + (n / (per * per)) * 4u);
        if (a == 0u || ext2_read_block(fs, a, block) != 0) {
            return -1;
        }
        b = rd32(block + ((n / per) % per) * 4u);
        if (b == 0u || ext2_read_ptrs(fs, b, block) != 0) {
            return -1;
        }
        *out = rd32(block + (n % per) * 4u);
    }
    return 0;
}

uint64_t ext2_inode_size(const uint8_t *inode) {
    /* The high half of a regular file's size lives in what was a directory ACL
     * field. Ignoring it silently truncates every file over 4 GiB to its low
     * 32 bits, which reads as a shorter file rather than as an error. */
    uint64_t low = rd32(inode + 4);
    uint64_t high = rd32(inode + 108);
    return low | (high << 32);
}

int ext2_inode_is_dir(const uint8_t *inode) {
    return (rd16(inode) & 0xF000u) == 0x4000u;
}

/* A symbolic link short enough to live in the inode's sixty bytes of i_block,
 * which hold its target and not block numbers (E4-001): Linux's own test, no
 * blocks of its own - an attribute block excepted - rather than a guess from
 * the length, because a long link written by ext4 has the extents flag and an
 * ext2 one has pointers, and either may be short. Read through
 * ext2_map_block, the target's first four bytes were a block number. */
int ext2_inode_is_fast_symlink(const vibeos_ext2_t *fs, const uint8_t *inode) {
    uint64_t blocks = rd32(inode + 0x1Cu);
    uint32_t acl = rd32(inode + 0x68u);

    if ((rd16(inode) & 0xF000u) != 0xA000u || ext2_inode_size(inode) >= 60u) {
        return 0;
    }
    if (acl != 0u) {
        blocks = blocks >= fs->block_size / VIBEOS_BLOCK_SIZE ? blocks - fs->block_size / VIBEOS_BLOCK_SIZE : 1u;
    }
    return blocks == 0u && (rd32(inode + 0x20u) & INODE_FLAG_EXTENTS) == 0u;
}

/* Blocks a directory of `size` bytes spans, without the overflow. The classic
 * (size + bs - 1) / bs wraps to 0 for a size near UINT64_MAX and the whole
 * directory then scans as empty (M-022); and a directory cannot own more blocks
 * than the filesystem has, so a corrupt size also stops bounding a long scan. */
uint32_t ext2_dir_nblocks(const vibeos_ext2_t *fs, uint64_t size) {
    uint64_t n = size / fs->block_size + ((size % fs->block_size) != 0u ? 1u : 0u);
    if (n > fs->blocks_count) {
        n = fs->blocks_count;
    }
    /* blocks_count is 64-bit with ext4: a count of 2^32 cast to 32 bits was
     * 0, a directory that scanned as empty (M-022 again, review v9 point 3).
     * The largest the scan can say is the largest it can count. */
    if (n > 0xFFFFFFFFull) {
        n = 0xFFFFFFFFull;
    }
    return (uint32_t)n;
}

/* One block of directory `ino`, read and, with metadata_csum, checked. A leaf
 * block ends in a twelve-byte entry that names no inode (file type 0xDE) and
 * holds crc32c of everything before it; that entry is skipped by the scan like
 * any other with inode 0. An htree node keeps its checksum elsewhere and has no
 * such tail: it is read as the empty block it looks like to a linear scan,
 * which is what htree was designed to be, and not checked here. */
int ext2_dir_block(vibeos_ext2_t *fs, uint32_t ino, const uint8_t *dir, uint64_t phys,
                          uint8_t *block) {
    const uint8_t *t;

    if (ext2_read_block(fs, phys, block) != 0) {
        return -1;
    }
    if (!fs->csum) {
        return 0;
    }
    t = block + fs->block_size - 12u;
    if (rd32(t) == 0u && rd16(t + 4u) == 12u && t[6] == 0u && t[7] == 0xDEu &&
        vibeos_ext2_crc32c(ext2_inode_seed(fs, ino, dir), block, fs->block_size - 12u) != rd32(t + 8u)) {
        return ext2_csum_bad(phys);
    }
    return 0;
}

/* Find `name` in directory `dir_ino`. Returns the inode number, or 0. */
uint32_t ext2_dir_find(vibeos_ext2_t *fs, uint32_t dir_ino, const uint8_t *dir,
                              const char *name, uint32_t name_len) {
    uint8_t block[EXT2_MAX_BLOCK];
    uint64_t size = ext2_inode_size(dir);
    uint32_t nblocks = ext2_dir_nblocks(fs, size);
    uint32_t bi;

    for (bi = 0; bi < nblocks; bi++) {
        uint64_t phys = 0;
        uint32_t off = 0;
        if (ext2_map_block(fs, dir_ino, dir, bi, &phys) != 0 || phys == 0u) {
            continue;
        }
        if (ext2_dir_block(fs, dir_ino, dir, phys, block) != 0) {
            return 0;
        }
        while (off + 8u <= fs->block_size) {
            uint32_t ino = rd32(block + off);
            uint16_t reclen = rd16(block + off + 4u);
            uint8_t nlen = block[off + 6u];

            /* A zero or unaligned record length would loop forever or walk
             * out of the block; a corrupt directory must end the scan, not
             * hang the kernel. Removing this guard does not make a test fail,
             * it makes the test suite hang - which is the whole reason it
             * lives here rather than being left to the caller. */
            if (reclen < 8u || (reclen & 3u) != 0u || off + reclen > fs->block_size) {
                vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, reclen);
                break;
            }
            /* Bounded by the record, as readdir is (M-049): the block was the
             * bound here, which kept a comparison inside the buffer but let it
             * run into the next record. */
            if (nlen > reclen - 8u) {
                vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, nlen);
                break;
            }
            if (ino != 0u && nlen == name_len) {
                uint32_t i;
                for (i = 0; i < nlen; i++) {
                    if (block[off + 8u + i] != (uint8_t)name[i]) {
                        break;
                    }
                }
                if (i == nlen) {
                    return ino;
                }
            }
            off += reclen;
        }
    }
    return 0;
}

/* Walk a '/'-separated path from the root. */
uint32_t ext2_resolve(vibeos_ext2_t *fs, const char *path, uint8_t *inode) {
    uint32_t ino = VIBEOS_EXT2_ROOT_INO;
    uint32_t i = 0;

    if (ext2_read_inode(fs, ino, inode) != 0) {
        return 0;
    }
    while (path[i] == '/') {
        i++;
    }
    while (path[i]) {
        uint32_t start = i;
        uint32_t len;
        while (path[i] && path[i] != '/') {
            i++;
        }
        len = i - start;
        if (len > 0u) {
            if (!ext2_inode_is_dir(inode)) {
                return 0;   /* a path component that is not a directory */
            }
            ino = ext2_dir_find(fs, ino, inode, path + start, len);
            if (ino == 0u || ext2_read_inode(fs, ino, inode) != 0) {
                return 0;
            }
        }
        while (path[i] == '/') {
            i++;
        }
    }
    return ino;
}

/* ---- the filesystem interface --------------------------------------------*/

int ext2_op_lookup(void *fsv, const char *path, vibeos_fs_node_t *out) {
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
    out->id = ino;
    out->size = ext2_inode_size(inode);
    out->is_dir = ext2_inode_is_dir(inode);
    return 0;
}

long ext2_op_read_at(void *fsv, const vibeos_fs_node_t *node,
                            uint64_t offset, void *buf, uint32_t len) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t inode[EXT2_MAX_INODE_SIZE];
    uint8_t block[EXT2_MAX_BLOCK];
    uint8_t *dst = (uint8_t *)buf;
    uint64_t size;
    uint32_t done = 0;

    if (!fs || !fs->mounted || node->is_dir) {
        return -1;
    }
    /* The node carries an inode number, not the block pointers, so the inode
     * is fetched again here. See the header: this is the cost of a node
     * identity that cannot hold driver-private data. */
    if (ext2_read_inode(fs, (uint32_t)node->id, inode) != 0) {
        return -1;
    }
    size = ext2_inode_size(inode);
    if (offset >= size) {
        return 0;   /* end of file, which is not a failure */
    }
    /* len > size - offset, not offset + len > size: the sum wraps for a
     * size near 2^64 (H-011 audit). */
    if (len > size - offset) {
        len = (uint32_t)(size - offset);
    }
    if (ext2_inode_is_fast_symlink(fs, inode)) {
        for (; done < len; done++) {
            dst[done] = inode[40u + offset + done];
        }
        return (long)done;
    }
    while (done < len) {
        uint64_t bi64 = (offset + done) / fs->block_size;
        uint32_t bi = (uint32_t)bi64;
        uint32_t within = (uint32_t)((offset + done) % fs->block_size);
        uint32_t chunk = fs->block_size - within;
        uint64_t phys = 0;
        uint32_t i;

        /* A size from the volume can reach past the 2^32 blocks a block
         * number names: the narrowing would wrap to an earlier block of the
         * same file and read it as this one (#19). */
        if (bi64 > 0xFFFFFFFFull) {
            return (done > 0u) ? (long)done : -1;
        }

        if (chunk > len - done) {
            chunk = len - done;
        }
        if (ext2_map_block(fs, (uint32_t)node->id, inode, bi, &phys) != 0) {
            return (done > 0u) ? (long)done : -1;
        }
        if (phys == 0u) {
            /* A hole: ext2 stores nothing for a run of zeroes, and a reader
             * that treated this as an error would fail on a sparse file that
             * is perfectly valid. */
            for (i = 0; i < chunk; i++) {
                dst[done + i] = 0;
            }
        } else {
            if (ext2_read_block(fs, phys, block) != 0) {
                return (done > 0u) ? (long)done : -1;
            }
            for (i = 0; i < chunk; i++) {
                dst[done + i] = block[within + i];
            }
        }
        done += chunk;
    }
    return (long)done;
}

int ext2_op_list(void *fsv, const char *path, uint32_t index, char *name,
                        uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    vibeos_ext2_t *fs = (vibeos_ext2_t *)fsv;
    uint8_t dir[EXT2_MAX_INODE_SIZE];
    uint8_t block[EXT2_MAX_BLOCK];
    uint64_t size;
    uint32_t nblocks, bi, seen = 0, dir_ino;

    if (!fs || !fs->mounted || (dir_ino = ext2_resolve(fs, path, dir)) == 0u) {
        return -1;
    }
    if (!ext2_inode_is_dir(dir)) {
        return -1;
    }
    size = ext2_inode_size(dir);
    nblocks = ext2_dir_nblocks(fs, size);
    for (bi = 0; bi < nblocks; bi++) {
        uint64_t phys = 0;
        uint32_t off = 0;
        if (ext2_map_block(fs, dir_ino, dir, bi, &phys) != 0 || phys == 0u) {
            continue;
        }
        if (ext2_dir_block(fs, dir_ino, dir, phys, block) != 0) {
            return -1;
        }
        while (off + 8u <= fs->block_size) {
            uint32_t ino = rd32(block + off);
            uint16_t reclen = rd16(block + off + 4u);
            uint8_t nlen = block[off + 6u];

            if (reclen < 8u || (reclen & 3u) != 0u || off + reclen > fs->block_size) {
                vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, reclen);
                break;
            }
            /* The name lives inside its own record (M-049). This path had no
             * bound on it at all - ext2_dir_find checked the block - so a
             * name_len larger than the record copied the next records and, at
             * the end of a block, up to 254 bytes of this function's stack past
             * the block into an ordinary readdir(). */
            if (nlen > reclen - 8u) {
                vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, nlen);
                break;
            }
            if (ino != 0u && nlen > 0u) {
                if (seen == index) {
                    uint32_t i;
                    for (i = 0; i + 1u < name_cap && i < nlen; i++) {
                        name[i] = (char)block[off + 8u + i];
                    }
                    name[i] = 0;
                    if (out_size || out_is_dir) {
                        uint8_t child[EXT2_MAX_INODE_SIZE];
                        if (ext2_read_inode(fs, ino, child) == 0) {
                            if (out_size) {
                                *out_size = ext2_inode_size(child);
                            }
                            if (out_is_dir) {
                                *out_is_dir = ext2_inode_is_dir(child);
                            }
                        }
                    }
                    return 0;
                }
                seen++;
            }
            off += reclen;
        }
    }
    return -1;   /* past the last entry */
}

/* Read-only: every writing operation is absent, and the wrappers say EROFS. */
static const vibeos_fs_ops_t g_ext2_ops = {
    .lookup = ext2_op_lookup,
    .read_at = ext2_op_read_at,
    .list = ext2_op_list,
};

const vibeos_fs_ops_t *vibeos_ext2_ops(void) {
    return &g_ext2_ops;
}

/* ---- the storage scan's view of this driver (C7) -------------------------
 *
 * This file is the whole of what adding ext2 cost: the scan used to name it
 * in a probe table of its own, with a mount wrapper and a member of
 * vibeos_volume_t for its state. Now it declares itself, the volume supplies
 * the state, and the order it is tried in is the number below. */
_Static_assert(sizeof(vibeos_ext2_t) <= VIBEOS_FS_STATE_BYTES,
               "ext2's state must fit the storage a volume supplies");

/* The volume must fit the partition it was found in. Reads are bounded by the
 * superblock's blocks_count, which is the volume's own claim: a 100 MiB
 * partition whose superblock says more read the next partition (review v9
 * point 2, H-029's ext2 half). Checked by division, so a 64-bit
 * blocks_count cannot wrap the product. With no partition length known
 * (`sectors` 0) the product is still kept inside 64 bits, or part_lba +
 * block * sectors_per_block wraps for a block number past 2^61. */
static int ext2_fits_partition(const vibeos_ext2_t *fs, uint64_t sectors) {
    uint64_t spb = fs->block_size / VIBEOS_BLOCK_SIZE;

    if (spb == 0u) {
        return 0;
    }
    if (sectors != 0u) {
        return fs->blocks_count <= sectors / spb;
    }
    return fs->blocks_count <= (0xFFFFFFFFFFFFFFFFull - fs->part_lba) / spb;
}

static int ext2_driver_mount(vibeos_fsmount_t *out, vibeos_blockcache_t *bc,
                              uint64_t first_lba, uint64_t sectors, void *state)
{
    vibeos_ext2_t *fs = (vibeos_ext2_t *)state;

    if (fs == 0 || vibeos_ext2_mount(fs, bc, first_lba) != 0) {
        return -1;
    }
    if (!ext2_fits_partition(fs, sectors)) {
        fs->mounted = 0;
        return -1;
    }
    /* Under its own name only: a volume with extents or 64-bit numbers is
     * ext4's, and the ext4 descriptor below takes it, so what the storage scan
     * and /proc/mounts report says what the volume is. */
    if ((fs->incompat & (INCOMPAT_EXTENTS | INCOMPAT_64BIT)) != 0u) {
        fs->mounted = 0;
        return -1;
    }
    return vibeos_fs_mount(out, vibeos_ext2_ops(), fs, "ext2");
}

static int ext4_driver_mount(vibeos_fsmount_t *out, vibeos_blockcache_t *bc,
                             uint64_t first_lba, uint64_t sectors, void *state)
{
    vibeos_ext2_t *fs = (vibeos_ext2_t *)state;

    if (fs == 0 || vibeos_ext2_mount(fs, bc, first_lba) != 0) {
        return -1;
    }
    if (!ext2_fits_partition(fs, sectors)) {
        fs->mounted = 0;
        return -1;
    }
    /* The writer decides here, once, whether this volume is one it keeps true:
     * the operations it gets are the writing set or the reading one. */
    ext4_w_mount(fs);
    return vibeos_fs_mount(out, ext4_ops(fs), fs, "ext4");
}

static const vibeos_fs_driver_t g_ext2_driver = {
    "ext2", 0, ext2_driver_mount, 0, 30u
};
VIBEOS_FS_DRIVER(g_ext2_driver);

/* The same reader and, for a volume it can keep true, the writer
 * (ext4_write.c), tried after ext2 so that a plain ext2 volume keeps ext2's
 * name. */
static const vibeos_fs_driver_t g_ext4_driver = {
    "ext4", 0, ext4_driver_mount, 0, 31u
};
VIBEOS_FS_DRIVER(g_ext4_driver);

/* For a host test, which does not walk the linker section and registers the
 * drivers itself. A function rather than an exported object, so the
 * descriptor stays this file's own state. */
const vibeos_fs_driver_t *vibeos_ext2_fs_driver(void)
{
    return &g_ext2_driver;
}

/* And the ext4 one, for the exerciser that mounts a volume mkfs.ext4 made the
 * way the storage scan does - through the driver - because the choice between
 * reading it and writing it is made there. */
const vibeos_fs_driver_t *vibeos_ext4_fs_driver(void)
{
    return &g_ext4_driver;
}
