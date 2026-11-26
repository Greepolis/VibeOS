/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ext4's journal, jbd2's format (docs/filesystems/ext4/design.md step 3).
 *
 * Ordered mode, one operation to a transaction. While an operation runs, a
 * block of metadata it changes is not written where it lives: the first write
 * copies the block into the next free block of the log and every later read
 * and write of it - from the writer or from the reader's own helpers - is
 * answered there. The block cache is write-back, so nothing written in place
 * can be held back until the commit; the log is the only place a change can
 * wait without being on the volume. A file's data is written in place
 * straight away, which is what "ordered" means: by the time the commit names
 * it, it is on the disk.
 *
 * At the end of the operation (ext4_j_end), in two flushes:
 *   1. the descriptors are written, and the data and the logged copies with
 *      them - and, for the first transaction since the log was empty, the
 *      superblock says RECOVER: flush;
 *   2. the commit block - and for that first transaction the journal
 *      superblock pointing at it: flush. From here the operation is on the
 *      volume whatever happens next.
 * Then every logged block is written home, not flushed: until the log is
 * emptied, a cut replays it. The next transaction is appended after this one
 * (E4-016). When the next might not fit before the end of the log, the log is
 * emptied - flush, journal superblock empty, flush, RECOVER clear, flush -
 * which is also what a sync does, so a volume synced is a volume clean.
 * A power cut before 2 leaves the volume as it was before the operation, and
 * after it as it is after - replayed by ext4_j_recover at the next mount, or
 * by e2fsck, which reads the same format. The commit and the journal
 * superblock go in one flush on purpose: whichever of the two reaches the
 * disk alone, the scan finds no committed transaction or no log to scan.
 *
 * Several committed transactions in the log means a block an older one logged
 * can be freed and then written by a newer one as a file's data, in place -
 * and a replay would write the older copy over the data. Linux writes revoke
 * records for that. Here such a data write is logged instead (fs->j.logged,
 * a filter of what the live log holds): the newer copy is then in the log
 * too, and a replay applies it last. Replay still honours the revoke records
 * a journal Linux wrote carries.
 *
 * What one transaction may log is fs->j.cap blocks; an operation that needs
 * more is refused with ENOSPC part-way, and what it had done is committed as
 * a failed operation leaves it without a journal (counted, log_full).
 *
 * The superblock is the one block the rules above do not cover by
 * themselves, and the power-cut sweep (scripts/dev/verify-ext4-powercut.sh)
 * found both ways it breaks. It is the block that says RECOVER, so the copy
 * of it that goes home from the log must say RECOVER too (sb_log_recover),
 * or the checkpoint clears the flag under a full journal. And its checksum is
 * in its second sector, so its two sectors must reach the disk as one run: it
 * is written last before every flush (sb_block), and the cache flushes in
 * order of sector. What the device writes whole is a question for the block
 * layer, which writes a sector at a time (docs filesystems/ext4/progress.md E4-017).
 *
 * Ordered mode writes data in place and does not make an overwrite atomic:
 * a cut in the middle of one leaves a file with some of each - as on Linux -
 * so the sweep's script appends and replaces, and never overwrites in place.
 * For everything else it asserts the whole tree, contents included, moves
 * from one state to the next whole.
 */

#define memset(d, v, n) __builtin_memset((d), (v), (n))
#define memcpy(d, s, n) __builtin_memcpy((d), (s), (n))

#include "vibeos/ext2.h"
#include "vibeos/ext4_journal.h"
#include "vibeos/mbz.h"
#include "vibeos/abi_linux.h"
#include "ext2_internal.h"

#define JBD2_MAGIC 0xC03B3998u
#define JT_DESCRIPTOR 1u
#define JT_COMMIT 2u
#define JT_SB_V1 3u
#define JT_SB_V2 4u
#define JT_REVOKE 5u

#define JF_INCOMPAT_REVOKE 0x01u
#define JF_INCOMPAT_64BIT 0x02u
#define JF_INCOMPAT_ASYNC 0x04u
#define JF_INCOMPAT_CSUM_V2 0x08u
#define JF_INCOMPAT_CSUM_V3 0x10u
#define JF_INCOMPAT_KNOWN (JF_INCOMPAT_REVOKE | JF_INCOMPAT_64BIT | JF_INCOMPAT_ASYNC | JF_INCOMPAT_CSUM_V3)

#define TAG_ESCAPE 0x1u
#define TAG_SAME_UUID 0x2u
#define TAG_LAST 0x8u

#define JSB_CRC32C 4u
#define J_HOME_MAX 384u
/* Revoke records a replay can hold (M-158): 1000 refused a legitimate Linux
 * volume that had revoked more in the transactions left to replay. Beyond
 * this a replay is still refused rather than half done. In a table of its
 * own, hashed: the lookup was linear, once per logged block. */
#define J_REVOKE_MAX 8192u
#define J_REVOKE_SLOTS 16384u
/* The longest journal looked at (M-158): its blocks are mapped one by one at
 * mount, through ext3's block map when there is no extent tree, and a
 * superblock claiming 2^30 of them was a mount that did not end. mkfs makes
 * journals of at most 2^18 blocks; this is sixteen times that. */
#define J_MAXLEN_MAX (1u << 22)

static uint64_t g_commits, g_replayed, g_log_full, g_bad_tags, g_flushes, g_checkpoints, g_ops;

void vibeos_ext4_journal_stats(vibeos_ext4_journal_stats_t *out) {
    out->commits = __atomic_load_n(&g_commits, __ATOMIC_RELAXED);
    out->replayed = __atomic_load_n(&g_replayed, __ATOMIC_RELAXED);
    out->log_full = __atomic_load_n(&g_log_full, __ATOMIC_RELAXED);
    out->bad_tags = __atomic_load_n(&g_bad_tags, __ATOMIC_RELAXED);
    out->flushes = __atomic_load_n(&g_flushes, __ATOMIC_RELAXED);
    out->checkpoints = __atomic_load_n(&g_checkpoints, __ATOMIC_RELAXED);
    out->ops = __atomic_load_n(&g_ops, __ATOMIC_RELAXED);
}

static void count(uint64_t *c) {
    __atomic_add_fetch(c, 1u, __ATOMIC_RELAXED);
}

