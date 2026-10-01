/* FAT16/FAT32: the boot volume's filesystem, and any other volume formatted so.
 *
 * It began as a read-only reader over the virtio-blk driver - an MBR, a BPB, an
 * 8.3 name in the root directory - which is why it sat in kernel/arch/x86_64
 * until docs/abi/ L1 step 4. What it took from the architecture is registered
 * now (vibeos/fat.h): the boot disk, the lock, and a hook the architecture
 * prints a line from when a volume mounts. Nothing here names a machine.
 */

#include <stdint.h>

#include "vibeos/fat.h"
#include "vibeos/fat_chain.h"
#include "vibeos/blockdev.h"
#include "vibeos/storage.h"
#include "vibeos/vfs.h"
#include "vibeos/mbz.h"

/* The boot disk, as the architecture registered it. Absent - a host test that
 * mounts only images, a machine with no disk - every transfer fails, and the
 * boot volume does not mount. */
static int (*g_boot_read)(uint64_t lba, void *buf);
static int (*g_boot_write)(uint64_t lba, const void *buf);
static int (*g_boot_read_many)(uint64_t lba, void *buf, uint32_t sectors);
static void (*g_mount_hook)(int is_fat32, uint32_t part_lba, uint32_t data_lba);

void vibeos_fat_set_boot_device(int (*read)(uint64_t lba, void *buf),
                                int (*write)(uint64_t lba, const void *buf),
                                int (*read_many)(uint64_t lba, void *buf, uint32_t sectors)) {
    g_boot_read = read;
    g_boot_write = write;
    g_boot_read_many = read_many;
}

void vibeos_fat_set_mount_hook(void (*hook)(int is_fat32, uint32_t part_lba, uint32_t data_lba)) {
    g_mount_hook = hook;
}

static int boot_read(uint64_t lba, void *buf) {
    return g_boot_read ? g_boot_read(lba, buf) : -1;
}

static int boot_write(uint64_t lba, const void *buf) {
    return g_boot_write ? g_boot_write(lba, buf) : -1;
}

typedef struct {
    uint32_t part_lba;        /* partition start sector           */
    uint32_t fat_lba;         /* first FAT sector                 */
    uint32_t root_lba;        /* FAT16 root dir sector (0 if F32) */
    uint32_t data_lba;        /* first data sector                */
    uint32_t sectors_per_fat;
    uint32_t max_clusters;
    uint32_t part_sectors;    /* partition length, from the BPB   */
    uint32_t root_cluster;    /* FAT32 root cluster               */
    uint16_t root_entries;    /* FAT16 root entry count           */
    uint8_t  sectors_per_cluster;
    uint8_t  is_fat32;
    uint32_t alloc_hint;      /* where the last search for a free cluster ended */
    int      mounted;
    /* Which cache - and so which device - this volume lives on.
     *
     * Was implicit: there was one volume and one cache and the code named the
     * cache directly. A volume that does not know its own device cannot be the
     * second one. */
    vibeos_blockcache_t *cache;
} fat_fs_t;

/* Volumes, and the one the operation in hand is on.
 *
 * This driver had a single global, and that was the structural reason it could
 * mount exactly one filesystem - not a missing feature, a missing *place to
 * put* a second one. The same shape as the VFS mount table before I4b step 4,
 * one layer down.
 *
 * A current-volume pointer rather than a `fat_fs_t *` threaded through thirty
 * static functions. Both are correct; this one is a change that can be read in
 * an afternoon and verified by a boot, and the alternative is a wide edit to
 * the driver the machine boots from. The pointer is safe for exactly one
 * reason and it is worth being explicit about it: **every public entry point
 * here takes fs_lock for the whole operation**, so there is never more than
 * one operation in flight and never a moment when the current volume is
 * ambiguous.
 *
 * What that costs is real and is not a correctness problem: two volumes cannot
 * be read at the same time. This driver already serialised everything through
 * that one lock, so nothing got slower - but it is the limit to lift if a
 * second volume ever carries real traffic, and lifting it means threading the
 * parameter after all.
 */
#define FAT_MAX_VOLUMES 4u

static fat_fs_t g_volumes[FAT_MAX_VOLUMES];
static uint32_t g_volume_count;
static fat_fs_t *g_fat_cur = &g_volumes[0];

/* Make `vol` the one the next operation acts on; null means the boot volume.
 * Called with the lock held, always.
 *
 * Every public entry point that takes fs_lock must call this. One that does
 * not operates on whatever the previous caller left selected, silently and on
 * the wrong volume - which is exactly what happened the first time a second
 * volume existed: file_extent read the scratch volume and the swap area
 * reported that this medium has no swap file. */
static void fat_select(void *vol) {
    g_fat_cur = vol ? (fat_fs_t *)vol : &g_volumes[0];
}

/* ---- one cache, not three (I2) --------------------------------------------
 *
 * Every single-sector read this filesystem does goes through
 * kernel/fs/blockcache.c now, instead of straight at the driver. That layer
 * was written, host-tested and sabotage-verified, and until this change no
 * booting machine had ever executed a line of it: exfat, ext2, iso9660 and the
 * journal are its only other callers and none of them mount. Same shape as the
 * swap stack before it was given somewhere to write.
 *
 * ## Write-through, deliberately, and only for now
 *
 * The cache can do write-back and this does not use it. The reason is
 * the boot disk's multi-sector read, which stays a direct bulk transfer: a
 * two-megabyte image read one sector at a time through a cache would undo the
 * multi-sector path, and that path is what turned a FAT read from something
 * indistinguishable from a hang into an ordinary read.
 *
 * A bulk read that bypasses the cache is only safe while the device is
 * authoritative - which write-back is precisely the thing that stops being
 * true. So writes go to the cache *and* to the medium, and the device never
 * holds anything older than the cache does. Write-back belongs with I4, where
 * writes get read back and proved; turning it on here would be trading a
 * verified property for an unverified one.
 *
 * ## Sixty-four slots
 *
 * 32 KiB. The access pattern that matters is a chain walk, which reads the
 * same handful of table sectors over and over - the single-sector cache this
 * replaces was worth about a thousand round trips on a two-megabyte file all
 * by itself. Sixty-four holds a directory and its table sectors together,
 * which is the case a single sector could not.
 */
#define FAT_CACHE_SLOTS 64u

static uint8_t g_bc_data[FAT_CACHE_SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_bc_slots[FAT_CACHE_SLOTS];
static vibeos_blockdev_t g_bc_dev;
static vibeos_blockcache_t g_bc;
static int g_bc_ready;

static int bc_dev_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    return boot_read(lba, buf);
}

static int bc_dev_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    return boot_write(lba, buf);
}

/* Brought up on mount, and torn down the same way: a cache that outlived a
 * mount would answer for a volume that is no longer there. */
static void fat_cache_bringup(void) {
    uint32_t i;

    if (g_bc_ready) {
        vibeos_blockcache_invalidate(&g_bc);
        return;
    }
    for (i = 0; i < FAT_CACHE_SLOTS; i++) {
        g_bc_slots[i].data = g_bc_data[i];
    }
    g_bc_dev.read = bc_dev_read;
    g_bc_dev.write = bc_dev_write;
    g_bc_dev.flush = 0;
    g_bc_dev.ctx = 0;
    /* Zero means "the size is unknown", and the cache then declines to
     * bounds-check. It is checked one layer down by kernel/io/blkdev.c, which
     * knows the real capacity because the driver had to say it to register -
     * so leaving it zero here is not skipping the check, it is not doing it
     * twice with two sources of truth. */
    g_bc_dev.sectors = 0;
    if (vibeos_blockcache_init(&g_bc, &g_bc_dev, g_bc_slots,
                               FAT_CACHE_SLOTS) == 0) {
        g_bc_ready = 1;
    }
}

/* The two calls every sector path in this file goes through.
 *
 * A wrapper rather than twenty edited call sites: the point of the phase is
 * that there is one road to the medium, and a wrapper makes that checkable by
 * grep instead of by remembering. */
/* The cache this operation reads through.
 *
 * The current volume's, falling back to the boot device's. A volume that came
 * from the scan carries the cache the scan was using; the boot volume carries
 * this file's own. Naming the boot cache directly, as this did, is what made a
 * second volume impossible - every read went to one device however the caller
 * had mounted. */
static vibeos_blockcache_t *fat_cache(void) {
    if (g_fat_cur && g_fat_cur->cache) {
        return g_fat_cur->cache;
    }
    return g_bc_ready ? &g_bc : 0;
}

static int fat_sector_read(uint64_t lba, void *buf) {
    vibeos_blockcache_t *bc = fat_cache();
    if (!bc) {
        return boot_read(lba, buf);
    }
    return vibeos_blockcache_read(bc, lba, buf);
}

static int fat_sector_write(uint64_t lba, const void *buf) {
    vibeos_blockcache_t *bc = fat_cache();
    if (!bc) {
        return boot_write(lba, buf);
    }
    /* Through the cache, so a subsequent read sees it, and then straight out.
     * See the write-through note above. */
    if (vibeos_blockcache_write(bc, lba, buf) != 0) {
        return -1;
    }
    return vibeos_blockcache_flush(bc);
}

/* The one cache, handed out so the volume scan reads through it rather than
 * standing up a second one. Two caches over one device is the arrangement I2
 * spent its whole phase removing. Null before the volume is mounted. */
vibeos_blockcache_t *vibeos_fat_cache(void) {
    return g_bc_ready ? &g_bc : 0;
}

void vibeos_fat_cache_stats(uint64_t *hits, uint64_t *misses,
                                   uint64_t *evictions, uint64_t *evict_failed) {
    if (hits) {
        *hits = g_bc_ready ? g_bc.hits : 0ull;
    }
    if (misses) {
        *misses = g_bc_ready ? g_bc.misses : 0ull;
    }
    if (evictions) {
        *evictions = g_bc_ready ? g_bc.evictions : 0ull;
    }
    if (evict_failed) {
        *evict_failed = g_bc_ready ? g_bc.evict_failed : 0ull;
    }
}

#define SECTOR_SIZE 512u


static uint8_t g_secbuf[SECTOR_SIZE] __attribute__((aligned(16)));

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The one-sector FAT cache that used to live here is gone (I2).
 *
 * It did a real job - a chain walk re-reads the same table sector for every
 * cluster, so a two-megabyte file cost about a thousand round trips returning
 * identical bytes - and that job now belongs to kernel/fs/blockcache.c, which
 * does it with sixty-four slots instead of one and is host-tested.
 *
 * Leaving it in place was worse than either arrangement, and the measurement
 * says how much: with both caches present the block cache saw a 7% hit rate on
 * twenty-four consecutive boots, because this one absorbed every repeat before
 * it got there and passed on only the misses. Two caches in series where the
 * first is a hundredth the size of the second is not belt and braces, it is
 * the small one deciding what the large one is allowed to see.
 *
 * That is the whole point of the phase: one cache, not three. */

/* Sticky "the chain walk went wrong" flag.
 *
 * fat_next_cluster() has to return a cluster number, so it reports a failed
 * FAT read as the end-of-chain marker - which is exactly what a healthy last
 * cluster returns. A reader cannot tell the two apart, so a single failed FAT
 * sector read looked like a complete, shorter file. Callers clear this before
 * a walk and check it after. */
static int g_fat_chain_error;

/* Kept as a name because callers say it after a write, but the block cache is
 * coherent by construction - a write goes through it, so a later read sees it -
 * and there is no separate copy left to drop. */
static void fat_cache_drop(void) {
}

/* One sector of the table, from the one cache.
 *
 * The buffer is static and the pointer is handed back to the caller, which is
 * only safe because every caller uses it before calling this again. That was
 * true of the cache this replaced and is true now; it is stated because the
 * lifetime is no longer obvious from the code. */
static uint8_t g_tablesec[SECTOR_SIZE];

static const uint8_t *fat_table_sector(uint32_t lba) {
    if (fat_sector_read(lba, g_tablesec) != 0) {
        g_fat_chain_error = 1;
        return 0;
    }
    return g_tablesec;
}

/* Mount `vol` from `bc`, at `at_lba`.
 *
 * `at_lba` of zero means "find it yourself", which is what the boot volume
 * does: parse the MBR and take the first non-empty partition. A caller that
 * already knows - the volume scan does, it read the table - says so, and that
 * is the difference between a driver that can mount one partition and one that
 * can mount the partition it was asked about.
 *
 * `bc` of null means the boot device's own cache, so the boot path is
 * unchanged.
 */
