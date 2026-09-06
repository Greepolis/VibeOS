/* FAT as a filesystem driver.
 *
 * A thin adapter over the existing FAT code rather than a rewrite of it. That
 * is the whole point of this stage: the abstraction lands with no behaviour
 * change, so if BusyBox still reads files, lists directories and execs
 * programs on the boot gate, the new layer carries the contract the old direct
 * calls carried. Doing a refactor and a feature in one step means neither gets
 * verified.
 */

#include "vibeos/vfs.h"
#include "vibeos/arch_x86_64.h"

/* Declared in vibeos/arch_x86_64.h, which this file includes. There was a
 * second set of externs here, and it drifted the moment the entry points
 * grew a volume argument - a second place to keep true is a second place
 * to be wrong. */

/* A directory has no size of its own in FAT, and the existing open() reports
 * zero for one. Whether a path is a directory is answered the way the rest of
 * the kernel already answers it: a path that can be enumerated is a directory.
 * Keeping that here rather than in the syscall layer is the reason this file
 * exists - it is a FAT fact, not a filesystem fact. */
static int fat_is_directory(void *fs, const char *path) {
    char probe[16];
    uint32_t probe_size = 0;
    int probe_dir = 0;
    return vibeos_x86_64_fat_list_on(fs, path, 0, probe, &probe_size,
                                     &probe_dir) == 0;
}

static int fat_vfs_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    uint32_t cluster = 0, size = 0;

    /* The volume root, however it is spelled. */
    if ((path[0] == '/' && path[1] == 0) || (path[0] == '.' && path[1] == 0) ||
        path[0] == 0) {
        out->id = 0;
        out->size = 0;
        out->is_dir = 1;
        return 0;
    }
    if (vibeos_x86_64_fat_open_on(fs, path, &cluster, &size) != 0) {
        return -1;
    }
    out->id = cluster;
    out->size = size;
    /* A FAT directory entry carries no length, so a non-zero size settles it
     * without asking anything else. Enumerating the path is not a test on its
     * own: the lister accepts a file path and answers about its parent, which
     * made every ordinary file look like a directory - and a directory cannot
     * be read, so nothing loaded at all. */
    out->is_dir = (size == 0u) && fat_is_directory(fs, path);
    return 0;
}

static long fat_vfs_read_at(void *fs, const vibeos_fs_node_t *node,
                            uint64_t offset, void *buf, uint32_t len) {
    if (node->is_dir) {
        return -1;   /* directories are enumerated, not read as bytes */
    }
    /* The underlying reader is 32-bit throughout; a FAT file cannot exceed
     * that anyway, so an offset beyond it is a caller error rather than a
     * case to support. */
    if (offset > 0xFFFFFFFFull || node->size > 0xFFFFFFFFull) {
        return -1;
    }
    return vibeos_x86_64_fat_read_at_on(fs, (uint32_t)node->id, (uint32_t)node->size,
                                     (uint32_t)offset, buf, len);
}

static long fat_vfs_write_file(void *fs, const char *path, const void *buf,
                               uint32_t len) {
        return vibeos_x86_64_fat_write_file_on(fs, path, buf, len);
}

static int fat_vfs_list(void *fs, const char *path, uint32_t index, char *name,
                        uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    uint32_t size = 0;
    int is_dir = 0;
    char local[VIBEOS_FS_NAME_MAX];
    uint32_t i;

    if (name_cap == 0u) {
        return -1;
    }
    if (vibeos_x86_64_fat_list_on(fs, path, index, local, &size, &is_dir) != 0) {
        return -1;
    }
    for (i = 0; i + 1u < name_cap && local[i]; i++) {
        name[i] = local[i];
    }
    name[i] = 0;
    if (out_size) {
        *out_size = size;
    }
    if (out_is_dir) {
        *out_is_dir = is_dir;
    }
    return 0;
}

static int fat_vfs_unlink(void *fs, const char *path) {
        return vibeos_x86_64_fat_unlink_on(fs, path);
}

static int fat_vfs_mkdir(void *fs, const char *path) {
        return vibeos_x86_64_fat_mkdir_on(fs, path);
}

static const vibeos_fs_ops_t g_fat_ops = {
    fat_vfs_lookup,
    fat_vfs_read_at,
    fat_vfs_write_file,
    fat_vfs_list,
    fat_vfs_unlink,
    fat_vfs_mkdir
};

/* Mount the boot volume. Returns 0 on success. */
int vibeos_x86_64_fat_vfs_mount(vibeos_fsmount_t *mnt) {
    if (vibeos_x86_64_fat_mount() != 0) {
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

#include "vibeos/blockdev.h"
#include "vibeos/storage.h"

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
                          uint64_t first_lba) {
    void *vol;

    if (first_lba == 0ull) {
        return vibeos_x86_64_fat_vfs_mount(out);
    }
    vol = vibeos_x86_64_fat_mount_volume(cache, (uint32_t)first_lba);
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

static const vibeos_fs_driver_t g_fat_driver = {
    "fat", fat_probe, fat_scan_mount, fat_format
};

/* Handed out so the I4c boot exercise can drive them without a second copy of
 * what a FAT volume looks like. */
int (*g_fat_driver_probe)(vibeos_blockcache_t *cache, uint64_t first_lba) =
    fat_probe;
int (*g_fat_driver_format)(vibeos_blockcache_t *cache, uint64_t first_lba,
                           uint64_t sectors) = fat_format;

/* The ops table, for a caller that mounts a volume itself. */
const vibeos_fs_ops_t *vibeos_x86_64_fat_ops(void) {
    return &g_fat_ops;
}

void vibeos_x86_64_fat_register_driver(void) {
    (void)vibeos_storage_register(&g_fat_driver);
}