/* jbd2 is big-endian throughout; ext4 is little-endian. */
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t be16(const uint8_t *p) {
    return ((uint32_t)p[0] << 8) | (uint32_t)p[1];
}

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t spb(const vibeos_ext2_t *fs) {
    return fs->block_size / VIBEOS_BLOCK_SIZE;
}

/* ---- where the log is ----------------------------------------------------- */

/* A block of the journal, by its number inside the journal, as a block of the
 * volume; 0 if the journal's extents do not cover it. */
static uint64_t j_phys(const vibeos_ext2_t *fs, uint32_t jblk) {
    uint32_t i;

    if (fs->j.bmap) {
        uint64_t out = 0;

        /* The reader's own map walk; it keeps one indirect block cached, and
         * a log is read and written in order, so the walk is mostly that. */
        if (ext2_map_block((vibeos_ext2_t *)fs, fs->j.ino, fs->j.jinode, jblk, &out) != 0) {
            return 0;
        }
        return out < fs->blocks_count ? out : 0u;
    }

    for (i = 0; i < fs->j.nx; i++) {
        if (jblk >= fs->j.x[i].lblk && jblk - fs->j.x[i].lblk < fs->j.x[i].len) {
            return fs->j.x[i].phys + (jblk - fs->j.x[i].lblk);
        }
    }
    return 0;
}

static uint64_t j_lba(const vibeos_ext2_t *fs, uint32_t jblk, uint32_t sector) {
    return fs->part_lba + j_phys(fs, jblk) * spb(fs) + sector;
}

/* Where the k-th logged block of a transaction goes: the log starts at
 * s_first, and every tags_per_desc blocks are preceded by their descriptor. */
static uint32_t slot_jblk(const vibeos_ext2_t *fs, uint32_t k) {
    uint32_t t = fs->j.tags_per_desc;

    return fs->j.base + (k / t) * (t + 1u) + 1u + k % t;
}

static uint32_t desc_jblk(const vibeos_ext2_t *fs, uint32_t chunk) {
    return fs->j.base + chunk * (fs->j.tags_per_desc + 1u);
}

/* The next block of the log after `b`, which wraps to s_first. */
static uint32_t j_next(const vibeos_ext2_t *fs, uint32_t b) {
    return b + 1u >= fs->j.maxlen ? fs->j.first : b + 1u;
}

/* ---- whole journal blocks, read and written by sector --------------------- */