static int fat_mount_on(fat_fs_t *vol, vibeos_blockcache_t *bc,
                        uint32_t at_lba) {
    uint32_t part_lba = at_lba;
    uint16_t reserved, bytes_per_sec;
    uint32_t total_sectors, root_dir_sectors;

    g_fat_cur = vol;
    g_fat_cur->mounted = 0;
    g_fat_cur->cache = bc;

    /* Before the first read below, and invalidating on a remount: a cache that
     * outlived a mount would answer for a volume that is no longer there.
     * Only for the boot device - a caller that brought its own cache owns it. */
    if (!bc) {
        fat_cache_bringup();
    }

    if (part_lba == 0u) {
        /* MBR: the first non-empty partition; fall back to a bare
         * superfloppy. */
        if (fat_sector_read(0, g_secbuf) != 0) {
            return -1;
        }
        if (rd16(&g_secbuf[510]) != 0xAA55u) {
            return -1;
        }
        {
            const uint8_t *pe = &g_secbuf[446];
            if (pe[4] != 0 && rd32(&pe[8]) != 0) {
                part_lba = rd32(&pe[8]);
            }
        }
    }

    /* Read the volume boot record / BPB. */
    if (fat_sector_read(part_lba, g_secbuf) != 0) {
        return -1;
    }
    bytes_per_sec = rd16(&g_secbuf[11]);
    if (bytes_per_sec != SECTOR_SIZE) {
        return -1;
    }
    g_fat_cur->part_lba = part_lba;
    g_fat_cur->sectors_per_cluster = g_secbuf[13];
    reserved = rd16(&g_secbuf[14]);
    g_fat_cur->root_entries = rd16(&g_secbuf[17]);
    g_fat_cur->sectors_per_fat = rd16(&g_secbuf[22]);
    if (g_fat_cur->sectors_per_fat == 0) {
        g_fat_cur->sectors_per_fat = rd32(&g_secbuf[36]);      /* FAT32 */
        g_fat_cur->root_cluster = rd32(&g_secbuf[44]);
        g_fat_cur->is_fat32 = 1;
    } else {
        g_fat_cur->is_fat32 = 0;
    }
    if (g_fat_cur->sectors_per_cluster == 0 || g_fat_cur->sectors_per_fat == 0) {
        return -1;
    }

    total_sectors = rd16(&g_secbuf[19]);
    if (total_sectors == 0) {
        total_sectors = rd32(&g_secbuf[32]);
    }
    root_dir_sectors = ((uint32_t)g_fat_cur->root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
    g_fat_cur->fat_lba = part_lba + reserved;
    g_fat_cur->root_lba = g_fat_cur->fat_lba + 2u * g_fat_cur->sectors_per_fat;         /* FAT16 root */
    g_fat_cur->data_lba = g_fat_cur->root_lba + root_dir_sectors;                  /* first data */
    if (total_sectors <= g_fat_cur->data_lba - part_lba) {
        return -1;
    }
    g_fat_cur->part_sectors = total_sectors;
    g_fat_cur->max_clusters = (total_sectors - (g_fat_cur->data_lba - part_lba)) /
                         g_fat_cur->sectors_per_cluster;
    if (g_fat_cur->max_clusters == 0u) {
        return -1;
    }

    fat_cache_drop();
    g_fat_cur->mounted = 1;
    if (g_mount_hook) {
        g_mount_hook(g_fat_cur->is_fat32, part_lba, g_fat_cur->data_lba);
    }
    return 0;
}

static uint32_t fat_cluster_lba(uint32_t cluster) {
    vibeos_fat_geometry_t g;
    uint32_t lba = 0;

    g.data_lba = g_fat_cur->data_lba;
    g.sectors_per_cluster = g_fat_cur->sectors_per_cluster;
    g.max_clusters = g_fat_cur->max_clusters;
    g.part_lba = g_fat_cur->part_lba;
    g.part_sectors = g_fat_cur->part_sectors;
    /* 0 for a cluster this volume does not have (H-012). Sector 0 cannot be a
     * data sector - the reserved sectors come first - so every caller can test
     * for it. The chain error is set as well, for the paths that already
     * report through it. */
    if (vibeos_fat_cluster_sector(&g, cluster, &lba) != 0) {
        /* Must be zero: a cluster number the volume does not have came out of
         * a directory entry or a table, which is a corrupt volume or a driver
         * that computed one wrongly. Counted beside fat_chain.c's, which
         * watches the same thing on the bulk-read path. */
        vibeos_mbz_hit(VIBEOS_MBZ_FAT_CHAIN_BAD, cluster);
        g_fat_chain_error = 1;
        return 0;
    }
    return lba;
}

/* Follow the FAT chain: return the next cluster, or >= EOC when the chain ends.
 *
 * The entry's sector is checked against the size of the table. Without that,
 * a cluster number larger than the volume holds reads whatever sector follows
 * the FAT - the root directory or file data - and hands back those bytes as
 * the next cluster, which sends the reader off to an arbitrary sector or past
 * the end of the device. fat_get_entry() has always made this check; the read
 * path did not. */
static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t per_sec = g_fat_cur->is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t eoc = g_fat_cur->is_fat32 ? 0x0FFFFFFFu : 0xFFFFu;
    uint32_t sec_index = cluster / per_sec;
    const uint8_t *sec;

    if (cluster < 2u || cluster - 2u >= g_fat_cur->max_clusters ||
        sec_index >= g_fat_cur->sectors_per_fat) {
        g_fat_chain_error = 1;
        return eoc;
    }
    sec = fat_table_sector(g_fat_cur->fat_lba + sec_index);
    if (!sec) {
        return eoc;
    }
    if (g_fat_cur->is_fat32) {
        return rd32(&sec[(cluster % per_sec) * 4u]) & 0x0FFFFFFFu;
    }
    return rd16(&sec[(cluster % per_sec) * 2u]);
}

static int fat_chain_end(uint32_t cluster) {
    return g_fat_cur->is_fat32 ? (cluster >= 0x0FFFFFF8u) : (cluster >= 0xFFF8u);
}

/* Build the 11-byte 8.3 on-disk name from "NAME.EXT".
 *
 * Folded to upper case, because that is the only case a short FAT name has on
 * disk. Without the fold, lookups are accidentally case-sensitive against a
 * filesystem that is not, so a program asking for "cat" cannot find CAT - and
 * programs do ask in lower case, since that is what Unix paths look like. */
static uint8_t fat_upper(uint8_t c) {
    return (c >= 'a' && c <= 'z') ? (uint8_t)(c - 'a' + 'A') : c;
}

static void fat_make_83(const char *name, uint8_t out[11]) {
    int i = 0, o = 0;
    for (o = 0; o < 11; o++) {
        out[o] = ' ';
    }
    for (o = 0; o < 8 && name[i] && name[i] != '.'; i++, o++) {
        out[o] = fat_upper((uint8_t)name[i]);
    }
    while (name[i] && name[i] != '.') {
        i++;
    }
    if (name[i] == '.') {
        i++;
    }
    for (o = 8; o < 11 && name[i]; i++, o++) {
        out[o] = fat_upper((uint8_t)name[i]);
    }
    return;
}

/* Whether a name has an exact 8.3 form: at most eight characters, at most one
 * dot, at most three after it. "." and ".." are directory entries of their own. */
static int fat_fits_83(const char *name) {
    int base = 0, ext = 0, dots = 0;
    const char *p;

    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
        return 1;
    }
    for (p = name; *p; p++) {
        if (*p == '.') {
            if (++dots > 1 || base == 0) {
                return 0;
            }
        } else if (dots) {
            ext++;
        } else {
            base++;
        }
    }
    return base >= 1 && base <= 8 && ext <= 3;
}

/* ---- long names (VFAT) ------------------------------------------------------
 *
 * A name that is not 8.3 - ld-musl-x86_64.so.1, the name a dynamic program asks
 * for its interpreter by - is stored as a run of long-name entries in front of
 * the short entry it belongs to: attribute 0x0F, 13 UCS-2 characters each,
 * last piece first, every piece carrying a checksum of the short name. This
 * driver used to skip them, so a volume could hold such a file and no path
 * could reach it; the kernel substituted an 8.3 name for the interpreter
 * instead (docs/abi/ A4 deletes that).
 *
 * Read only. A file this driver creates still gets an 8.3 name, which is what
 * the writer has always done.
 *
 * The run is accumulated across sectors and clusters, because a directory scan
 * reads them one at a time and nothing stops a run from straddling the edge. It
 * is believed only if every piece arrived in order with the same checksum and
 * that checksum matches the short entry that follows: a run left behind by a
 * tool that renamed the short name without it - an older DOS, a careless
 * formatter - is not this file's name, and matching it would open the wrong
 * file under the right name. Characters outside ASCII cannot be spelled by a
 * path this kernel accepts, so they make the name unmatchable, not mismatched. */
#define FAT_LFN_MAX 255u

typedef struct {
    char name[FAT_LFN_MAX + 1u];
    uint8_t sum;
    uint8_t expect;   /* the sequence number the next piece must carry */
    uint8_t valid;
} fat_lfn_t;

static uint8_t fat_short_sum(const uint8_t *d) {
    uint8_t sum = 0;
    int i;
    for (i = 0; i < 11; i++) {
        sum = (uint8_t)(((sum & 1u) ? 0x80u : 0u) + (sum >> 1) + d[i]);
    }
    return sum;
}

static void fat_lfn_piece(fat_lfn_t *l, const uint8_t *d) {
    static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    uint32_t seq = d[0] & 0x1Fu, base, k;

    if (d[0] & 0x40u) {                 /* the last piece, which comes first */
        uint32_t i;
        for (i = 0; i <= FAT_LFN_MAX; i++) {
            l->name[i] = 0;
        }
        l->valid = (seq >= 1u && seq <= 20u);
        l->sum = d[13];
        l->expect = (uint8_t)seq;
    } else if (!l->valid || seq != (uint32_t)l->expect - 1u || d[13] != l->sum) {
        l->valid = 0;
        return;
    } else {
        l->expect = (uint8_t)seq;
    }
    if (!l->valid) {
        return;
    }
    base = (seq - 1u) * 13u;
    for (k = 0; k < 13u; k++) {
        uint16_t c = rd16(&d[at[k]]);
        if (c == 0x0000u || c == 0xFFFFu) {
            break;
        }
        if (base + k >= FAT_LFN_MAX) {
            l->valid = 0;
            return;
        }
        /* Not ASCII: no path here can spell it. 0x01 is a byte no path holds. */
        l->name[base + k] = (c < 0x80u) ? (char)c : (char)0x01;
    }
}

/* The long name for the short entry `d`, or 0 when it has none (or the run did
 * not belong to it). Consumes the run either way. */
static const char *fat_lfn_take(fat_lfn_t *l, const uint8_t *d) {
    int ok = l->valid && l->expect == 1u && l->sum == fat_short_sum(d) && l->name[0];
    l->valid = 0;
    return ok ? l->name : 0;
}

/* Long names compare as FAT does: case-insensitively, preserving the case they
 * were written in. */