static int j_read_block(vibeos_ext2_t *fs, uint32_t jblk, uint8_t *out) {
    uint32_t i;

    if (j_phys(fs, jblk) == 0u) {
        return -1;
    }
    for (i = 0; i < spb(fs); i++) {
        if (vibeos_blockcache_read(fs->cache, j_lba(fs, jblk, i), out + i * VIBEOS_BLOCK_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

static int j_write_block(vibeos_ext2_t *fs, uint32_t jblk, const uint8_t *in) {
    uint32_t i;

    if (j_phys(fs, jblk) == 0u) {
        return -1;
    }
    for (i = 0; i < spb(fs); i++) {
        if (vibeos_blockcache_write(fs->cache, j_lba(fs, jblk, i), in + i * VIBEOS_BLOCK_SIZE) != 0) {
            return -1;
        }
    }
    return 0;
}

/* crc32c of a journal block as jbd2 seeds it, the block read a sector at a
 * time so that nothing here holds a second block. `zero_at` is a four-byte
 * field taken as zero (the block's own checksum), or ~0 for none; `seq_first`
 * prepends the big-endian sequence number, as a v3 tag's checksum does. */
static int j_block_crc(vibeos_ext2_t *fs, uint32_t jblk, uint32_t zero_at, int seq_first, uint32_t seq,
                       uint32_t *out) {
    uint8_t sector[VIBEOS_BLOCK_SIZE];
    uint32_t crc = fs->j.seed, i, k;

    if (seq_first) {
        uint8_t s[4];

        put_be32(s, seq);
        crc = vibeos_ext2_crc32c(crc, s, 4u);
    }
    for (i = 0; i < spb(fs); i++) {
        if (vibeos_blockcache_read(fs->cache, j_lba(fs, jblk, i), sector) != 0) {
            return -1;
        }
        for (k = 0; k < 4u; k++) {
            uint32_t at = zero_at + k;

            if (zero_at != ~0u && at / VIBEOS_BLOCK_SIZE == i) {
                sector[at % VIBEOS_BLOCK_SIZE] = 0;
            }
        }
        crc = vibeos_ext2_crc32c(crc, sector, VIBEOS_BLOCK_SIZE);
    }
    *out = crc;
    return 0;
}

/* The same over a block in memory. */
static uint32_t mem_crc(const vibeos_ext2_t *fs, const uint8_t *b, uint32_t zero_at) {
    uint8_t z[4] = { 0, 0, 0, 0 };
    uint32_t crc = vibeos_ext2_crc32c(fs->j.seed, b, zero_at);

    crc = vibeos_ext2_crc32c(crc, z, 4u);
    return vibeos_ext2_crc32c(crc, b + zero_at + 4u, fs->block_size - zero_at - 4u);
}

static int flush(vibeos_ext2_t *fs) {
    count(&g_flushes);
    return vibeos_blockcache_flush(fs->cache);
}

/* The filter of blocks the live log holds. */
static uint32_t logged_bit(uint64_t block) {
    return (uint32_t)((block * 0x9E3779B97F4A7C15ull) >> 52);   /* 12 bits */
}

static int logged_maybe(const vibeos_ext2_t *fs, uint64_t block) {
    uint32_t b = logged_bit(block);

    return fs->j.live && (fs->j.logged[b >> 3] & (1u << (b & 7u))) != 0u;
}

/* The most a transaction can take of the log: every block it may log, its
 * descriptors and its commit block. */
static uint32_t tx_room(const vibeos_ext2_t *fs) {
    return fs->j.cap + (fs->j.cap + fs->j.tags_per_desc - 1u) / fs->j.tags_per_desc + 1u;
}

/* ---- the journal superblock ----------------------------------------------- */

/* Read-modify-write of the journal superblock's two sectors through wbuf2,
 * which nothing holds across a call: the fields given are set, and its
 * checksum with them when the journal has one. */
static int jsb_store(vibeos_ext2_t *fs, uint32_t start, uint32_t seq, uint32_t set_incompat) {
    uint8_t *b = fs->wbuf2;

    if (vibeos_blockcache_read(fs->cache, j_lba(fs, 0u, 0u), b) != 0 ||
        vibeos_blockcache_read(fs->cache, j_lba(fs, 0u, 1u), b + VIBEOS_BLOCK_SIZE) != 0) {
        return -1;
    }
    put_be32(b + 0x18u, seq);
    put_be32(b + 0x1Cu, start);
    if (set_incompat != 0u) {
        put_be32(b + 0x28u, be32(b + 0x28u) | set_incompat);
        if ((set_incompat & JF_INCOMPAT_CSUM_V3) != 0u) {
            b[0x50u] = (uint8_t)JSB_CRC32C;
        }
    }
    if ((be32(b + 0x28u) & JF_INCOMPAT_CSUM_V3) != 0u) {
        put_be32(b + 0xFCu, 0u);
        put_be32(b + 0xFCu, vibeos_ext2_crc32c(~0u, b, 1024u));
    }
    if (vibeos_blockcache_write(fs->cache, j_lba(fs, 0u, 0u), b) != 0 ||
        vibeos_blockcache_write(fs->cache, j_lba(fs, 0u, 1u), b + VIBEOS_BLOCK_SIZE) != 0) {
        return -1;
    }
    return 0;
}

/* The volume's superblock, at home, with RECOVER set or clear: the on-disk
 * copy as it is (a transaction's own change to it is in the log, and goes
 * home at the checkpoint), not the writer's. */
static int sb_recover_flag(vibeos_ext2_t *fs, int set) {
    uint8_t *b = fs->wbuf2;
    uint32_t inc;

    if (vibeos_blockcache_read(fs->cache, fs->part_lba + 2u, b) != 0 ||
        vibeos_blockcache_read(fs->cache, fs->part_lba + 3u, b + VIBEOS_BLOCK_SIZE) != 0) {
        return -1;
    }
    inc = le32(b + 0x60u);
    inc = set ? (inc | INCOMPAT_RECOVER) : (inc & ~INCOMPAT_RECOVER);
    put_le32(b + 0x60u, inc);
    if (fs->csum) {
        put_le32(b + 0x3FCu, vibeos_ext2_crc32c(~0u, b, 0x3FCu));
    }
    if (vibeos_blockcache_write(fs->cache, fs->part_lba + 2u, b) != 0 ||
        vibeos_blockcache_write(fs->cache, fs->part_lba + 3u, b + VIBEOS_BLOCK_SIZE) != 0) {
        return -1;
    }
    return 0;
}

/* ---- finding the journal -------------------------------------------------- */

static int load_extents(vibeos_ext2_t *fs, const uint8_t *hdr, int depth_ok) {
    uint32_t n = hdr[2] | ((uint32_t)hdr[3] << 8), depth = hdr[6] | ((uint32_t)hdr[7] << 8), i;

    if ((hdr[0] | ((uint32_t)hdr[1] << 8)) != EXTENT_MAGIC) {
        return -1;
    }
    for (i = 0; i < n; i++) {
        const uint8_t *e = hdr + 12u + i * 12u;

        if (12u + (i + 1u) * 12u > fs->block_size) {
            return -1;
        }
        if (depth == 0u) {
            uint32_t len = e[4] | ((uint32_t)e[5] << 8);

            if (len > 32768u || fs->j.nx >= 16u) {
                return -1;   /* an unwritten extent, or more pieces than kept: not written */
            }
            fs->j.x[fs->j.nx].lblk = le32(e);
            fs->j.x[fs->j.nx].len = len;
            fs->j.x[fs->j.nx].phys = le32(e + 8u) | ((uint64_t)(e[6] | ((uint32_t)e[7] << 8)) << 32);
            if (fs->j.x[fs->j.nx].phys == 0u || fs->j.x[fs->j.nx].phys + len > fs->blocks_count) {
                return -1;
            }
            fs->j.nx++;
        } else {
            uint64_t leaf = le32(e + 4u) | ((uint64_t)(e[8] | ((uint32_t)e[9] << 8)) << 32);

            if (!depth_ok || depth != 1u || ext2_read_block(fs, leaf, fs->wbuf) != 0 ||
                load_extents(fs, fs->wbuf, 0) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int j_geometry(vibeos_ext2_t *fs) {
    uint8_t inode[EXT2_MAX_INODE_SIZE];
    uint8_t *b = fs->wbuf;
    uint32_t ino, bt, inc, i;

    memset(&fs->j, 0, sizeof(fs->j));
    if ((le32(fs->sb + 0x5Cu) & COMPAT_HAS_JOURNAL) == 0u) {
        return 0;
    }
    ino = le32(fs->sb + 0xE0u);
    if (ino == 0u /* an external journal, which this does not write */ ||
        ext2_read_inode(fs, ino, inode) != 0 || fs->block_size < 1024u) {
        return -1;
    }
    if ((le32(inode + 0x20u) & INODE_FLAG_EXTENTS) == 0u) {
        /* ext3's: block-mapped, and found through its block map (E4-020). */
        fs->j.bmap = 1;
        fs->j.ino = ino;
        memcpy(fs->j.jinode, inode, sizeof(fs->j.jinode));
    } else if (load_extents(fs, inode + 40u, 1) != 0) {
        return -1;
    }
    if (j_read_block(fs, 0u, b) != 0) {
        return -1;
    }
    bt = be32(b + 4u);
    if (be32(b) != JBD2_MAGIC || (bt != JT_SB_V1 && bt != JT_SB_V2) || be32(b + 0x0Cu) != fs->block_size) {
        return -1;
    }
    fs->j.maxlen = be32(b + 0x10u);
    fs->j.first = be32(b + 0x14u);
    fs->j.seq = be32(b + 0x18u);
    inc = bt == JT_SB_V2 ? be32(b + 0x28u) : 0u;
    if ((inc & ~JF_INCOMPAT_KNOWN) != 0u || be32(b + 0x20u) != 0u /* s_errno: the journal was aborted */ ||
        fs->j.first == 0u || fs->j.first >= fs->j.maxlen) {
        return -1;
    }
    /* No longer than the file that holds it, and than any journal mkfs makes
     * by far (M-158): checked before the walk below, which is that long. */
    {
        uint64_t isize = le32(inode + 4u) | ((uint64_t)le32(inode + 0x6Cu) << 32);

        if (fs->j.maxlen > J_MAXLEN_MAX || fs->j.maxlen > isize / fs->block_size) {
            return -1;
        }
    }
    for (i = 0; i < fs->j.maxlen; i++) {   /* every block of the log must be somewhere */
        if (j_phys(fs, i) == 0u) {
            return -1;
        }
    }
    if ((inc & JF_INCOMPAT_CSUM_V3) != 0u) {
        uint32_t want = be32(b + 0xFCu);

        if (b[0x50u] != JSB_CRC32C) {
            return -1;
        }
        put_be32(b + 0xFCu, 0u);
        if (vibeos_ext2_crc32c(~0u, b, 1024u) != want) {
            return ext2_csum_bad(ino);
        }
        put_be32(b + 0xFCu, want);
    }
    fs->j.start = be32(b + 0x1Cu);
    fs->j.csum3 = (inc & JF_INCOMPAT_CSUM_V3) != 0u;
    fs->j.is64 = (inc & JF_INCOMPAT_64BIT) != 0u;
    fs->j.seed = vibeos_ext2_crc32c(~0u, b + 0x30u, 16u);
    fs->j.tag_bytes = fs->j.csum3 ? 16u : (fs->j.is64 ? 12u : 8u);
    fs->j.tags_per_desc = (fs->block_size - 12u - 16u - (fs->j.csum3 ? 4u : 0u)) / fs->j.tag_bytes;
    /* The cap leaves room for its descriptors and the commit block - asked
     * before it is taken away (M-158): a log of one or two blocks wrapped the
     * subtraction to four billion, the clamp below made that J_HOME_MAX, and
     * the "too small" check let it through. */
    if (fs->j.tags_per_desc == 0u) {
        return -1;
    }
    {
        uint32_t room = fs->j.maxlen - fs->j.first;
        uint32_t overhead = room / (fs->j.tags_per_desc + 1u) + 2u;

        if (overhead >= room) {
            return -1;
        }
        fs->j.cap = room - overhead;
    }
    if (fs->j.cap > J_HOME_MAX) {
        fs->j.cap = J_HOME_MAX;
    }
    fs->j.base = fs->j.first;
    if (fs->j.cap < 16u) {
        return -1;
    }
    return 0;
}

/* For the writer. A journal mkfs made has no features; Linux turns on v3
 * checksums (and 64-bit tags for a 64-bit volume) the first time it mounts one
 * for writing, and so does this - here, with the log empty, because the
 * features decide the size of a tag and so where every logged block goes. A
 * log that is not empty belongs to a volume that should have said RECOVER:
 * it is not written over. */
int ext4_j_load(vibeos_ext2_t *fs) {
    uint32_t add = 0;

    if (j_geometry(fs) != 0) {
        memset(&fs->j, 0, sizeof(fs->j));
        return -1;
    }
    if (fs->j.maxlen == 0u) {
        return 0;   /* no journal: written as step 2 wrote it */
    }
    if (fs->j.start != 0u) {
        memset(&fs->j, 0, sizeof(fs->j));
        return -1;
    }
    if (fs->csum && !fs->j.csum3) {
        add |= JF_INCOMPAT_CSUM_V3;
    }
    if ((fs->incompat & INCOMPAT_64BIT) != 0u && !fs->j.is64) {
        add |= JF_INCOMPAT_64BIT;
    }
    if (add != 0u && (jsb_store(fs, 0u, fs->j.seq, add) != 0 || flush(fs) != 0 || j_geometry(fs) != 0)) {
        memset(&fs->j, 0, sizeof(fs->j));
        return -1;
    }
    fs->j.on = 1;
    return 0;
}

/* ---- reads and writes during a transaction -------------------------------- */

static int j_slot_of(const vibeos_ext2_t *fs, uint64_t block) {
    uint32_t i;

    for (i = 0; i < fs->j.n; i++) {
        if (fs->j.home[i] == block) {
            return (int)i;
        }
    }
    return -1;
}

int ext2_sec_read(vibeos_ext2_t *fs, uint64_t lba, void *buf) {
    if (fs->j.open && fs->j.n > 0u && lba >= fs->part_lba) {
        uint64_t rel = lba - fs->part_lba;
        int k = j_slot_of(fs, rel / spb(fs));

        if (k >= 0) {
            lba = j_lba(fs, slot_jblk(fs, (uint32_t)k), (uint32_t)(rel % spb(fs)));
        }
    }
    return vibeos_blockcache_read(fs->cache, lba, buf);
}

/* The slot that holds `block` in this transaction, the block copied into a new
 * one the first time it is written. */
static int j_capture(vibeos_ext2_t *fs, uint64_t block) {
    uint8_t sector[VIBEOS_BLOCK_SIZE];
    int k = j_slot_of(fs, block);
    uint32_t i, jb;

    if (k >= 0) {
        return k;
    }
    if (fs->j.n >= fs->j.cap) {
        count(&g_log_full);
        return -VIBEOS_ENOSPC;
    }
    k = (int)fs->j.n;
    jb = slot_jblk(fs, (uint32_t)k);
    for (i = 0; i < spb(fs); i++) {
        if (vibeos_blockcache_read(fs->cache, fs->part_lba + block * spb(fs) + i, sector) != 0 ||
            vibeos_blockcache_write(fs->cache, j_lba(fs, jb, i), sector) != 0) {
            return -VIBEOS_EIO;
        }
    }
    fs->j.home[k] = block;
    fs->j.escaped[(uint32_t)k >> 3] = (uint8_t)(fs->j.escaped[(uint32_t)k >> 3] & ~(1u << ((uint32_t)k & 7u)));
    fs->j.n++;
    return k;
}

int ext4_j_meta_write(vibeos_ext2_t *fs, uint64_t lba, const void *buf) {
    uint64_t rel;
    int k;

    if (!fs->j.open) {
        return vibeos_blockcache_write(fs->cache, lba, buf) == 0 ? 0 : -VIBEOS_EIO;
    }
    if (lba < fs->part_lba) {
        return -VIBEOS_EIO;
    }
    rel = lba - fs->part_lba;
    k = j_capture(fs, rel / spb(fs));
    if (k < 0) {
        return k;
    }
    return vibeos_blockcache_write(fs->cache, j_lba(fs, slot_jblk(fs, (uint32_t)k), (uint32_t)(rel % spb(fs))),
                                   buf) == 0
               ? 0
               : -VIBEOS_EIO;
}

static int freed_here(const vibeos_ext2_t *fs, uint64_t block) {
    uint32_t i;

    if (fs->j.freed_overflow) {
        return 1;
    }
    for (i = 0; i < fs->j.nfreed; i++) {
        if (block >= fs->j.freed[i].start && block - fs->j.freed[i].start < fs->j.freed[i].count) {
            return 1;
        }
    }
    return 0;
}

int ext4_j_data_write(vibeos_ext2_t *fs, uint64_t block, const uint8_t *buf) {
    uint32_t i;
    int logged = fs->j.open && (freed_here(fs, block) || j_slot_of(fs, block) >= 0 || logged_maybe(fs, block));

    for (i = 0; i < spb(fs); i++) {
        uint64_t lba = fs->part_lba + block * spb(fs) + i;
        int rc = logged ? ext4_j_meta_write(fs, lba, buf + i * VIBEOS_BLOCK_SIZE)
                        : (vibeos_blockcache_write(fs->cache, lba, buf + i * VIBEOS_BLOCK_SIZE) == 0 ? 0
                                                                                                     : -VIBEOS_EIO);
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

/* A long write stops here and returns short. Data over blocks the live log
 * still holds is logged, so one write can fill a transaction by itself - nightly
 * seed 124705 did at 2 MiB, and then had no room left for its own inode. The last
 * quarter stays for the metadata that closes the operation. */
int ext4_j_low(const vibeos_ext2_t *fs) {
    return fs->j.open && fs->j.n >= fs->j.cap - fs->j.cap / 4u;
}

/* A block this transaction frees may be named by the volume as it is on the
 * disk until the commit: an extent leaf, a directory block. Writing a file's
 * data over it in place before then would put data where the last committed
 * state keeps metadata, so such a block is logged when it is written again. */
void ext4_j_freed(vibeos_ext2_t *fs, uint64_t start, uint64_t count_) {
    if (!fs->j.open || count_ == 0u) {
        return;
    }
    if (fs->j.nfreed > 0u && fs->j.freed[fs->j.nfreed - 1u].start + fs->j.freed[fs->j.nfreed - 1u].count == start) {
        fs->j.freed[fs->j.nfreed - 1u].count += count_;
        return;
    }
    if (fs->j.nfreed >= 16u) {
        fs->j.freed_overflow = 1;
        return;
    }
    fs->j.freed[fs->j.nfreed].start = start;
    fs->j.freed[fs->j.nfreed].count = count_;
    fs->j.nfreed++;
}

/* ---- commit --------------------------------------------------------------- */

/* Group commit (E4-016, the contract decided 2026-11-16: Linux's). A
 * transaction is not one operation: it stays open across them and is
 * committed when it is a quarter full - so the next operation still has
 * three quarters of a transaction to itself - when it is older than the
 * commit interval, on sync (fsync, sync, O_SYNC), and from the idle loop when
 * nothing else commits it (ext4_j_tick). An operation can therefore return
 * before it is on the disk; a cut takes back what the last commit did not
 * hold, and never what a sync returned for. */
#define J_COMMIT_NS (5ull * 1000000000ull)

static int j_commit(vibeos_ext2_t *fs);

static int j_due(const vibeos_ext2_t *fs) {
    uint64_t now = vibeos_fs_now_ns();

    return now != 0u && fs->j.started != 0u && now - fs->j.started >= J_COMMIT_NS;
}

int ext4_j_begin(vibeos_ext2_t *fs) {
    if (fs->j.dead) {
        return -VIBEOS_EROFS;
    }
    if (!fs->j.on) {
        return 0;
    }
    count(&g_ops);
    if (fs->j.open) {
        int rc;

        if (fs->j.n <= fs->j.cap / 4u && !j_due(fs)) {
            return 0;   /* the running transaction takes this operation too */
        }
        rc = j_commit(fs);
        if (rc != 0) {
            return rc;
        }
    }
    fs->j.open = 1;
    fs->j.n = 0;
    fs->j.nfreed = 0;
    fs->j.freed_overflow = 0;
    fs->j.started = vibeos_fs_now_ns();
    return 0;
}

/* The end of an operation, which is not the end of its transaction. One that
 * logged nothing closes it, so reads stop looking in the log. */
int ext4_j_end(vibeos_ext2_t *fs) {
    if (fs->j.dead) {
        return -VIBEOS_EIO;
    }
    if (fs->j.open && fs->j.n == 0u) {
        fs->j.open = 0;
    }
    return 0;
}

int ext4_j_commit(vibeos_ext2_t *fs) {
    return fs->j.dead ? -VIBEOS_EIO : j_commit(fs);
}

int ext4_j_tick(vibeos_ext2_t *fs) {
    if (fs->j.dead || !fs->j.open || fs->j.n == 0u || !j_due(fs)) {
        return 0;
    }
    return j_commit(fs);
}

/* The descriptors: one per tags_per_desc logged blocks, each tag naming the
 * block's home, its escape flag and - with v3 - its checksum. A logged block
 * that begins with the journal's magic would read as a journal block in a
 * scan, so its first four bytes are zero in the log and the tag says so. */
static int write_descriptors(vibeos_ext2_t *fs, uint32_t tid) {
    uint8_t *d = fs->wbuf2;
    uint32_t chunks = (fs->j.n + fs->j.tags_per_desc - 1u) / fs->j.tags_per_desc, c;

    for (c = 0; c < chunks; c++) {
        uint32_t k0 = c * fs->j.tags_per_desc, k, at = 12u;
        uint32_t k1 = k0 + fs->j.tags_per_desc < fs->j.n ? k0 + fs->j.tags_per_desc : fs->j.n;

        for (k = k0; k < k1; k++) {
            uint8_t sector[VIBEOS_BLOCK_SIZE];
            uint32_t jb = slot_jblk(fs, k), flags = 0, crc = 0;

            if (vibeos_blockcache_read(fs->cache, j_lba(fs, jb, 0u), sector) != 0) {
                return -1;
            }
            if (be32(sector) == JBD2_MAGIC) {
                put_be32(sector, 0u);
                if (vibeos_blockcache_write(fs->cache, j_lba(fs, jb, 0u), sector) != 0) {
                    return -1;
                }
                fs->j.escaped[k >> 3] = (uint8_t)(fs->j.escaped[k >> 3] | (1u << (k & 7u)));
                flags |= TAG_ESCAPE;
            }
            if (fs->j.csum3 && j_block_crc(fs, jb, ~0u, 1, tid, &crc) != 0) {
                return -1;
            }
            /* The tags are written into wbuf2 only now: j_block_crc and the
             * escape above read sectors through their own buffer. */
            if (k == k0) {
                memset(d, 0, fs->block_size);
                put_be32(d, JBD2_MAGIC);
                put_be32(d + 4u, JT_DESCRIPTOR);
                put_be32(d + 8u, tid);
            }
            if (k > k0) {
                flags |= TAG_SAME_UUID;
            }
            if (k + 1u == k1) {
                flags |= TAG_LAST;
            }
            put_be32(d + at, (uint32_t)fs->j.home[k]);
            if (fs->j.csum3) {
                put_be32(d + at + 4u, flags);
                put_be32(d + at + 8u, (uint32_t)(fs->j.home[k] >> 32));
                put_be32(d + at + 12u, crc);
            } else {
                d[at + 6u] = (uint8_t)(flags >> 8);
                d[at + 7u] = (uint8_t)flags;
                if (fs->j.is64) {
                    put_be32(d + at + 8u, (uint32_t)(fs->j.home[k] >> 32));
                }
            }
            at += fs->j.tag_bytes;
            if (k == k0) {
                uint8_t *u = d + at;   /* the first tag is followed by the journal's uuid */

                if (vibeos_blockcache_read(fs->cache, j_lba(fs, 0u, 0u), sector) != 0) {
                    return -1;
                }
                memcpy(u, sector + 0x30u, 16u);
                at += 16u;
            }
        }
        if (fs->j.csum3) {
            put_be32(d + fs->block_size - 4u, mem_crc(fs, d, fs->block_size - 4u));
        }
        if (j_write_block(fs, desc_jblk(fs, c), d) != 0) {
            return -1;
        }
    }
    return 0;
}

static int write_commit(vibeos_ext2_t *fs, uint32_t tid) {
    uint8_t *b = fs->wbuf2;
    uint32_t chunks = (fs->j.n + fs->j.tags_per_desc - 1u) / fs->j.tags_per_desc;
    uint32_t jb = fs->j.base + fs->j.n + chunks;

    memset(b, 0, fs->block_size);
    put_be32(b, JBD2_MAGIC);
    put_be32(b + 4u, JT_COMMIT);
    put_be32(b + 8u, tid);
    if (fs->j.csum3) {
        put_be32(b + 16u, mem_crc(fs, b, 16u));
    }
    return j_write_block(fs, jb, b);
}

/* The block that holds the superblock, which every writer here puts last
 * before a flush: written among others, the cache may evict one of its two
 * sectors early, and a cut between them leaves a superblock whose checksum
 * matches neither (blockcache.c, the flush's order). */
static uint64_t sb_block(const vibeos_ext2_t *fs) {
    return 1024u / fs->block_size;
}

/* Every logged block home, the escaped ones with their magic put back, the
 * superblock's last. */
static int checkpoint(vibeos_ext2_t *fs) {
    uint8_t sector[VIBEOS_BLOCK_SIZE];
    uint32_t k, i, pass;

    for (pass = 0; pass < 2u; pass++) {
        for (k = 0; k < fs->j.n; k++) {
            if ((fs->j.home[k] == sb_block(fs)) != (pass == 1u)) {
                continue;
            }
            for (i = 0; i < spb(fs); i++) {
                if (vibeos_blockcache_read(fs->cache, j_lba(fs, slot_jblk(fs, k), i), sector) != 0) {
                    return -1;
                }
                if (i == 0u && (fs->j.escaped[k >> 3] & (1u << (k & 7u))) != 0u) {
                    put_be32(sector, JBD2_MAGIC);
                }
                if (vibeos_blockcache_write(fs->cache, fs->part_lba + fs->j.home[k] * spb(fs) + i, sector) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* The superblock's logged copy says what the writer holds, without RECOVER,
 * so putting it home would clear the flag while the journal still holds the
 * transaction - and e2fsck refuses to replay that ("needs_recovery flag is
 * clear, but journal has data"). Rewriting the flag after the checkpoint is
 * not enough: the cache evicts the checkpoint's sectors whenever it likes, so
 * there is a moment with the logged copy on the disk. Found by the power-cut
 * sweep, both times. So the logged copy itself carries RECOVER, set before
 * the descriptors are checksummed; a replay writes it home with the flag on,
 * which is what the volume says until the replay has finished. */
static int sb_log_recover(vibeos_ext2_t *fs) {
    uint8_t *b = fs->wbuf2;
    uint32_t sb_block = 1024u / fs->block_size, s = (1024u % fs->block_size) / VIBEOS_BLOCK_SIZE, jb;
    int k = j_slot_of(fs, sb_block);

    if (k < 0) {
        return 0;
    }
    jb = slot_jblk(fs, (uint32_t)k);
    if (vibeos_blockcache_read(fs->cache, j_lba(fs, jb, s), b) != 0 ||
        vibeos_blockcache_read(fs->cache, j_lba(fs, jb, s + 1u), b + VIBEOS_BLOCK_SIZE) != 0) {
        return -1;
    }
    put_le32(b + 0x60u, le32(b + 0x60u) | INCOMPAT_RECOVER);
    if (fs->csum) {
        put_le32(b + 0x3FCu, vibeos_ext2_crc32c(~0u, b, 0x3FCu));
    }
    if (vibeos_blockcache_write(fs->cache, j_lba(fs, jb, s), b) != 0 ||
        vibeos_blockcache_write(fs->cache, j_lba(fs, jb, s + 1u), b + VIBEOS_BLOCK_SIZE) != 0) {
        return -1;
    }
    return 0;
}

/* Empty the log: everything it holds is at home already (written after each
 * commit), so make that durable, then say the log is empty, then clear
 * RECOVER - each step on the disk before the next. */
static int empty_log(vibeos_ext2_t *fs) {
    if (!fs->j.live) {
        return 0;
    }
    if (flush(fs) != 0 || jsb_store(fs, 0u, fs->j.seq, 0u) != 0 || flush(fs) != 0 || sb_recover_flag(fs, 0) != 0 ||
        flush(fs) != 0) {
        return -1;
    }
    count(&g_checkpoints);
    fs->j.live = 0;
    fs->j.base = fs->j.first;
    memset(fs->j.logged, 0, sizeof(fs->j.logged));
    fs->j.logged_set = 0;
    return 0;
}

/* fsync's promise: what the running transaction holds is on the disk when
 * this returns. Two flushes; the log stays as it is. */
int ext4_j_sync(vibeos_ext2_t *fs) {
    if (!fs->j.on) {
        return 0;
    }
    return ext4_j_commit(fs);
}

/* The volume clean as it stands: committed, and the log emptied - for the
 * end of userland and for an image about to be handed to anything else. */
int ext4_j_settle(vibeos_ext2_t *fs) {
    int rc;

    if (!fs->j.on) {
        return 0;
    }
    rc = ext4_j_commit(fs);
    if (rc == 0 && empty_log(fs) != 0) {
        fs->j.dead = 1;
        rc = -VIBEOS_EIO;
    }
    return rc;
}

static int j_commit(vibeos_ext2_t *fs) {
    uint32_t tid = fs->j.seq, chunks, k;
    int bad, first = !fs->j.live;

    if (!fs->j.open) {
        return 0;
    }
    fs->j.open = 0;   /* from here every read is of the volume as written */
    if (fs->j.n == 0u) {
        return 0;
    }
    bad = sb_log_recover(fs) != 0 || write_descriptors(fs, tid) != 0 || (first && sb_recover_flag(fs, 1) != 0) ||
          flush(fs) != 0 || write_commit(fs, tid) != 0 || (first && jsb_store(fs, fs->j.base, tid, 0u) != 0) ||
          flush(fs) != 0;
    if (!bad) {
        count(&g_commits);
        fs->j.live = 1;
        for (k = 0; k < fs->j.n; k++) {
            uint32_t b = logged_bit(fs->j.home[k]);

            if ((fs->j.logged[b >> 3] & (1u << (b & 7u))) == 0u) {
                fs->j.logged[b >> 3] = (uint8_t)(fs->j.logged[b >> 3] | (1u << (b & 7u)));
                fs->j.logged_set++;
            }
        }
        /* Home from where this transaction is in the log - before base
         * moves past it. */
        bad = checkpoint(fs) != 0;
        chunks = (fs->j.n + fs->j.tags_per_desc - 1u) / fs->j.tags_per_desc;
        fs->j.base += fs->j.n + chunks + 1u;
    }
    fs->j.seq = tid + 1u;
    fs->j.n = 0;
    /* The next transaction must fit before the end of the log - and the
     * filter of what the log holds must still mean something. Every bit it
     * has set makes a data write over that block a logged one, so a filter
     * left to fill up logs nearly every data block: on a volume of 1 KiB
     * blocks the log holds thousands of homes, and a 1 MiB write logged most
     * of itself, ran past what one transaction may log and failed (the
     * nightly, seed 118183). Past a sixteenth of its bits the log is emptied,
     * which empties the filter: under one data write in sixteen is logged. */
    if (!bad && (fs->j.base + tx_room(fs) > fs->j.maxlen ||
                 fs->j.logged_set > (uint32_t)sizeof(fs->j.logged) * 8u / 16u)) {
        bad = empty_log(fs) != 0;
    }
    if (bad) {
        /* The volume is what the disk says, and what the writer holds of it
         * may not be: no more writes until it is mounted again, which
         * replays whatever was committed. */
        fs->j.dead = 1;
        return -VIBEOS_EIO;
    }
    return 0;
}

/* ---- replay --------------------------------------------------------------- */

/* The revoke records of the transactions being replayed: a block and the
 * newest transaction that revoked it, open-addressed by block. One table for
 * every volume, so one replay at a time: a second finds it taken and is
 * refused (g_revoke_busy) - mounts are made one after another, so it is
 * never expected. */
typedef struct {
    uint64_t block;
    uint32_t seq;
    uint32_t used;
} revoke_t;

static revoke_t g_revoke[J_REVOKE_SLOTS];   /* quota: none - a replay's scratch, one at a time */
static volatile uint32_t g_revoke_busy;

static revoke_t *revoke_slot(uint64_t block) {
    uint32_t h = (uint32_t)((block * 0x9E3779B97F4A7C15ull) >> 50) & (J_REVOKE_SLOTS - 1u), k;

    for (k = 0; k < J_REVOKE_SLOTS; k++) {
        revoke_t *r = &g_revoke[(h + k) & (J_REVOKE_SLOTS - 1u)];

        if (!r->used || r->block == block) {
            return r;
        }
    }
    return 0;
}

static int revoked(vibeos_ext2_t *fs, uint32_t nrev, uint64_t block, uint32_t seq) {
    const revoke_t *r;

    (void)fs;
    if (nrev == 0u) {
        return 0;
    }
    r = revoke_slot(block);
    return r && r->used && (int32_t)(r->seq - seq) >= 0;
}

/* A tag at `at` of the descriptor in wbuf: its home and flags. */
static void tag_read(const vibeos_ext2_t *fs, const uint8_t *t, uint64_t *block, uint32_t *flags, uint32_t *csum) {
    *block = be32(t);
    if (fs->j.csum3) {
        *flags = be32(t + 4u);
        *block |= (uint64_t)be32(t + 8u) << 32;
        *csum = be32(t + 12u);
    } else {
        *flags = be16(t + 6u);
        if (fs->j.is64) {
            *block |= (uint64_t)be32(t + 8u) << 32;
        }
        *csum = 0;
    }
}

static int desc_ok(vibeos_ext2_t *fs, const uint8_t *b) {
    return !fs->j.csum3 || mem_crc(fs, b, fs->block_size - 4u) == be32(b + fs->block_size - 4u);
}

/* One pass over the log from s_start. Pass 0 finds where the committed
 * transactions end, pass 1 collects their revoke records, pass 2 writes their
 * blocks home but the superblock's, which pass 3 writes last (sb_block).
 * Pass 0 returns the number of committed transactions; the others 0, or -1. */
static int j_pass(vibeos_ext2_t *fs, int pass, uint32_t start, uint32_t seq0, uint32_t end_seq, uint32_t *nrev) {
    uint8_t *b = fs->wbuf;
    uint32_t pos = start, seq = seq0, steps = 0;

    for (;;) {
        uint32_t bt;

        if (pass > 0 && seq == end_seq) {
            return 0;
        }
        if (++steps > 4u * fs->j.maxlen || j_read_block(fs, pos, b) != 0) {
            return pass == 0 ? (int)(seq - seq0) : -1;
        }
        if (be32(b) != JBD2_MAGIC || be32(b + 8u) != seq) {
            return pass == 0 ? (int)(seq - seq0) : -1;
        }
        bt = be32(b + 4u);
        if (bt == JT_DESCRIPTOR) {
            uint32_t at = 12u, limit = fs->block_size - (fs->j.csum3 ? 4u : 0u);

            if (!desc_ok(fs, b)) {
                return pass == 0 ? (int)(seq - seq0) : -1;
            }
            pos = j_next(fs, pos);
            while (at + fs->j.tag_bytes <= limit) {
                uint64_t home;
                uint32_t flags, csum, got;

                tag_read(fs, b + at, &home, &flags, &csum);
                at += fs->j.tag_bytes + ((flags & TAG_SAME_UUID) ? 0u : 16u);
                if (pass >= 2 && (home == sb_block(fs)) == (pass == 3) && !revoked(fs, *nrev, home, seq)) {
                    uint8_t sector[VIBEOS_BLOCK_SIZE];
                    uint32_t i;

                    if (fs->j.csum3 && (j_block_crc(fs, pos, ~0u, 1, seq, &got) != 0 || got != csum)) {
                        count(&g_bad_tags);   /* jbd2 skips the block too, and says so */
                    } else if (home < fs->first_data_block || home >= fs->blocks_count) {
                        vibeos_mbz_hit(VIBEOS_MBZ_EXT2_BAD_METADATA, home);
                        return -1;
                    } else {
                        for (i = 0; i < spb(fs); i++) {
                            if (vibeos_blockcache_read(fs->cache, j_lba(fs, pos, i), sector) != 0) {
                                return -1;
                            }
                            if (i == 0u && (flags & TAG_ESCAPE) != 0u) {
                                put_be32(sector, JBD2_MAGIC);
                            }
                            if (vibeos_blockcache_write(fs->cache, fs->part_lba + home * spb(fs) + i, sector) != 0) {
                                return -1;
                            }
                        }
                    }
                }
                pos = j_next(fs, pos);
                if ((flags & TAG_LAST) != 0u) {
                    break;
                }
                /* The descriptor in wbuf is still the one being walked: the
                 * replay above reads sectors through its own buffer. */
            }
        } else if (bt == JT_COMMIT) {
            if (fs->j.csum3 && mem_crc(fs, b, 16u) != be32(b + 16u)) {
                return pass == 0 ? (int)(seq - seq0) : -1;
            }
            pos = j_next(fs, pos);
            seq++;
        } else if (bt == JT_REVOKE) {
            uint32_t used = be32(b + 12u), rs = fs->j.is64 ? 8u : 4u, at;

            if (!desc_ok(fs, b) || used > fs->block_size - (fs->j.csum3 ? 4u : 0u) || used < 16u) {
                return pass == 0 ? (int)(seq - seq0) : -1;
            }
            if (pass == 1) {
                for (at = 16u; at + rs <= used; at += rs) {
                    uint64_t blk = rs == 8u ? ((uint64_t)be32(b + at) << 32) | be32(b + at + 4u) : be32(b + at);
                    uint32_t i;

                    revoke_t *r = revoke_slot(blk);

                    (void)i;
                    if (!r) {
                        return -1;
                    }
                    if (!r->used) {
                        if (*nrev >= J_REVOKE_MAX) {
                            return -1;
                        }
                        (*nrev)++;
                        r->used = 1;
                        r->block = blk;
                        r->seq = seq;
                    } else if ((int32_t)(seq - r->seq) > 0) {
                        r->seq = seq;
                    }
                }
            }
            pos = j_next(fs, pos);
        } else {
            return pass == 0 ? (int)(seq - seq0) : -1;
        }
    }
}

int ext4_j_recover(vibeos_ext2_t *fs) {
    uint32_t start, seq0, nrev = 0;
    int n;

    if (fs->cache == 0 || fs->cache->dev == 0 || fs->cache->dev->write == 0) {
        return -1;   /* a journal to replay on a device that takes no writes: not read past */
    }
    if (vibeos_blockcache_read(fs->cache, fs->part_lba + 2u, fs->sb) != 0 ||
        vibeos_blockcache_read(fs->cache, fs->part_lba + 3u, fs->sb + VIBEOS_BLOCK_SIZE) != 0 ||
        j_geometry(fs) != 0 || fs->j.maxlen == 0u || j_read_block(fs, 0u, fs->wbuf) != 0) {
        return -1;
    }
    start = be32(fs->wbuf + 0x1Cu);
    seq0 = be32(fs->wbuf + 0x18u);
    if (start != 0u) {
        int bad;

        if (start < fs->j.first || start >= fs->j.maxlen) {
            return -1;
        }
        if (__atomic_exchange_n(&g_revoke_busy, 1u, __ATOMIC_ACQUIRE) != 0u) {
            return -1;   /* another volume is being replayed */
        }
        memset(g_revoke, 0, sizeof(g_revoke));
        n = j_pass(fs, 0, start, seq0, 0u, &nrev);
        bad = n < 0 || (n > 0 && (j_pass(fs, 1, start, seq0, seq0 + (uint32_t)n, &nrev) != 0 ||
                                  j_pass(fs, 2, start, seq0, seq0 + (uint32_t)n, &nrev) != 0 ||
                                  j_pass(fs, 3, start, seq0, seq0 + (uint32_t)n, &nrev) != 0));
        __atomic_store_n(&g_revoke_busy, 0u, __ATOMIC_RELEASE);
        if (bad) {
            return -1;
        }
        __atomic_add_fetch(&g_replayed, (uint64_t)n, __ATOMIC_RELAXED);
        seq0 += (uint32_t)n + 1u;
    }
    if (flush(fs) != 0 || jsb_store(fs, 0u, seq0, 0u) != 0 || flush(fs) != 0 || sb_recover_flag(fs, 0) != 0 ||
        flush(fs) != 0) {
        return -1;
    }
    memset(fs->ext, 0, sizeof(fs->ext));
    return 0;
}