static int fat_lfn_equal(const char *a, const char *b) {
    while (*a && *b) {
        if (fat_upper((uint8_t)*a) != fat_upper((uint8_t)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == 0 && *b == 0;
}

/* Scan a run of directory sectors for a name - its long name if it has one, its
 * 8.3 name either way; report cluster/size/attr. */
static int fat_scan_sectors(uint32_t lba, uint32_t sectors, const char *comp,
                            const uint8_t want[11], fat_lfn_t *lfn,
                            uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr) {
    uint32_t s, e;
    for (s = 0; s < sectors; s++) {
        if (fat_sector_read(lba + s, g_secbuf) != 0) {
            return -1;
        }
        for (e = 0; e < SECTOR_SIZE; e += 32u) {
            const uint8_t *d = &g_secbuf[e];
            const char *longname;
            int k, match = 1;
            if (d[0] == 0x00) {
                return -1; /* end of directory */
            }
            if (d[0] == 0xE5) {
                lfn->valid = 0;
                continue;  /* deleted */
            }
            if ((d[11] & 0x3Fu) == 0x0Fu) {
                fat_lfn_piece(lfn, d);
                continue;
            }
            if (d[11] & 0x08u) {
                lfn->valid = 0;
                continue;  /* volume label */
            }
            longname = fat_lfn_take(lfn, d);
            for (k = 0; k < 11; k++) {
                if (d[k] != want[k]) {
                    match = 0;
                    break;
                }
            }
            if (match || (longname && fat_lfn_equal(longname, comp))) {
                *out_cluster = ((uint32_t)rd16(&d[20]) << 16) | rd16(&d[26]);
                *out_size = rd32(&d[28]);
                *out_attr = d[11];
                return 0;
            }
        }
    }
    return -1;
}

/* Find an 8.3 name inside a directory. dir_cluster==0 means the root (a fixed
 * region on FAT16, the root cluster chain on FAT32); otherwise a cluster chain. */
static int fat_dir_find(uint32_t dir_cluster, const char *comp,
                        uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr) {
    static fat_lfn_t lfn;   /* under the driver's lock, like g_secbuf */
    uint8_t want[11];
    uint32_t cl;
    uint32_t steps = 0;

    /* An 8.3 conversion of a long component is not that component's short name
     * - it would match whatever file happens to own the truncation - so a name
     * that does not fit 8.3 is looked up by its long name only. */
    fat_make_83(comp, want);
    if (!fat_fits_83(comp)) {
        want[0] = 0xE5;   /* no live entry starts with it */
    }
    lfn.valid = 0;

    if (dir_cluster == 0 && !g_fat_cur->is_fat32) {
        uint32_t root_sectors = ((uint32_t)g_fat_cur->root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
        return fat_scan_sectors(g_fat_cur->root_lba, root_sectors, comp, want, &lfn,
                                out_cluster, out_size, out_attr);
    }
    cl = (dir_cluster == 0) ? g_fat_cur->root_cluster : dir_cluster;
    while (!fat_chain_end(cl) && cl >= 2u && cl - 2u < g_fat_cur->max_clusters &&
           steps++ < g_fat_cur->max_clusters) {
        if (fat_scan_sectors(fat_cluster_lba(cl), g_fat_cur->sectors_per_cluster,
                             comp, want, &lfn, out_cluster, out_size, out_attr) == 0) {
            return 0;
        }
        cl = fat_next_cluster(cl);
    }
    if (cl >= 2u && !fat_chain_end(cl)) {
        g_fat_chain_error = 1;
    }
    return -1;
}

/* Resolve a '/'- or '\\'-separated path from the root; the last component is a
 * file, intermediate ones must be directories. */
static int fat_resolve(const char *path, uint32_t *out_cluster, uint32_t *out_size) {
    uint32_t dir = 0; /* root */
    const char *p = path;

    while (*p == '/' || *p == '\\') {
        p++;
    }
    while (*p) {
        static char comp[FAT_LFN_MAX + 1u];   /* under the driver's lock */
        uint8_t attr = 0;
        uint32_t cl = 0, size = 0;
        uint32_t n = 0;
        int last;

        while (*p && *p != '/' && *p != '\\') {
            if (n >= FAT_LFN_MAX) {
                return -1;   /* longer than any FAT name */
            }
            comp[n++] = *p++;
        }
        comp[n] = 0;
        while (*p == '/' || *p == '\\') {
            p++;
        }
        last = (*p == 0);

        if (fat_dir_find(dir, comp, &cl, &size, &attr) != 0) {
            return -1;
        }
        if (last) {
            *out_cluster = cl;
            *out_size = size;
            return 0;
        }
        if ((attr & 0x10u) == 0) {
            return -1; /* not a directory */
        }
        dir = cl;
    }
    return -1;
}

/* ---- public API: open / positional read / list / write ------------------- */

/* Resolve a path to its first cluster and size (a file "open"). */
static int fat_open_locked(const char *path, uint32_t *out_cluster, uint32_t *out_size) {
    if (!g_fat_cur->mounted || !out_cluster || !out_size) {
        return -1;
    }
    return fat_resolve(path, out_cluster, out_size);
}

/* Read `len` bytes at byte offset `off` from a file given by its first cluster
 * and size. Returns bytes read (0 at EOF). */
static long fat_read_at_locked(uint32_t first_cluster, uint32_t size, uint32_t off,
                               void *buf, uint32_t len) {
    uint32_t cluster_bytes, cluster, skip, done = 0;
    uint8_t *out = (uint8_t *)buf;

    if (!g_fat_cur->mounted || !buf) {
        return -1;
    }
    if (off >= size) {
        return 0;
    }
    if (len > size - off) {
        len = size - off;
    }
    cluster_bytes = (uint32_t)g_fat_cur->sectors_per_cluster * SECTOR_SIZE;
    cluster = first_cluster;
    g_fat_chain_error = 0;
    for (skip = off / cluster_bytes; skip > 0 && !fat_chain_end(cluster); skip--) {
        cluster = fat_next_cluster(cluster);
    }
    off %= cluster_bytes;

    while (done < len && !fat_chain_end(cluster) && cluster >= 2u) {
        uint32_t s;
        for (s = off / SECTOR_SIZE; s < g_fat_cur->sectors_per_cluster && done < len; s++) {
            uint32_t in_sec = off % SECTOR_SIZE;
            uint32_t n = SECTOR_SIZE - in_sec;
            uint32_t i;
            uint32_t clba = fat_cluster_lba(cluster);
            if (clba == 0u || fat_sector_read(clba + s, g_secbuf) != 0) {
                return -1;
            }
            if (n > len - done) {
                n = len - done;
            }
            for (i = 0; i < n; i++) {
                out[done + i] = g_secbuf[in_sec + i];
            }
            done += n;
            off += n;
        }
        off = 0;
        cluster = fat_next_cluster(cluster);
    }
    /* A broken chain must not look like a short read at end of file: read(2)
     * would return the truncated count and the program would believe it. */
    if (g_fat_chain_error) {
        return -1;
    }
    return (long)done;
}

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu); p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Separate buffer for FAT-table I/O: directory scanning holds g_secbuf. */
static uint8_t g_fatbuf[SECTOR_SIZE] __attribute__((aligned(16)));

/* Write one FAT entry to every FAT copy. */
static int fat_set_entry(uint32_t cluster, uint32_t value) {
    uint32_t per_sec = g_fat_cur->is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t s = cluster / per_sec, i = cluster % per_sec, copy;

    if (s >= g_fat_cur->sectors_per_fat) {
        return -1;
    }
    if (fat_sector_read(g_fat_cur->fat_lba + s, g_fatbuf) != 0) {
        return -1;
    }
    fat_cache_drop();
    if (g_fat_cur->is_fat32) {
        wr32(&g_fatbuf[i * 4u], value & 0x0FFFFFFFu);
    } else {
        wr16(&g_fatbuf[i * 2u], (uint16_t)value);
    }
    for (copy = 0; copy < 2u; copy++) {
        if (fat_sector_write(g_fat_cur->fat_lba + copy * g_fat_cur->sectors_per_fat + s,
                                           g_fatbuf) != 0) {
            return -1;
        }
    }
    return 0;
}

static uint32_t fat_get_entry(uint32_t cluster) {
    uint32_t per_sec = g_fat_cur->is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t s = cluster / per_sec, i = cluster % per_sec;

    if (s >= g_fat_cur->sectors_per_fat ||
        fat_sector_read(g_fat_cur->fat_lba + s, g_fatbuf) != 0) {
        return g_fat_cur->is_fat32 ? 0x0FFFFFFFu : 0xFFFFu;
    }
    return g_fat_cur->is_fat32 ? (rd32(&g_fatbuf[i * 4u]) & 0x0FFFFFFFu) : rd16(&g_fatbuf[i * 2u]);
}

/* Release a whole cluster chain back to the free pool. */
static void fat_free_chain(uint32_t cluster) {
    while (cluster >= 2u && !fat_chain_end(cluster)) {
        uint32_t next = fat_get_entry(cluster);
        if (fat_set_entry(cluster, 0) != 0) {
            return;
        }
        cluster = next;
    }
}

/* Allocate `count` clusters and link them into one chain; 0 on failure. */
static uint32_t fat_alloc_chain(uint32_t count) {
    uint32_t per_sec = g_fat_cur->is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t total = g_fat_cur->sectors_per_fat * per_sec;
    uint32_t first = 0, prev = 0, cl, got = 0, looked = 0;
    uint32_t eoc = g_fat_cur->is_fat32 ? 0x0FFFFFFFu : 0xFFFFu;

    /* The table ends where the volume's clusters do, not where its last sector
     * does: an entry past them is not a cluster anybody can use. */
    if (total > g_fat_cur->max_clusters + 2u) {
        total = g_fat_cur->max_clusters + 2u;
    }
    /* From where the last search ended, and round once. It started at cluster
     * 2 every time, which was nothing when a boot wrote thirty sectors and is
     * a walk of the whole table per write now that files grow a cluster at a
     * time (docs/abi/ L1 step 4). */
    cl = g_fat_cur->alloc_hint;
    if (cl < 2u || cl >= total) {
        cl = 2u;
    }
    while (got < count && looked < total) {
        if (cl >= total) {
            cl = 2u;
        }
        looked++;
        if (fat_get_entry(cl) != 0u) {
            cl++;
            continue;
        }
        if (fat_set_entry(cl, eoc) != 0) {
            break;
        }
        if (prev != 0 && fat_set_entry(prev, cl) != 0) {
            break;
        }
        if (first == 0) {
            first = cl;
        }
        prev = cl;
        got++;
        cl++;
    }
    if (got < count) {
        if (first != 0) {
            fat_free_chain(first);
        }
        return 0;
    }
    g_fat_cur->alloc_hint = cl;
    return first;
}

/* ==== entries, names and writing in place (docs/abi/ L1 step 4) =====================
 *
 * Everything above identifies a file by its first cluster and writes one by
 * replacing it whole. Neither survives a real write path:
 *
 * - An empty file has no first cluster, so every empty file had the same
 *   identity, zero; and a write has to update the size in the directory entry,
 *   which a cluster number does not find. A file is identified here by where
 *   its directory entry is.
 * - A name was turned into 8.3 by cutting it, so "sedAbCdEf" - the nine
 *   characters sed -i calls its temporary file - was created as SEDABCDE, and
 *   the rename that followed could not find it. Names that are not 8.3 get the
 *   long-name entries the reader has understood since A4, and a short alias.
 * - A directory that was full could not take another name. It grows.
 *
 * What a FAT volume cannot hold stays refused: owners, permission bits beyond
 * read-only, links of either kind. The wrappers answer EPERM for those.
 *
 * Known gap, the same shape as tmpfs's: an identity is a directory slot, so a
 * description left open across an unlink is refused (the slot says deleted) -
 * until another name takes the slot, when it would reach that file. And a
 * rename moves the entry, so a description open on the old name is refused. */

#include "vibeos/abi_linux.h"

#define FAT_RUN_MAX 21u   /* twenty long-name entries and the short one */

typedef struct {
    uint32_t lba;
    uint32_t off;
} fat_slot_t;

typedef struct {
    fat_slot_t at;                 /* the short entry                       */
    fat_slot_t run[FAT_RUN_MAX];   /* every entry of its name, in order     */
    uint32_t run_len;
    uint32_t cluster;
    uint32_t size;
    uint32_t dir;                  /* the directory it is in; 0 is the root */
    uint16_t date;
    uint16_t time;
    uint8_t attr;
    uint8_t ntres;                 /* which parts of an 8.3 name are lower case */
} fat_ent_t;

/* ---- walking a directory, a sector at a time -------------------------------------- */

typedef struct {
    uint32_t cl;          /* 0: the FAT16 root's fixed region */
    uint32_t s;           /* sector within the cluster, or within the region */
    uint32_t limit;
    uint32_t steps;
    uint32_t lba;
} fat_diter_t;

static int diter_load(fat_diter_t *it) {
    if (it->cl == 0u) {
        it->lba = g_fat_cur->root_lba + it->s;
        return 0;
    }
    it->lba = fat_cluster_lba(it->cl);
    if (it->lba == 0u) {
        return -1;
    }
    it->lba += it->s;
    return 0;
}

static int diter_first(fat_diter_t *it, uint32_t dir) {
    it->s = 0;
    it->steps = 0;
    if (dir == 0u && !g_fat_cur->is_fat32) {
        it->cl = 0;
        it->limit = ((uint32_t)g_fat_cur->root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
        return it->limit ? diter_load(it) : -1;
    }
    it->cl = (dir == 0u) ? g_fat_cur->root_cluster : dir;
    it->limit = g_fat_cur->sectors_per_cluster;
    if (it->cl < 2u) {
        return -1;
    }
    return diter_load(it);
}

/* The next sector, or -1 at the end of the directory. */
static int diter_next(fat_diter_t *it) {
    if (++it->s < it->limit) {
        return diter_load(it);
    }
    if (it->cl == 0u) {
        return -1;
    }
    it->cl = fat_get_entry(it->cl);
    if (it->cl < 2u || fat_chain_end(it->cl) || ++it->steps > g_fat_cur->max_clusters) {
        return -1;
    }
    it->s = 0;
    return diter_load(it);
}

static void ent_from(fat_ent_t *e, const uint8_t *d, uint32_t lba, uint32_t off, uint32_t dir) {
    e->at.lba = lba;
    e->at.off = off;
    e->cluster = ((uint32_t)rd16(&d[20]) << 16) | rd16(&d[26]);
    e->size = rd32(&d[28]);
    e->attr = d[11];
    e->ntres = d[12];
    e->time = rd16(&d[22]);
    e->date = rd16(&d[24]);
    e->dir = dir;
}

/* The entry named `name` in `dir`: by its long name, or by its 8.3 name. 0 and
 * `out`, or -ENOENT. */
static int fat_find(uint32_t dir, const char *name, fat_ent_t *out) {
    static fat_lfn_t lfn;   /* under the driver's lock, like the buffers */
    fat_slot_t run[FAT_RUN_MAX];
    uint32_t run_len = 0;
    uint8_t want[11];
    fat_diter_t it;
    uint32_t e;
    int r;

    fat_make_83(name, want);
    if (!fat_fits_83(name)) {
        want[0] = 0xE5;   /* no live entry starts with it */
    }
    if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) {
        /* "." and "..": dots that are the name, not the separator before an
         * extension, which is how the 8.3 conversion reads any other dot. */
        uint32_t k;
        for (k = 0; k < 11u; k++) {
            want[k] = ' ';
        }
        want[0] = '.';
        if (name[1] == '.') {
            want[1] = '.';
        }
    }
    lfn.valid = 0;
    for (r = diter_first(&it, dir); r == 0; r = diter_next(&it)) {
        if (fat_sector_read(it.lba, g_secbuf) != 0) {
            return -VIBEOS_EIO;
        }
        for (e = 0; e < SECTOR_SIZE; e += 32u) {
            const uint8_t *d = &g_secbuf[e];
            const char *longname;
            int k, match = 1;

            if (d[0] == 0x00) {
                return -VIBEOS_ENOENT;
            }
            if (d[0] == 0xE5) {
                lfn.valid = 0;
                run_len = 0;
                continue;
            }
            if ((d[11] & 0x3Fu) == 0x0Fu) {
                if (d[0] & 0x40u) {
                    run_len = 0;   /* a new run begins at its last piece */
                }
                if (run_len + 1u < FAT_RUN_MAX) {
                    run[run_len].lba = it.lba;
                    run[run_len].off = e;
                    run_len++;
                }
                fat_lfn_piece(&lfn, d);
                continue;
            }
            if (d[11] & 0x08u) {
                lfn.valid = 0;
                run_len = 0;
                continue;
            }
            longname = fat_lfn_take(&lfn, d);
            for (k = 0; k < 11; k++) {
                if (d[k] != want[k]) {
                    match = 0;
                    break;
                }
            }
            if (match || (longname && fat_lfn_equal(longname, name))) {
                uint32_t i;
                ent_from(out, d, it.lba, e, dir);
                /* The long-name entries are part of the name only if they were
                 * this entry's: an orphaned run in front of it stays where it is. */
                out->run_len = 0;
                if (longname) {
                    for (i = 0; i < run_len; i++) {
                        out->run[out->run_len++] = run[i];
                    }
                }
                out->run[out->run_len++] = out->at;
                return 0;
            }
            run_len = 0;
        }
    }
    return -VIBEOS_ENOENT;
}

/* The next component of *p, into `comp`. Its length, 0 at the end, or a
 * negated errno for one no FAT name can be. */
static int fat_comp(const char **p, char *comp) {
    const char *s = *p;
    uint32_t n = 0;

    while (*s == '/' || *s == '\\') {
        s++;
    }
    while (*s && *s != '/' && *s != '\\') {
        if (n >= FAT_LFN_MAX) {
            return -VIBEOS_ENAMETOOLONG;
        }
        comp[n++] = *s++;
    }
    comp[n] = 0;
    *p = s;
    return (int)n;
}

/* A path to its entry. The root has none: *is_root, and the entry untouched. */
static int fat_path_ent(const char *path, fat_ent_t *out, int *is_root) {
    static char comp[FAT_LFN_MAX + 1u];   /* under the driver's lock */
    uint32_t dir = 0;
    int n, r, seen = 0;

    *is_root = 0;
    for (;;) {
        n = fat_comp(&path, comp);
        if (n < 0) {
            return n;
        }
        if (n == 0) {
            break;
        }
        if (seen && (out->attr & 0x10u) == 0u) {
            return -VIBEOS_ENOTDIR;
        }
        if (n == 1 && comp[0] == '.') {
            continue;   /* the directory itself; the root has no "." entry */
        }
        r = fat_find(dir, comp, out);
        if (r != 0) {
            return r;
        }
        seen = 1;
        dir = out->cluster;
    }
    if (!seen) {
        *is_root = 1;
    }
    return 0;
}

/* The directory a name goes in, and the name. -EEXIST for the root itself. */
static int fat_path_parent(const char *path, uint32_t *dir, char *name) {
    static char comp[FAT_LFN_MAX + 1u];
    fat_ent_t e;
    uint32_t cur = 0;
    int n, r, have = 0;

    for (;;) {
        const char *probe = path;
        n = fat_comp(&probe, comp);
        if (n < 0) {
            return n;
        }
        if (n == 0) {
            break;
        }
        if (have) {
            /* `name` holds the component before this one: it is a directory
             * on the way, not the last name. */
            r = fat_find(cur, name, &e);
            if (r != 0) {
                return r;
            }
            if ((e.attr & 0x10u) == 0u) {
                return -VIBEOS_ENOTDIR;
            }
            cur = e.cluster;
        }
        for (r = 0; r <= n; r++) {
            name[r] = comp[r];
        }
        have = 1;
        path = probe;
    }
    if (!have) {
        return -VIBEOS_EEXIST;
    }
    *dir = cur;
    return 0;
}

/* ---- identities -------------------------------------------------------------------- */

static uint64_t fat_ent_id(const fat_ent_t *e) {
    return (((uint64_t)e->at.lba << 4) | (uint64_t)(e->at.off >> 5)) + 1u;
}

/* The entry an identity names, read from the volume as it is now - so a size
 * another description changed is seen. -ENOENT when the slot no longer holds a
 * file: deleted, or never one. */
static int fat_ent_load(uint64_t id, fat_ent_t *out) {
    uint32_t lba, off;
    const uint8_t *d;

    if (id == 0u) {
        return -VIBEOS_EISDIR;   /* the root */
    }
    id -= 1u;
    lba = (uint32_t)(id >> 4);
    off = (uint32_t)(id & 0xFu) * 32u;
    if (lba < g_fat_cur->part_lba ||
        (g_fat_cur->part_sectors && lba - g_fat_cur->part_lba >= g_fat_cur->part_sectors)) {
        return -VIBEOS_ENOENT;
    }
    if (fat_sector_read(lba, g_secbuf) != 0) {
        return -VIBEOS_EIO;
    }
    d = &g_secbuf[off];
    if (d[0] == 0x00 || d[0] == 0xE5 || (d[11] & 0x3Fu) == 0x0Fu || (d[11] & 0x08u)) {
        return -VIBEOS_ENOENT;
    }
    ent_from(out, d, lba, off, 0);
    out->run_len = 0;
    return 0;
}

/* Write an entry's cluster, size and time back into its slot. */
static int fat_ent_store(const fat_ent_t *e) {
    uint8_t *d;

    if (fat_sector_read(e->at.lba, g_secbuf) != 0) {
        return -VIBEOS_EIO;
    }
    d = &g_secbuf[e->at.off];
    wr16(&d[20], (uint16_t)(e->cluster >> 16));
    wr16(&d[26], (uint16_t)(e->cluster & 0xFFFFu));
    wr32(&d[28], e->size);
    wr16(&d[22], e->time);
    wr16(&d[24], e->date);
    d[11] = e->attr;
    return fat_sector_write(e->at.lba, g_secbuf) == 0 ? 0 : -VIBEOS_EIO;
}

/* ---- times ------------------------------------------------------------------------- *
 *
 * FAT keeps a date and a two-second time, from 1980. */

static uint64_t fat_time_ns(uint16_t date, uint16_t time) {
    uint32_t y = 1980u + (date >> 9), m = (date >> 5) & 0xFu, d = date & 0x1Fu;
    uint32_t a, yy, mm;
    uint64_t days, secs;

    if (m < 1u || m > 12u || d < 1u) {
        return 0;   /* an entry with no date */
    }
    /* Days since 1970-01-01 (the civil-from-days arithmetic, forwards). */
    a = (14u - m) / 12u;
    yy = y + 4800u - a;
    mm = m + 12u * a - 3u;
    days = d + (153u * mm + 2u) / 5u + 365ull * yy + yy / 4u - yy / 100u + yy / 400u - 32045u;
    days -= 2440588ull;
    secs = days * 86400ull + (uint64_t)(time >> 11) * 3600u + (uint64_t)((time >> 5) & 0x3Fu) * 60u +
           (uint64_t)(time & 0x1Fu) * 2u;
    return secs * 1000000000ull;
}

static void fat_time_from_ns(uint64_t ns, uint16_t *date, uint16_t *time) {
    uint64_t secs = ns / 1000000000ull;
    uint64_t days = secs / 86400ull, rem = secs % 86400ull;
    uint64_t j = days + 2440588ull;
    uint64_t f = j + 1401u + (((4u * j + 274277u) / 146097u) * 3u) / 4u - 38u;
    uint64_t e = 4u * f + 3u, g = (e % 1461u) / 4u, h = 5u * g + 2u;
    uint32_t d = (uint32_t)((h % 153u) / 5u + 1u);
    uint32_t m = (uint32_t)((h / 153u + 2u) % 12u + 1u);
    uint32_t y = (uint32_t)(e / 1461u - 4716u + (14u - m) / 12u);

    if (y < 1980u) {
        /* Before FAT's epoch - which uptime always is, until the kernel has a
         * wall clock: the first day it can say. */
        *date = (uint16_t)((1u << 5) | 1u);
        *time = 0;
        return;
    }
    if (y > 2107u) {
        y = 2107u;
    }
    *date = (uint16_t)(((y - 1980u) << 9) | (m << 5) | d);
    *time = (uint16_t)(((rem / 3600u) << 11) | (((rem % 3600u) / 60u) << 5) | ((rem % 60u) / 2u));
}

/* ---- names ------------------------------------------------------------------------- */

static int fat_short_char(uint8_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80u) {
        return 1;
    }
    switch (c) {
        case '!': case '#': case '$': case '%': case '&': case '\'': case '(': case ')':
        case '-': case '@': case '^': case '_': case '`': case '{': case '}': case '~':
            return 1;
        default:
            return 0;
    }
}

/* Whether `name` can be a FAT name at all. */
static int fat_name_ok(const char *name) {
    uint32_t n = 0;

    for (; name[n]; n++) {
        uint8_t c = (uint8_t)name[n];
        if (c < 0x20u || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' ||
            c == '>' || c == '?' || c == '\\' || c == '|') {
            return 0;
        }
    }
    if (n == 0u || n > FAT_LFN_MAX) {
        return 0;
    }
    if (name[0] == '.' && (n == 1u || (n == 2u && name[1] == '.'))) {
        return 0;
    }
    return name[n - 1u] != ' ' && name[n - 1u] != '.';   /* which FAT cannot store */
}

/* Can `name` be stored as a short entry alone, exactly? Then `out` is its
 * eleven bytes and `ntres` says which parts were lower case - the way every
 * FAT implementation since Windows NT keeps "notes.txt" without a long name. */
static int fat_name_plain(const char *name, uint8_t out[11], uint8_t *ntres) {
    int base_lower = 0, base_upper = 0, ext_lower = 0, ext_upper = 0, in_ext = 0;
    const char *p;

    if (!fat_fits_83(name)) {
        return 0;
    }
    for (p = name; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == '.') {
            in_ext = 1;
            continue;
        }
        if (c >= 'a' && c <= 'z') {
            *(in_ext ? &ext_lower : &base_lower) = 1;
        } else if (c >= 'A' && c <= 'Z') {
            *(in_ext ? &ext_upper : &base_upper) = 1;
        }
        if (!fat_short_char(fat_upper(c))) {
            return 0;
        }
    }
    if ((base_lower && base_upper) || (ext_lower && ext_upper)) {
        return 0;   /* mixed case: only a long name keeps it */
    }
    fat_make_83(name, out);
    *ntres = (uint8_t)((base_lower ? 0x08u : 0u) | (ext_lower ? 0x10u : 0u));
    return 1;
}

static int fat_short_taken(uint32_t dir, const uint8_t want[11]) {
    fat_diter_t it;
    uint32_t e;
    int r;

    for (r = diter_first(&it, dir); r == 0; r = diter_next(&it)) {
        if (fat_sector_read(it.lba, g_secbuf) != 0) {
            return 1;   /* cannot tell: do not hand the name out */
        }
        for (e = 0; e < SECTOR_SIZE; e += 32u) {
            const uint8_t *d = &g_secbuf[e];
            int k, match = 1;
            if (d[0] == 0x00) {
                return 0;
            }
            if (d[0] == 0xE5 || (d[11] & 0x3Fu) == 0x0Fu) {
                continue;
            }
            for (k = 0; k < 11; k++) {
                if (d[k] != want[k]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                return 1;
            }
        }
    }
    return 0;
}

/* The short alias of a long name: what is left of it in upper case, cut to
 * make room for "~n", with the first n no other entry in the directory has. */
static int fat_alias(uint32_t dir, const char *name, uint8_t out[11]) {
    uint8_t base[8], ext[3];
    uint32_t nb = 0, ne = 0, n, i, last_dot = 0, len = 0;
    int have_dot = 0;

    for (len = 0; name[len]; len++) {
        if (name[len] == '.' && len > 0u) {
            last_dot = len;
            have_dot = 1;
        }
    }
    for (i = 0; i < (have_dot ? last_dot : len) && nb < 8u; i++) {
        uint8_t c = fat_upper((uint8_t)name[i]);
        if (fat_short_char(c)) {
            base[nb++] = c;
        }
    }
    for (i = have_dot ? last_dot + 1u : len; i < len && ne < 3u; i++) {
        uint8_t c = fat_upper((uint8_t)name[i]);
        if (fat_short_char(c)) {
            ext[ne++] = c;
        }
    }
    if (nb == 0u) {
        base[nb++] = '_';
    }
    for (n = 1; n < 1000000u; n++) {
        char digits[8];
        uint32_t nd = 0, v = n, keep;

        while (v) {
            digits[nd++] = (char)('0' + v % 10u);
            v /= 10u;
        }
        keep = nb < 7u - nd ? nb : 7u - nd;   /* base, '~', digits: eight in all */
        for (i = 0; i < 11u; i++) {
            out[i] = ' ';
        }
        for (i = 0; i < keep; i++) {
            out[i] = base[i];
        }
        out[keep] = '~';
        for (i = 0; i < nd; i++) {
            out[keep + 1u + i] = (uint8_t)digits[nd - 1u - i];
        }
        for (i = 0; i < ne; i++) {
            out[8u + i] = ext[i];
        }
        if (!fat_short_taken(dir, out)) {
            return 0;
        }
    }
    return -VIBEOS_ENOSPC;
}

/* ---- directory slots --------------------------------------------------------------- */

static int fat_zero_cluster(uint32_t cluster) {
    uint32_t s, i, lba = fat_cluster_lba(cluster);

    if (lba == 0u) {
        return -VIBEOS_EIO;
    }
    for (i = 0; i < SECTOR_SIZE; i++) {
        g_fatbuf[i] = 0;
    }
    for (s = 0; s < g_fat_cur->sectors_per_cluster; s++) {
        if (fat_sector_write(lba + s, g_fatbuf) != 0) {
            return -VIBEOS_EIO;
        }
    }
    return 0;
}

/* `n` consecutive free slots in `dir`, growing the directory by a cluster when
 * it has none - which the FAT16 root, a fixed region, cannot do. The slot
 * after them is reported too when they used up the end-of-directory marker:
 * what follows that marker is not promised to be zero, and the directory must
 * still end somewhere. */
static int fat_dir_reserve(uint32_t dir, uint32_t n, fat_slot_t *slots, fat_slot_t *after,
                           int *need_end) {
    fat_diter_t it;
    uint32_t have = 0, e, last_cl = 0, grown = 0;
    int r, at_end = 0;

    *need_end = 0;
    for (;;) {
        for (r = diter_first(&it, dir); r == 0; r = diter_next(&it)) {
            last_cl = it.cl;
            if (fat_sector_read(it.lba, g_secbuf) != 0) {
                return -VIBEOS_EIO;
            }
            for (e = 0; e < SECTOR_SIZE; e += 32u) {
                const uint8_t *d = &g_secbuf[e];
                if (have == n) {
                    /* The run is complete; this is the slot after it. */
                    if (at_end) {
                        after->lba = it.lba;
                        after->off = e;
                        *need_end = 1;
                    }
                    return 0;
                }
                if (d[0] == 0x00) {
                    at_end = 1;
                }
                if (at_end || d[0] == 0xE5) {
                    slots[have].lba = it.lba;
                    slots[have].off = e;
                    have++;
                } else {
                    have = 0;
                }
            }
        }
        if (have == n) {
            return 0;   /* the run ends exactly where the directory does */
        }
        /* No room: one more cluster, zeroed, on the end of the chain. */
        if (last_cl == 0u || grown++ > 2u) {
            return -VIBEOS_ENOSPC;
        }
        {
            uint32_t fresh = fat_alloc_chain(1);
            if (fresh == 0u) {
                return -VIBEOS_ENOSPC;
            }
            if (fat_zero_cluster(fresh) != 0 || fat_set_entry(last_cl, fresh) != 0) {
                fat_free_chain(fresh);
                return -VIBEOS_EIO;
            }
        }
        have = 0;
        at_end = 0;
    }
}

static int fat_slot_write(const fat_slot_t *s, const uint8_t ent[32]) {
    uint32_t i;

    if (fat_sector_read(s->lba, g_secbuf) != 0) {
        return -VIBEOS_EIO;
    }
    for (i = 0; i < 32u; i++) {
        g_secbuf[s->off + i] = ent[i];
    }
    return fat_sector_write(s->lba, g_secbuf) == 0 ? 0 : -VIBEOS_EIO;
}

/* A new name in `dir`: its long-name entries if it needs them, then the short
 * entry. `out` describes it. */
static int fat_dir_add(uint32_t dir, const char *name, uint8_t attr, uint32_t cluster,
                       uint32_t size, fat_ent_t *out) {
    static const uint8_t at[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
    fat_slot_t slots[FAT_RUN_MAX], after;
    uint8_t short_name[11], ent[32], ntres = 0, sum;
    uint32_t len = 0, pieces = 0, i, k;
    uint16_t date, time;
    int need_end = 0, r;

    if (!fat_name_ok(name)) {
        return -VIBEOS_EINVAL;
    }
    while (name[len]) {
        len++;
    }
    if (!fat_name_plain(name, short_name, &ntres)) {
        pieces = (len + 12u) / 13u;
        r = fat_alias(dir, name, short_name);
        if (r != 0) {
            return r;
        }
    }
    r = fat_dir_reserve(dir, pieces + 1u, slots, &after, &need_end);
    if (r != 0) {
        return r;
    }
    sum = fat_short_sum(short_name);
    /* The long name, last piece first, as every reader expects to meet it. */
    for (i = 0; i < pieces; i++) {
        uint32_t seq = pieces - i, base = (seq - 1u) * 13u;

        for (k = 0; k < 32u; k++) {
            ent[k] = 0;
        }
        ent[0] = (uint8_t)(seq | (i == 0u ? 0x40u : 0u));
        ent[11] = 0x0F;
        ent[13] = sum;
        for (k = 0; k < 13u; k++) {
            uint16_t c = 0xFFFFu;
            if (base + k < len) {
                c = (uint8_t)name[base + k];
            } else if (base + k == len) {
                c = 0x0000u;
            }
            wr16(&ent[at[k]], c);
        }
        r = fat_slot_write(&slots[i], ent);
        if (r != 0) {
            return r;
        }
    }
    fat_time_from_ns(vibeos_fs_now_ns(), &date, &time);
    for (k = 0; k < 32u; k++) {
        ent[k] = 0;
    }
    for (k = 0; k < 11u; k++) {
        ent[k] = short_name[k];
    }
    ent[11] = attr;
    ent[12] = ntres;
    wr16(&ent[14], time);
    wr16(&ent[16], date);
    wr16(&ent[18], date);
    wr16(&ent[20], (uint16_t)(cluster >> 16));
    wr16(&ent[22], time);
    wr16(&ent[24], date);
    wr16(&ent[26], (uint16_t)(cluster & 0xFFFFu));
    wr32(&ent[28], size);
    r = fat_slot_write(&slots[pieces], ent);
    if (r != 0) {
        return r;
    }
    if (need_end) {
        for (k = 0; k < 32u; k++) {
            ent[k] = 0;
        }
        r = fat_slot_write(&after, ent);
        if (r != 0) {
            return r;
        }
    }
    if (out) {
        ent_from(out, ent, slots[pieces].lba, slots[pieces].off, dir);
        /* ent[] was overwritten by the end marker above when there was one. */
        out->cluster = cluster;
        out->size = size;
        out->attr = attr;
        out->ntres = ntres;
        out->date = date;
        out->time = time;
        out->run_len = 0;
        for (i = 0; i <= pieces; i++) {
            out->run[out->run_len++] = slots[i];
        }
    }
    return 0;
}

/* Take a name out of its directory: every entry of it, long pieces included,
 * so nothing is left for a reader to wonder about. The clusters are the
 * caller's to free. */
static int fat_dir_remove(const fat_ent_t *e) {
    uint32_t i;

    for (i = 0; i < e->run_len; i++) {
        if (fat_sector_read(e->run[i].lba, g_secbuf) != 0) {
            return -VIBEOS_EIO;
        }
        g_secbuf[e->run[i].off] = 0xE5;
        if (fat_sector_write(e->run[i].lba, g_secbuf) != 0) {
            return -VIBEOS_EIO;
        }
    }
    return 0;
}

/* Does a directory hold anything but "." and ".."? */
static int fat_dir_empty(uint32_t dir) {
    fat_diter_t it;
    uint32_t e;
    int r;

    for (r = diter_first(&it, dir); r == 0; r = diter_next(&it)) {
        if (fat_sector_read(it.lba, g_secbuf) != 0) {
            return 0;
        }
        for (e = 0; e < SECTOR_SIZE; e += 32u) {
            const uint8_t *d = &g_secbuf[e];
            if (d[0] == 0x00) {
                return 1;
            }
            if (d[0] == 0xE5 || (d[11] & 0x3Fu) == 0x0Fu || (d[11] & 0x08u)) {
                continue;
            }
            if (d[0] == '.' && (d[1] == ' ' || (d[1] == '.' && d[2] == ' '))) {
                continue;
            }
            return 0;
        }
    }
    return 1;
}

/* ---- a file's clusters ------------------------------------------------------------- */

static uint32_t fat_free_clusters(void);   /* below, with statfs, which it is for */

static uint32_t fat_cluster_bytes(void) {
    return (uint32_t)g_fat_cur->sectors_per_cluster * SECTOR_SIZE;
}

/* How many clusters a chain has. A chain that does not end is a corrupt
 * volume: counted, and reported as longer than any file. */
static uint32_t fat_chain_count(uint32_t first) {
    uint32_t n = 0, cl = first;

    while (cl >= 2u && !fat_chain_end(cl)) {
        if (++n > g_fat_cur->max_clusters) {
            vibeos_mbz_hit(VIBEOS_MBZ_FAT_CHAIN_BAD, first);
            return n;
        }
        cl = fat_get_entry(cl);
    }
    return n;
}

static uint32_t fat_chain_at(uint32_t first, uint32_t index) {
    uint32_t cl = first;

    while (index-- > 0u && cl >= 2u && !fat_chain_end(cl)) {
        cl = fat_get_entry(cl);
    }
    return (cl >= 2u && !fat_chain_end(cl)) ? cl : 0u;
}

/* Bytes [off, off + len) of a chain: written from `src`, or zeroed when there
 * is none. A sector not wholly covered is read first. */
static int fat_chain_put(uint32_t first, uint32_t off, const uint8_t *src, uint32_t len) {
    uint32_t cb = fat_cluster_bytes(), done = 0;
    uint32_t cl = fat_chain_at(first, off / cb);

    off %= cb;
    while (done < len) {
        uint32_t lba, s = off / SECTOR_SIZE, in_sec = off % SECTOR_SIZE;
        uint32_t n = SECTOR_SIZE - in_sec, i;

        if (cl == 0u || (lba = fat_cluster_lba(cl)) == 0u) {
            return -VIBEOS_EIO;
        }
        if (n > len - done) {
            n = len - done;
        }
        if (n != SECTOR_SIZE && fat_sector_read(lba + s, g_fatbuf) != 0) {
            return -VIBEOS_EIO;
        }
        for (i = 0; i < n; i++) {
            g_fatbuf[in_sec + i] = src ? src[done + i] : 0u;
        }
        if (fat_sector_write(lba + s, g_fatbuf) != 0) {
            return -VIBEOS_EIO;
        }
        done += n;
        off += n;
        if (off == cb) {
            off = 0;
            cl = fat_get_entry(cl);
            if (cl < 2u || fat_chain_end(cl)) {
                cl = 0;
            }
        }
    }
    return 0;
}

/* Make a file's chain `need` clusters long. The new clusters are not zeroed:
 * whoever moves the size past old bytes zeroes exactly what it exposes. */
static int fat_chain_grow(fat_ent_t *e, uint32_t have, uint32_t need) {
    uint32_t fresh;

    if (need <= have) {
        return 0;
    }
    fresh = fat_alloc_chain(need - have);
    if (fresh == 0u) {
        return -VIBEOS_ENOSPC;
    }
    if (have == 0u) {
        e->cluster = fresh;
        return 0;
    }
    if (fat_set_entry(fat_chain_at(e->cluster, have - 1u), fresh) != 0) {
        fat_free_chain(fresh);
        return -VIBEOS_EIO;
    }
    return 0;
}

static void fat_touch(fat_ent_t *e) {
    fat_time_from_ns(vibeos_fs_now_ns(), &e->date, &e->time);
}

/* A file's size becomes `size`: clusters past it go back, and what a larger
 * size exposes reads as zeros - FAT has no holes, so they are written. */
static int fat_resize(fat_ent_t *e, uint32_t size) {
    uint32_t cb = fat_cluster_bytes();
    uint32_t have = (e->cluster >= 2u) ? fat_chain_count(e->cluster) : 0u;
    uint32_t need = (uint32_t)(((uint64_t)size + cb - 1u) / cb);
    int r;

    if (size > e->size) {
        r = fat_chain_grow(e, have, need);
        if (r != 0) {
            return r;
        }
        r = fat_chain_put(e->cluster, e->size, 0, size - e->size);
        if (r != 0) {
            return r;
        }
    } else if (need < have) {
        if (need == 0u) {
            fat_free_chain(e->cluster);
            e->cluster = 0;
        } else {
            uint32_t last = fat_chain_at(e->cluster, need - 1u);
            uint32_t rest = fat_get_entry(last);
            if (fat_set_entry(last, g_fat_cur->is_fat32 ? 0x0FFFFFFFu : 0xFFFFu) != 0) {
                return -VIBEOS_EIO;
            }
            fat_free_chain(rest);
        }
    }
    e->size = size;
    fat_touch(e);
    return fat_ent_store(e);
}

/* Write into a file where it stands. Bytes written, or a negated errno. */
static long fat_write_at(fat_ent_t *e, uint64_t off, const void *buf, uint32_t len) {
    uint32_t cb = fat_cluster_bytes();
    uint32_t have = (e->cluster >= 2u) ? fat_chain_count(e->cluster) : 0u;
    uint64_t end = off + len;
    uint32_t need;
    int r;

    if (len == 0u) {
        return 0;
    }
    if (end > 0xFFFFFFFFull) {
        return -VIBEOS_EFBIG;   /* a FAT file's size is thirty-two bits */
    }
    need = (uint32_t)((end + cb - 1u) / cb);
    if (need > have) {
        r = fat_chain_grow(e, have, need);
        if (r == -VIBEOS_ENOSPC) {
            /* Not room for all of it. The clusters that are left are taken -
             * a short write uses the volume up, it does not stop a few
             * clusters early - and what fits in the file is written; a write
             * that starts past that gets nothing. */
            uint32_t left = fat_free_clusters();
            uint64_t room;
            if (left > 0u && left < need - have && fat_chain_grow(e, have, have + left) == 0) {
                have += left;
            }
            room = (uint64_t)have * cb;
            if (off >= room) {
                return -VIBEOS_ENOSPC;
            }
            len = (uint32_t)(room - off);
            end = room;
        } else if (r != 0) {
            return r;
        }
    }
    /* The gap between the old end and where this write starts was never
     * written: it must read as zeros, not as what the clusters held. */
    if (off > e->size) {
        r = fat_chain_put(e->cluster, e->size, 0, (uint32_t)(off - e->size));
        if (r != 0) {
            return r;
        }
    }
    r = fat_chain_put(e->cluster, (uint32_t)off, (const uint8_t *)buf, len);
    if (r != 0) {
        return r;
    }
    if (end > e->size) {
        e->size = (uint32_t)end;
    }
    fat_touch(e);
    r = fat_ent_store(e);
    return r != 0 ? r : (long)len;
}

/* ---- names: create, remove, move ---------------------------------------------------- */

static int fat_create(const char *path, uint8_t attr, fat_ent_t *out) {
    static char name[FAT_LFN_MAX + 1u];
    fat_ent_t e;
    uint32_t dir = 0, cluster = 0;
    int r = fat_path_parent(path, &dir, name);

    if (r != 0) {
        return r;
    }
    if (fat_find(dir, name, &e) == 0) {
        return -VIBEOS_EEXIST;
    }
    if (attr & 0x10u) {
        /* A directory is one cluster holding "." and "..". */
        uint8_t dot[32];
        fat_slot_t s;
        uint32_t k;
        uint16_t date, time;

        cluster = fat_alloc_chain(1);
        if (cluster == 0u) {
            return -VIBEOS_ENOSPC;
        }
        if (fat_zero_cluster(cluster) != 0) {
            fat_free_chain(cluster);
            return -VIBEOS_EIO;
        }
        fat_time_from_ns(vibeos_fs_now_ns(), &date, &time);
        s.lba = fat_cluster_lba(cluster);
        for (k = 0; k < 2u; k++) {
            uint32_t target = (k == 0u) ? cluster : dir;
            uint32_t i;
            for (i = 0; i < 32u; i++) {
                dot[i] = (i < 11u) ? ' ' : 0u;
            }
            dot[0] = '.';
            if (k == 1u) {
                dot[1] = '.';
            }
            dot[11] = 0x10;
            wr16(&dot[20], (uint16_t)(target >> 16));
            wr16(&dot[26], (uint16_t)(target & 0xFFFFu));
            wr16(&dot[22], time);
            wr16(&dot[24], date);
            s.off = k * 32u;
            if (fat_slot_write(&s, dot) != 0) {
                fat_free_chain(cluster);
                return -VIBEOS_EIO;
            }
        }
    }
    r = fat_dir_add(dir, name, attr, cluster, 0, out);
    if (r != 0 && cluster != 0u) {
        fat_free_chain(cluster);
    }
    return r;
}

/* unlink (want_dir 0) and rmdir (want_dir 1). */
static int fat_remove(const char *path, int want_dir) {
    fat_ent_t e;
    int is_root = 0, r = fat_path_ent(path, &e, &is_root);

    if (r != 0) {
        return r;
    }
    if (is_root) {
        return want_dir ? -VIBEOS_EBUSY : -VIBEOS_EISDIR;
    }
    if (e.attr & 0x10u) {
        if (!want_dir) {
            return -VIBEOS_EISDIR;
        }
        if (!fat_dir_empty(e.cluster)) {
            return -VIBEOS_ENOTEMPTY;
        }
    } else if (want_dir) {
        return -VIBEOS_ENOTDIR;
    }
    r = fat_dir_remove(&e);
    if (r != 0) {
        return r;
    }
    if (e.cluster >= 2u) {
        fat_free_chain(e.cluster);
    }
    return 0;
}

/* The directory holding `dir`, read from its "..". */
static uint32_t fat_dir_parent(uint32_t dir) {
    uint32_t lba = fat_cluster_lba(dir);

    if (lba == 0u || fat_sector_read(lba, g_secbuf) != 0) {
        return 0;
    }
    return ((uint32_t)rd16(&g_secbuf[32u + 20u]) << 16) | rd16(&g_secbuf[32u + 26u]);
}

static int fat_rename(const char *from, const char *to, uint32_t flags) {
    static char fname[FAT_LFN_MAX + 1u], tname[FAT_LFN_MAX + 1u];
    fat_ent_t src, dst, made;
    uint32_t fdir = 0, tdir = 0, x, steps = 0;
    int have_dst, r;

    r = fat_path_parent(from, &fdir, fname);
    if (r == -VIBEOS_EEXIST) {
        r = -VIBEOS_EBUSY;
    }
    if (r != 0) {
        return r;
    }
    r = fat_find(fdir, fname, &src);
    if (r != 0) {
        return r;
    }
    r = fat_path_parent(to, &tdir, tname);
    if (r == -VIBEOS_EEXIST) {
        r = -VIBEOS_EBUSY;
    }
    if (r != 0) {
        return r;
    }
    have_dst = fat_find(tdir, tname, &dst) == 0;
    if (have_dst && (flags & VIBEOS_RENAME_NOREPLACE)) {
        return -VIBEOS_EEXIST;
    }
    if (have_dst && dst.at.lba == src.at.lba && dst.at.off == src.at.off) {
        return 0;   /* onto itself */
    }
    if (src.attr & 0x10u) {
        /* A directory cannot move inside itself: the tree would lose it. */
        for (x = tdir; x != 0u && steps++ < g_fat_cur->max_clusters; x = fat_dir_parent(x)) {
            if (x == src.cluster) {
                return -VIBEOS_EINVAL;
            }
        }
        if (have_dst && (dst.attr & 0x10u) == 0u) {
            return -VIBEOS_ENOTDIR;
        }
        if (have_dst && !fat_dir_empty(dst.cluster)) {
            return -VIBEOS_ENOTEMPTY;
        }
    } else if (have_dst && (dst.attr & 0x10u)) {
        return -VIBEOS_EISDIR;
    }
    /* The new name first: if the directory has no room, nothing has changed.
     * Until the old ones are removed the file has two names, which a check of
     * the volume can repair; the other order could leave it with none. */
    r = fat_dir_add(tdir, tname, src.attr, src.cluster, src.size, &made);
    if (r != 0) {
        return r;
    }
    made.date = src.date;
    made.time = src.time;
    (void)fat_ent_store(&made);
    if (have_dst) {
        r = fat_dir_remove(&dst);
        if (r == 0 && dst.cluster >= 2u) {
            fat_free_chain(dst.cluster);
        }
    }
    if (r == 0) {
        r = fat_dir_remove(&src);
    }
    if (r == 0 && (src.attr & 0x10u) && fdir != tdir) {
        /* The moved directory's ".." names its new parent. */
        uint32_t lba = fat_cluster_lba(src.cluster);
        if (lba == 0u || fat_sector_read(lba, g_secbuf) != 0) {
            return -VIBEOS_EIO;
        }
        wr16(&g_secbuf[32u + 20u], (uint16_t)(tdir >> 16));
        wr16(&g_secbuf[32u + 26u], (uint16_t)(tdir & 0xFFFFu));
        if (fat_sector_write(lba, g_secbuf) != 0) {
            return -VIBEOS_EIO;
        }
    }
    return r;
}

/* Clusters nobody has, counted a table sector at a time. */
static uint32_t fat_free_clusters(void) {
    uint32_t per_sec = g_fat_cur->is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t s, i, free_n = 0;

    for (s = 0; s < g_fat_cur->sectors_per_fat; s++) {
        if (fat_sector_read(g_fat_cur->fat_lba + s, g_fatbuf) != 0) {
            break;
        }
        for (i = 0; i < per_sec; i++) {
            uint32_t cl = s * per_sec + i;
            uint32_t v;
            if (cl < 2u || cl - 2u >= g_fat_cur->max_clusters) {
                continue;
            }
            v = g_fat_cur->is_fat32 ? (rd32(&g_fatbuf[i * 4u]) & 0x0FFFFFFFu)
                                    : rd16(&g_fatbuf[i * 2u]);
            if (v == 0u) {
                free_n++;
            }
        }
    }
    return free_n;
}

/* ---- listing and the whole-file writer, on the entries above ------------------------ */

/* Entry `idx` of the directory at `path` (empty or "/" is the root): its name
 * as it was written - the long name, or the 8.3 name in the case its entry
 * records - its size and whether it is a directory. 0, or -1 past the end.
 *
 * Every cluster of the directory, not only the first: the lister this replaces
 * stopped at one, so a directory that had grown showed its first sixteen or so
 * names and no more. */
static int fat_list_locked(const char *path, uint32_t idx, char *name, uint32_t name_cap,
                           uint32_t *out_size, int *out_is_dir) {
    static fat_lfn_t lfn;   /* under the driver's lock */
    fat_ent_t dir_ent;
    fat_diter_t it;
    uint32_t dir = 0, e, seen = 0;
    int is_root = 0, r;

    if (!g_fat_cur->mounted || !name || name_cap == 0u) {
        return -1;
    }
    if (fat_path_ent(path ? path : "", &dir_ent, &is_root) != 0) {
        return -1;
    }
    if (!is_root) {
        if ((dir_ent.attr & 0x10u) == 0u) {
            return -1;
        }
        dir = dir_ent.cluster;
    }
    lfn.valid = 0;
    for (r = diter_first(&it, dir); r == 0; r = diter_next(&it)) {
        if (fat_sector_read(it.lba, g_secbuf) != 0) {
            return -1;
        }
        for (e = 0; e < SECTOR_SIZE; e += 32u) {
            const uint8_t *d = &g_secbuf[e];
            const char *longname;
            uint32_t k, n = 0;

            if (d[0] == 0x00) {
                return -1;   /* end of directory */
            }
            if (d[0] == 0xE5) {
                lfn.valid = 0;
                continue;
            }
            if ((d[11] & 0x3Fu) == 0x0Fu) {
                fat_lfn_piece(&lfn, d);
                continue;
            }
            if (d[11] & 0x08u) {
                lfn.valid = 0;
                continue;
            }
            longname = fat_lfn_take(&lfn, d);
            if (seen++ != idx) {
                continue;
            }
            if (longname) {
                /* Truncated to the caller's buffer rather than overflowing it:
                 * a lister that asked for sixteen bytes has said how much it
                 * can hold. */
                for (k = 0; longname[k] && n + 1u < name_cap; k++) {
                    name[n++] = longname[k];
                }
            } else {
                /* Byte 12 says which half of an 8.3 name was lower case. */
                for (k = 0; k < 8u && d[k] != ' ' && n + 1u < name_cap; k++) {
                    uint8_t c = d[k];
                    name[n++] = (char)(((d[12] & 0x08u) && c >= 'A' && c <= 'Z') ? c + 32u : c);
                }
                if (d[8] != ' ' && n + 1u < name_cap) {
                    name[n++] = '.';
                    for (k = 8; k < 11u && d[k] != ' ' && n + 1u < name_cap; k++) {
                        uint8_t c = d[k];
                        name[n++] = (char)(((d[12] & 0x10u) && c >= 'A' && c <= 'Z') ? c + 32u : c);
                    }
                }
            }
            name[n] = 0;
            if (out_size) {
                *out_size = rd32(&d[28]);
            }
            if (out_is_dir) {
                *out_is_dir = (d[11] & 0x10u) ? 1 : 0;
            }
            return 0;
        }
    }
    return -1;
}

/* Why the last whole-file write refused, for a caller that prints it. Every
 * failure in that path used to be a bare -1, so a full disk, a missing
 * directory and a medium that would not take a sector arrived as one thing. */
static const char *g_fat_write_why = "-";

const char *vibeos_fat_write_why(void) {
    return g_fat_write_why;
}

/* Create or replace a file with `len` bytes. Bytes written, or a negated errno. */
static long fat_write_whole(const char *path, const void *buf, uint32_t len) {
    fat_ent_t e;
    int is_root = 0, r;
    long w = 0;

    g_fat_write_why = "-";
    if (!g_fat_cur->mounted || (!buf && len)) {
        g_fat_write_why = g_fat_cur->mounted ? "no_buffer" : "not_mounted";
        return -VIBEOS_EINVAL;
    }
    r = fat_path_ent(path, &e, &is_root);
    if (r == -VIBEOS_ENOENT) {
        r = fat_create(path, 0x20, &e);
    } else if (r == 0 && (is_root || (e.attr & 0x10u))) {
        r = -VIBEOS_EISDIR;
    }
    if (r == 0) {
        r = fat_resize(&e, 0);
    }
    if (r == 0 && len) {
        w = fat_write_at(&e, 0, buf, len);
        if (w < 0) {
            r = (int)w;
        } else if ((uint32_t)w != len) {
            r = -VIBEOS_ENOSPC;   /* a whole file, or it did not happen */
        }
    }
    if (r != 0) {
        g_fat_write_why = r == -VIBEOS_ENOSPC ? "no_free_clusters_or_directory_slot"
                        : r == -VIBEOS_EIO ? "medium_refused_a_sector"
                        : r == -VIBEOS_EINVAL ? "name_not_allowed"
                        : r == -VIBEOS_EISDIR ? "is_a_directory"
                        : "parent_directory_not_found";
        return r;
    }
    return (long)len;
}

/* The five primitives vibeos_fat_chain_read() needs from this driver. They
 * exist only so the run arithmetic can live in kernel/fs/fat_chain.c, where a
 * host test can drive it against a fabricated volume - the loop itself is the
 * part that depends on a file's layout, and nothing on this side of the
 * boundary can be tested without a device. */
static uint32_t fat_io_next(void *ctx, uint32_t cluster) {
    (void)ctx;
    return fat_next_cluster(cluster);
}

static int fat_io_end(void *ctx, uint32_t cluster) {
    (void)ctx;
    return fat_chain_end(cluster);
}

static uint32_t fat_io_lba(void *ctx, uint32_t cluster) {
    (void)ctx;
    return fat_cluster_lba(cluster);
}

static int fat_io_sectors(void *ctx, uint32_t lba, void *dst, uint32_t sectors) {
    (void)ctx;
    return g_boot_read_many ? g_boot_read_many(lba, dst, sectors) : -1;
}

static int fat_io_partial(void *ctx, uint32_t lba, void *dst, uint32_t bytes) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    (void)ctx;
    /* Through the bounce buffer, so a partial sector never writes past the end
     * of what the caller asked for. */
    if (fat_sector_read(lba, g_secbuf) != 0) {
        return -1;
    }
    for (i = 0; i < bytes; i++) {
        d[i] = g_secbuf[i];
    }
    return 0;
}

/* Read a whole file by path (e.g. "EFI/BOOT/INIT.ELF") into buf (up to bufcap).
 * Returns the file size, or -1 on error / too big / short read. */
static long fat_read_file_locked(const char *path, void *buf, uint32_t bufcap) {
    uint32_t cluster = 0, size = 0;
    vibeos_fat_chain_io_t io;
    long copied;

    if (!g_fat_cur->mounted || fat_resolve(path, &cluster, &size) != 0) {
        return -1;
    }
    if (size > bufcap) {
        return -1;
    }
    io.ctx = 0;
    io.cluster_bytes = (uint32_t)g_fat_cur->sectors_per_cluster * SECTOR_SIZE;
    io.next_cluster = fat_io_next;
    io.chain_end = fat_io_end;
    io.cluster_lba = fat_io_lba;
    io.read_sectors = fat_io_sectors;
    io.read_partial = fat_io_partial;

    g_fat_chain_error = 0;
    copied = vibeos_fat_chain_read(&io, cluster, size, (uint8_t *)buf);

    /* Report what was actually read, not what the directory entry claimed.
     *
     * This used to return `size` unconditionally. A chain that ended early - a
     * failed FAT sector read, a free cluster in the middle of it - therefore
     * looked like a complete file, and execve went on to parse a buffer whose
     * tail still held whatever the previous program had left in the shared
     * staging area. "Cannot exec" is the right answer to a short read, and a
     * much better one than loading half a program. */
    if (copied < 0 || g_fat_chain_error || (uint32_t)copied != size) {
        return -1;
    }
    return copied;
}

/* ---- SMP serialization ---------------------------------------------------
 *
 * The reader/writer above works out of a handful of shared static buffers, and
 * every request ultimately goes through the single virtio-blk virtqueue. Once
 * more than one core can be inside a filesystem syscall at the same time, both
 * would be corrupted, so the whole filesystem is entered under one lock.
 *
 * Callers are syscalls, which run with interrupts masked, so a core cannot be
 * preempted while holding it. */

/* The lock is the architecture's (vibeos_fat_set_lock): the spin with a pause
 * in it that used to be written here is one machine's instruction. With none
 * registered - the host tests, which are one thread - operations run unlocked. */
static void (*g_lock_fn)(void);
static void (*g_unlock_fn)(void);

void vibeos_fat_set_lock(void (*lock)(void), void (*unlock)(void)) {
    g_lock_fn = lock;
    g_unlock_fn = unlock;
}

static void fs_lock(void) {
    if (g_lock_fn) {
        g_lock_fn();
    }
}

static void fs_unlock(void) {
    if (g_unlock_fn) {
        g_unlock_fn();
    }
}

/* Where a file's bytes physically are, and whether they are one unbroken run.
 *
 * Swap needs this and nothing else does. The swap map moves 4 KiB into a slot
 * by sector number; it has no idea what a filesystem is, and it must not
 * acquire one. So this is the whole of the translation: resolve a path, walk
 * its chain, and report the first sector, the length, and whether the walk
 * ever jumped.
 *
 * Contiguity is *reported*, not assumed. A swap area that believed a
 * fragmented file was one run would write pages of somebody's memory into
 * whatever lies between its extents - which on this volume is other files -
 * and the damage would surface at the next boot as a corrupted program with no
 * event to point at. vibeos_swaparea_configure refuses a file that is not
 * contiguous and counts the refusal; this function's job is to tell it the
 * truth, and returning "contiguous" for a chain it could not fully walk would
 * be the one lie that matters here.
 *
 * The length reported is the whole clusters the chain actually covers, not the
 * size in the directory entry. Those differ - the last cluster is usually
 * partly unused - and a swap area sized from the declared length would run
 * past the end of the allocation. This project has already had one defect from
 * trusting a directory's declared size over what was really read.
 *
 * Returns 0 on success. `contiguous` is 0 or 1 and is meaningful even then:
 * a fragmented file resolves fine and is simply not usable as swap. */
int vibeos_fat_file_extent(const char *path, uint64_t *out_first_lba,
                                  uint64_t *out_sectors, int *out_contiguous) {
    uint32_t cluster = 0, size = 0;
    uint32_t cl, prev;
    uint64_t clusters = 0;
    int contiguous = 1;
    int rc = -1;

    if (!path || !out_first_lba || !out_sectors || !out_contiguous) {
        return -1;
    }
    *out_first_lba = 0;
    *out_sectors = 0;
    *out_contiguous = 0;

    fs_lock();
    /* The boot volume, explicitly.
     *
     * It was implicit, which meant "whatever the last operation selected" -
     * and the first time a second volume existed, this read that one instead
     * and the swap area could not find its file. With a current-volume
     * pointer, an entry point that does not select is an operation on the
     * wrong volume, silently. */
    fat_select(0);
    if (fat_open_locked(path, &cluster, &size) != 0 || cluster < 2u) {
        fs_unlock();
        return -1;
    }
    g_fat_chain_error = 0;
    prev = cluster;
    cl = cluster;
    while (!fat_chain_end(cl) && cl >= 2u && cl - 2u < g_fat_cur->max_clusters) {
        if (clusters != 0ull && cl != prev + 1u) {
            contiguous = 0;
        }
        clusters++;
        if (clusters > (uint64_t)g_fat_cur->max_clusters) {
            break;                 /* a chain that loops is not a file */
        }
        prev = cl;
        cl = fat_next_cluster(cl);
    }
    /* A table read that failed reports the end-of-chain marker, which is the
     * same value a healthy last cluster returns - the trap this file already
     * carries a comment about. A short walk must not be reported as a short
     * contiguous file. */
    if (!g_fat_chain_error && clusters > 0ull) {
        uint32_t first_lba = fat_cluster_lba(cluster);
        if (first_lba != 0u) {
            *out_first_lba = (uint64_t)first_lba;
            *out_sectors = clusters * (uint64_t)g_fat_cur->sectors_per_cluster;
            *out_contiguous = contiguous;
            rc = 0;
        }
    }
    (void)size;
    fs_unlock();
    return rc;
}

int vibeos_fat_mount(void) {
    int r;
    fs_lock();
    /* Volume 0 is the boot volume, always. Everything above still reaches this
     * driver through the no-argument entry points, and they operate on it. */
    r = fat_mount_on(&g_volumes[0], 0, 0u);
    if (r == 0 && g_volume_count == 0u) {
        g_volume_count = 1u;
    }
    fs_unlock();
    return r;
}

/* Mount another volume, from a device the caller names.
 *
 * Returns the handle the VFS ops carry back, or null. The handle is what makes
 * the rest of this driver multi-volume: every operation selects the volume
 * from it before doing anything, under the same lock.
 */
void *vibeos_fat_mount_volume(vibeos_blockcache_t *bc,
                                     uint32_t first_lba) {
    fat_fs_t *vol;
    int r;

    if (!bc || first_lba == 0u) {
        /* A second volume at sector 0 would be the whole device, which is the
         * boot volume's job and not something to have two opinions about. */
        return 0;
    }
    fs_lock();
    if (g_volume_count == 0u) {
        g_volume_count = 1u;      /* slot 0 is reserved for the boot volume */
    }
    if (g_volume_count >= FAT_MAX_VOLUMES) {
        fs_unlock();
        return 0;
    }
    vol = &g_volumes[g_volume_count];
    r = fat_mount_on(vol, bc, first_lba);
    if (r != 0) {
        /* Left out of the count, so a failed mount does not consume a slot and
         * does not leave a half-built volume something could select. */
        g_fat_cur = &g_volumes[0];
        fs_unlock();
        return 0;
    }
    g_volume_count++;
    g_fat_cur = &g_volumes[0];
    fs_unlock();
    return vol;
}


/* Forget every volume but the boot one. For tests: a volume has no unmount,
 * because nothing on a running machine ever takes one away, and a test that
 * formats and mounts in a loop would run out of the four there are. */
void vibeos_fat_forget_volumes(void) {
    uint32_t i;

    fs_lock();
    for (i = 1; i < FAT_MAX_VOLUMES; i++) {
        g_volumes[i].mounted = 0;
        g_volumes[i].cache = 0;
    }
    if (g_volume_count > 1u) {
        g_volume_count = 1u;
    }
    g_fat_cur = &g_volumes[0];
    fat_cache_drop();
    fs_unlock();
}

int vibeos_fat_open_on(void *vol, const char *path,
                              uint32_t *out_cluster, uint32_t *out_size) {
    int r;
    fs_lock();
    fat_select(vol);
    r = fat_open_locked(path, out_cluster, out_size);
    fs_unlock();
    return r;
}

int vibeos_fat_open(const char *path, uint32_t *out_cluster, uint32_t *out_size) {
    return vibeos_fat_open_on(0, path, out_cluster, out_size);
}

long vibeos_fat_read_at_on(void *vol, uint32_t first_cluster, uint32_t size,
                                  uint32_t off, void *buf, uint32_t len) {
    long r;
    fs_lock();
    fat_select(vol);
    r = fat_read_at_locked(first_cluster, size, off, buf, len);
    fs_unlock();
    return r;
}

long vibeos_fat_read_at(uint32_t first_cluster, uint32_t size, uint32_t off,
                               void *buf, uint32_t len) {
    return vibeos_fat_read_at_on(0, first_cluster, size, off, buf, len);
}

int vibeos_fat_list_on(void *vol, const char *path, uint32_t idx, char *name,
                              uint32_t name_cap, uint32_t *out_size, int *out_is_dir) {
    int r;
    fs_lock();
    fat_select(vol);
    r = fat_list_locked(path, idx, name, name_cap, out_size, out_is_dir);
    fs_unlock();
    return r;
}

int vibeos_fat_list(const char *path, uint32_t idx, char *name, uint32_t *out_size,
                           int *out_is_dir) {
    return vibeos_fat_list_on(0, path, idx, name, 13u, out_size, out_is_dir);
}

long vibeos_fat_write_file_on(void *vol, const char *path, const void *buf, uint32_t len) {
    long r;
    fs_lock();
    fat_select(vol);
    r = fat_write_whole(path, buf, len);
    fs_unlock();
    return r;
}

long vibeos_fat_write_file(const char *path, const void *buf, uint32_t len) {
    return vibeos_fat_write_file_on(0, path, buf, len);
}

int vibeos_fat_unlink_on(void *vol, const char *path) {
    int r;
    fs_lock();
    fat_select(vol);
    r = g_fat_cur->mounted ? fat_remove(path, 0) : -VIBEOS_ENOENT;
    fs_unlock();
    return r;
}

int vibeos_fat_unlink(const char *path) {
    return vibeos_fat_unlink_on(0, path);
}

int vibeos_fat_mkdir_on(void *vol, const char *path) {
    int r;
    fs_lock();
    fat_select(vol);
    r = g_fat_cur->mounted ? fat_create(path, 0x10, 0) : -VIBEOS_ENOENT;
    fs_unlock();
    return r;
}

int vibeos_fat_mkdir(const char *path) {
    return vibeos_fat_mkdir_on(0, path);
}

long vibeos_fat_read_file(const char *path, void *buf, uint32_t bufcap) {
    long r;
    fs_lock();
    fat_select(0);
    r = fat_read_file_locked(path, buf, bufcap);
    fs_unlock();
    return r;
}

/* ==== FAT as a filesystem driver ====================================================
 *
 * A thin adapter over the code above: the operations the VFS calls, and the
 * driver the volume scan finds. It was a file of its own (fat_vfs.c) while the
 * driver lived in the architecture's directory; one module, one file, since
 * docs/abi/ L1 step 4. */

/* What stat reports for an entry. FAT keeps no owner and one permission, read-
 * only; a node's identity is where its entry is. */
static void fat_node_fill(const fat_ent_t *e, vibeos_fs_node_t *out) {
    uint32_t perm = (e->attr & 0x10u) ? 0755u : 0644u;

    if (e->attr & 0x01u) {
        perm &= ~0222u;
    }
    out->id = fat_ent_id(e);
    out->is_dir = (e->attr & 0x10u) ? 1 : 0;
    out->size = out->is_dir ? 0u : e->size;
    out->mode = (out->is_dir ? VIBEOS_S_IFDIR : VIBEOS_S_IFREG) | perm;
    out->nlink = 1u;
    out->atime_ns = out->mtime_ns = out->ctime_ns = fat_time_ns(e->date, e->time);
}

static int fat_op_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    fat_ent_t e;
    int is_root = 0, r;

    fs_lock();
    fat_select(fs);
    r = g_fat_cur->mounted ? fat_path_ent(path, &e, &is_root) : -VIBEOS_ENOENT;
    if (r == 0) {
        if (is_root) {
            out->id = 0;
            out->size = 0;
            out->is_dir = 1;
        } else {
            fat_node_fill(&e, out);
        }
    }
    fs_unlock();
    return r;
}

/* The entry is read again on every call: the size and the first cluster are
 * whatever the volume says now, not what they were when the file was opened. */
static long fat_op_read_at(void *fs, const vibeos_fs_node_t *node,
                           uint64_t offset, void *buf, uint32_t len) {
    fat_ent_t e;
    long r;

    fs_lock();
    fat_select(fs);
    r = fat_ent_load(node->id, &e);
    if (r == 0 && (e.attr & 0x10u)) {
        r = -VIBEOS_EISDIR;   /* directories are enumerated, not read as bytes */
    }
    if (r == 0) {
        r = offset >= e.size ? 0
                             : fat_read_at_locked(e.cluster, e.size, (uint32_t)offset, buf, len);
    }
    fs_unlock();
    return r;
}

static long fat_op_write_at(void *fs, const vibeos_fs_node_t *node, uint64_t offset,
                            const void *buf, uint32_t len) {
    fat_ent_t e;
    long r;

    fs_lock();
    fat_select(fs);
    r = fat_ent_load(node->id, &e);
    if (r == 0 && (e.attr & 0x10u)) {
        r = -VIBEOS_EISDIR;
    }
    if (r == 0) {
        r = fat_write_at(&e, offset, buf, len);
    }
    fs_unlock();
    return r;
}

static int fat_op_truncate(void *fs, const vibeos_fs_node_t *node, uint64_t size) {
    fat_ent_t e;
    int r;

    if (size > 0xFFFFFFFFull) {
        return -VIBEOS_EFBIG;
    }
    fs_lock();
    fat_select(fs);
    r = fat_ent_load(node->id, &e);
    if (r == 0 && (e.attr & 0x10u)) {
        r = -VIBEOS_EISDIR;
    }
    if (r == 0) {
        r = fat_resize(&e, (uint32_t)size);
    }
    fs_unlock();
    return r;
}

static long fat_op_write_file(void *fs, const char *path, const void *buf, uint32_t len) {
    return vibeos_fat_write_file_on(fs, path, buf, len);
}

static int fat_op_list(void *fs, const char *path, uint32_t index, char *name,
                       uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    uint32_t size = 0;
    int is_dir = 0;

    if (name_cap == 0u) {
        return -1;
    }
    if (vibeos_fat_list_on(fs, path, index, name, name_cap, &size, &is_dir) != 0) {
        return -1;
    }
    if (out_size) {
        *out_size = size;
    }
    if (out_is_dir) {
        *out_is_dir = is_dir;
    }
    return 0;
}

static int fat_op_unlink(void *fs, const char *path) {
    return vibeos_fat_unlink_on(fs, path);
}

static int fat_op_mkdir(void *fs, const char *path) {
    return vibeos_fat_mkdir_on(fs, path);
}

static int fat_op_create(void *fs, const char *path, uint32_t mode, vibeos_fs_node_t *out) {
    fat_ent_t e;
    int r;

    fs_lock();
    fat_select(fs);
    /* The one permission FAT keeps: a file made without any write bit is
     * read-only. */
    r = fat_create(path, (uint8_t)(0x20u | ((mode & 0222u) ? 0u : 0x01u)), &e);
    if (r == 0) {
        fat_node_fill(&e, out);
    }
    fs_unlock();
    return r;
}

static int fat_op_rmdir(void *fs, const char *path) {
    int r;

    fs_lock();
    fat_select(fs);
    r = fat_remove(path, 1);
    fs_unlock();
    return r;
}

static int fat_op_rename(void *fs, const char *from, const char *to, uint32_t flags) {
    int r;

    fs_lock();
    fat_select(fs);
    r = fat_rename(from, to, flags);
    fs_unlock();
    return r;
}

/* What FAT can take of an attribute change: the modification time, and the
 * write permission as its read-only bit. It has no owners - one that is not
 * root's is refused, root's is what every file already has - and no access
 * time worth the write. */
static int fat_op_setattr(void *fs, const char *path, const vibeos_fs_attr_t *a) {
    fat_ent_t e;
    int is_root = 0, r;

    if (((a->valid & VIBEOS_ATTR_UID) && a->uid != 0u) ||
        ((a->valid & VIBEOS_ATTR_GID) && a->gid != 0u)) {
        return -VIBEOS_EPERM;
    }
    fs_lock();
    fat_select(fs);
    r = fat_path_ent(path, &e, &is_root);
    if (r == 0 && !is_root && (a->valid & (VIBEOS_ATTR_MODE | VIBEOS_ATTR_MTIME))) {
        if ((a->valid & VIBEOS_ATTR_MODE) && (e.attr & 0x10u) == 0u) {
            e.attr = (uint8_t)((e.attr & ~0x01u) | ((a->mode & 0222u) ? 0u : 0x01u));
        }
        if (a->valid & VIBEOS_ATTR_MTIME) {
            fat_time_from_ns(a->mtime_ns, &e.date, &e.time);
        }
        r = fat_ent_store(&e);
    }
    fs_unlock();
    return r;
}

static int fat_op_statfs(void *fs, vibeos_fs_statfs_t *out) {
    fs_lock();
    fat_select(fs);
    out->magic = 0x4d44u;   /* Linux's MSDOS_SUPER_MAGIC */
    out->block_size = fat_cluster_bytes();
    out->blocks = g_fat_cur->max_clusters;
    out->blocks_free = fat_free_clusters();
    out->name_max = FAT_LFN_MAX;
    out->read_only = 0;
    fs_unlock();
    return 0;
}

/* Writes go through the cache and straight out already; this is for a caller
 * that wants to hear it said. */
static int fat_op_sync(void *fs) {
    vibeos_blockcache_t *bc;
    int r = 0;

    fs_lock();
    fat_select(fs);
    bc = fat_cache();
    if (bc && vibeos_blockcache_flush(bc) != 0) {
        r = -VIBEOS_EIO;
    }
    fs_unlock();
    return r;
}

/* Designated, not positional: the table grew eleven operations in L1, and a
 * positional initialiser is how a struct silently gains a hole (CLAUDE.md). No
 * link, symlink or readlink: FAT has neither kind, and the wrappers say EPERM. */
static const vibeos_fs_ops_t g_fat_ops = {
    .lookup = fat_op_lookup,
    .read_at = fat_op_read_at,
    .write_file = fat_op_write_file,
    .list = fat_op_list,
    .unlink = fat_op_unlink,
    .mkdir = fat_op_mkdir,
    .write_at = fat_op_write_at,
    .truncate = fat_op_truncate,
    .create = fat_op_create,
    .rmdir = fat_op_rmdir,
    .rename = fat_op_rename,
    .setattr = fat_op_setattr,
    .statfs = fat_op_statfs,
    .sync = fat_op_sync,
};

/* Mount the boot volume. Returns 0 on success. Reached as the driver's mount
 * with a first_lba of zero - see fat_scan_mount. */
static int fat_vfs_mount_boot(vibeos_fsmount_t *mnt) {
    if (vibeos_fat_mount() != 0) {
        return -1;
    }
    return vibeos_fs_mount(mnt, &g_fat_ops, 0, "fat");
}

/* ---- joining the volume scan (I4b step 3) ---------------------------------
 *
 * The scan lives in kernel/fs/ and cannot name this driver: it is in the arch
 * layer, and kernel/fs depending on kernel/arch would invert the layering the
 * whole storage refactor exists to establish. So this registers itself.
 *
 * That is the measurement from steps 1 and 2 being acted on rather than
 * written down. The scan found a 504 MB volume, identified it correctly as
 * FAT, and reported `fs=none`, because the only FAT driver on the machine was
 * invisible to the code doing the identifying.
 */


/* Does this volume look like FAT?
 *
 * Reads the boot sector and nothing else, and answers without keeping
 * anything. The three fields checked are the ones that are wrong on a volume
 * that merely *resembles* FAT:
 *
 *  - the 0xAA55 signature, which every boot sector has and which alone proves
 *    nothing;
 *  - a bytes-per-sector that is a sane power of two, because exFAT stores a
 *    log2 there and NTFS a different layout, so a plain 512 or 4096 is already
 *    a discriminator;
 *  - a non-zero sectors-per-cluster and a non-zero FAT count, which exFAT
 *    leaves as zero precisely so a FAT driver does not claim it.
 *
 * The last of those is the one that matters and it is the reason the probe
 * exists at all: a check of the jump instruction and the signature says yes to
 * an exFAT volume, and a FAT driver that mounts one produces files full of
 * nonsense rather than a refusal. exFAT and NTFS are probed before this for
 * the same reason.
 */
static int fat_probe(vibeos_blockcache_t *cache, uint64_t first_lba) {
    uint8_t sec[VIBEOS_BLOCK_SIZE];
    uint32_t bytes_per_sector;

    if (!cache) {
        return -1;
    }
    if (vibeos_blockcache_read(cache, first_lba, sec) != 0) {
        return -1;
    }
    if (sec[510] != 0x55u || sec[511] != 0xAAu) {
        return -1;
    }
    bytes_per_sector = (uint32_t)sec[11] | ((uint32_t)sec[12] << 8);
    if (bytes_per_sector != 512u && bytes_per_sector != 1024u &&
        bytes_per_sector != 2048u && bytes_per_sector != 4096u) {
        return -1;
    }
    if (sec[13] == 0u) {
        return -1;            /* sectors per cluster; zero on exFAT */
    }
    if (sec[16] == 0u) {
        return -1;            /* number of FATs; zero on exFAT      */
    }
    return 0;
}

/* Mount, once the probe has said yes.
 *
 * The limitation this carried until the driver stopped being a singleton is
 * gone: it took a first_lba and ignored it, because fat.c found its own
 * partition by parsing the MBR itself, so it mounted the volume it would have
 * chosen anyway - the right one on a disk with one FAT partition and the wrong
 * one on a disk with two.
 *
 * A first_lba of zero still means "the boot volume", which is what the scan
 * asks for when it is looking at a whole disk. Anything else mounts a second
 * volume and hands back the handle every operation on it will carry. */
static int fat_scan_mount(vibeos_fsmount_t *out, vibeos_blockcache_t *cache,
                          uint64_t first_lba, uint64_t sectors, void *state) {
    void *vol;

    (void)sectors;   /* FAT reads its own geometry and keeps its own volumes */
    (void)state;

    if (first_lba == 0ull) {
        return fat_vfs_mount_boot(out);
    }
    vol = vibeos_fat_mount_volume(cache, (uint32_t)first_lba);
    if (!vol) {
        return -1;
    }
    /* The handle goes in as the mount's `fs`, which is the pointer every op
     * gets back. That is the whole of what makes this multi-volume: the ops
     * were already written to take it and were throwing it away. */
    return vibeos_fs_mount(out, &g_fat_ops, vol, "fat");
}





/* ---- format (I4c step 3) --------------------------------------------------
 *
 * An empty FAT16 volume: a boot sector, two copies of the table, and a root
 * directory of zeroes.
 *
 * ## Why FAT16 and not FAT12 or FAT32
 *
 * The three are the *same* format with the same header; which one a reader
 * uses is decided by the cluster count and by nothing else - under 4085 is
 * FAT12, under 65525 is FAT16. That means a formatter that picks its geometry
 * carelessly produces a volume of a different kind from the one it intended,
 * and every field still looks right. So the geometry here is chosen to land
 * comfortably inside FAT16 and the code refuses a volume too small to get
 * there, rather than silently producing FAT12.
 *
 * ## What is deliberately absent
 *
 * No boot code. The first three bytes are a jump to nothing, which is what
 * every tool writes for a data volume, and pretending otherwise would put a
 * bootloader on a partition nobody asked to boot from.
 */
#define FAT_FMT_RESERVED     1u
#define FAT_FMT_FATS         2u
#define FAT_FMT_ROOT_ENTRIES 512u
#define FAT_FMT_MIN_CLUSTERS 4085u   /* below this a reader sees FAT12 */

static void fat_fmt_wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void fat_fmt_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static int fat_format(vibeos_blockcache_t *cache, uint64_t first_lba,
                      uint64_t sectors) {
    uint8_t sec[VIBEOS_BLOCK_SIZE];
    uint32_t root_sectors = (FAT_FMT_ROOT_ENTRIES * 32u) / VIBEOS_BLOCK_SIZE;
    uint32_t spf, clusters;
    uint32_t i, f;

    if (!cache || sectors < 64ull || sectors > 0xFFFFull) {
        /* The 16-bit total-sectors field bounds this. A larger volume needs
         * the 32-bit field and a different set of checks, and quietly
         * truncating would produce a filesystem that claims to be smaller than
         * the partition it sits in - which reads fine and loses the tail. */
        return -1;
    }

    /* Solve for sectors-per-FAT. Two bytes per cluster, so a FAT sector holds
     * 256 entries. It converges immediately; the loop is here because the
     * table's size depends on the cluster count which depends on the table's
     * size, and writing that as a closed form would be a place to be subtly
     * wrong. */
    spf = 1u;
    for (i = 0; i < 8u; i++) {
        uint32_t overhead = FAT_FMT_RESERVED + FAT_FMT_FATS * spf + root_sectors;
        uint32_t next;
        if ((uint64_t)overhead >= sectors) {
            return -1;
        }
        clusters = (uint32_t)(sectors - overhead);
        next = (clusters + 255u) / 256u;
        if (next == spf) {
            break;
        }
        spf = next;
    }
    if (clusters < FAT_FMT_MIN_CLUSTERS) {
        /* Refused rather than written. A volume this size is FAT12, and a
         * formatter that produced one while its caller asked for FAT16 would
         * be handing back a filesystem of a kind nobody chose. */
        return -1;
    }

    /* ---- the boot sector ------------------------------------------------ */
    for (i = 0; i < VIBEOS_BLOCK_SIZE; i++) {
        sec[i] = 0;
    }
    sec[0] = 0xEBu; sec[1] = 0x3Cu; sec[2] = 0x90u;   /* jmp short; nop */
    sec[3] = 'V'; sec[4] = 'I'; sec[5] = 'B'; sec[6] = 'E';
    sec[7] = 'O'; sec[8] = 'S'; sec[9] = ' '; sec[10] = ' ';
    fat_fmt_wr16(&sec[11], (uint16_t)VIBEOS_BLOCK_SIZE);
    sec[13] = 1u;                                     /* sectors per cluster */
    fat_fmt_wr16(&sec[14], (uint16_t)FAT_FMT_RESERVED);
    sec[16] = (uint8_t)FAT_FMT_FATS;
    fat_fmt_wr16(&sec[17], (uint16_t)FAT_FMT_ROOT_ENTRIES);
    fat_fmt_wr16(&sec[19], (uint16_t)sectors);
    sec[21] = 0xF8u;                                  /* fixed disk */
    fat_fmt_wr16(&sec[22], (uint16_t)spf);
    fat_fmt_wr16(&sec[24], 32u);                      /* sectors per track */
    fat_fmt_wr16(&sec[26], 2u);                       /* heads */
    fat_fmt_wr32(&sec[28], (uint32_t)first_lba);      /* hidden sectors */
    sec[38] = 0x29u;                                  /* extended signature */
    fat_fmt_wr32(&sec[39], 0x56424F53u);              /* volume id */
    for (i = 0; i < 11u; i++) {
        sec[43 + i] = ' ';
    }
    sec[54] = 'F'; sec[55] = 'A'; sec[56] = 'T'; sec[57] = '1'; sec[58] = '6';
    sec[59] = ' '; sec[60] = ' '; sec[61] = ' ';
    fat_fmt_wr16(&sec[510], 0xAA55u);
    if (vibeos_blockcache_write(cache, first_lba, sec) != 0) {
        return -1;
    }

    /* ---- the tables ----------------------------------------------------- */
    for (f = 0; f < FAT_FMT_FATS; f++) {
        uint64_t base = first_lba + FAT_FMT_RESERVED + (uint64_t)f * spf;
        for (i = 0; i < spf; i++) {
            uint32_t k;
            for (k = 0; k < VIBEOS_BLOCK_SIZE; k++) {
                sec[k] = 0;
            }
            if (i == 0u) {
                /* Entry 0 carries the media descriptor, entry 1 the
                 * end-of-chain marker. Both are conventions a reader checks,
                 * and a table of pure zeroes is one many tools call corrupt. */
                sec[0] = 0xF8u; sec[1] = 0xFFu;
                sec[2] = 0xFFu; sec[3] = 0xFFu;
            }
            if (vibeos_blockcache_write(cache, base + i, sec) != 0) {
                return -1;
            }
        }
    }

    /* ---- the root directory --------------------------------------------- */
    for (i = 0; i < VIBEOS_BLOCK_SIZE; i++) {
        sec[i] = 0;
    }
    for (i = 0; i < root_sectors; i++) {
        uint64_t lba = first_lba + FAT_FMT_RESERVED +
                       (uint64_t)FAT_FMT_FATS * spf + i;
        if (vibeos_blockcache_write(cache, lba, sec) != 0) {
            return -1;
        }
    }

    /* Durable before the caller is told it worked. A format that is only in
     * the cache is a volume the next boot will not find. */
    return vibeos_blockcache_flush(cache);
}

/* The whole of what the rest of the kernel knows about this driver (C7). The
 * boot volume, the I4c formatting exercise and the volume scan all reach it
 * through vibeos_storage_driver("fat") or the scan; there used to be a register
 * function, an ops accessor and two exported function pointers, one for each. */
static const vibeos_fs_driver_t g_fat_driver = {
    "fat", fat_probe, fat_scan_mount, fat_format,
    50u   /* after NTFS and exFAT, whose boot sectors look like FAT's */
};
VIBEOS_FS_DRIVER(g_fat_driver);

/* For a caller that registers drivers itself: the host tests, which have no
 * linker section to walk. */
const vibeos_fs_driver_t *vibeos_fat_fs_driver(void) {
    return &g_fat_driver;
}
