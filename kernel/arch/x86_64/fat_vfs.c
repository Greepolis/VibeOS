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

extern int vibeos_x86_64_fat_mount(void);
extern int vibeos_x86_64_fat_open(const char *path, uint32_t *out_cluster,
                                  uint32_t *out_size);
extern long vibeos_x86_64_fat_read_at(uint32_t first_cluster, uint32_t size,
                                      uint32_t off, void *buf, uint32_t len);
extern int vibeos_x86_64_fat_list(const char *path, uint32_t idx, char *name,
                                  uint32_t *out_size, int *out_is_dir);
extern long vibeos_x86_64_fat_write_file(const char *path, const void *buf,
                                         uint32_t len);
extern int vibeos_x86_64_fat_unlink(const char *path);
extern int vibeos_x86_64_fat_mkdir(const char *path);

/* A directory has no size of its own in FAT, and the existing open() reports
 * zero for one. Whether a path is a directory is answered the way the rest of
 * the kernel already answers it: a path that can be enumerated is a directory.
 * Keeping that here rather than in the syscall layer is the reason this file
 * exists - it is a FAT fact, not a filesystem fact. */
static int fat_is_directory(const char *path) {
    char probe[16];
    uint32_t probe_size = 0;
    int probe_dir = 0;
    return vibeos_x86_64_fat_list(path, 0, probe, &probe_size, &probe_dir) == 0;
}

static int fat_vfs_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    uint32_t cluster = 0, size = 0;

    (void)fs;
    /* The volume root, however it is spelled. */
    if ((path[0] == '/' && path[1] == 0) || (path[0] == '.' && path[1] == 0) ||
        path[0] == 0) {
        out->id = 0;
        out->size = 0;
        out->is_dir = 1;
        return 0;
    }
    if (vibeos_x86_64_fat_open(path, &cluster, &size) != 0) {
        return -1;
    }
    out->id = cluster;
    out->size = size;
    /* A FAT directory entry carries no length, so a non-zero size settles it
     * without asking anything else. Enumerating the path is not a test on its
     * own: the lister accepts a file path and answers about its parent, which
     * made every ordinary file look like a directory - and a directory cannot
     * be read, so nothing loaded at all. */
    out->is_dir = (size == 0u) && fat_is_directory(path);
    return 0;
}

static long fat_vfs_read_at(void *fs, const vibeos_fs_node_t *node,
                            uint64_t offset, void *buf, uint32_t len) {
    (void)fs;
    if (node->is_dir) {
        return -1;   /* directories are enumerated, not read as bytes */
    }
    /* The underlying reader is 32-bit throughout; a FAT file cannot exceed
     * that anyway, so an offset beyond it is a caller error rather than a
     * case to support. */
    if (offset > 0xFFFFFFFFull || node->size > 0xFFFFFFFFull) {
        return -1;
    }
    return vibeos_x86_64_fat_read_at((uint32_t)node->id, (uint32_t)node->size,
                                     (uint32_t)offset, buf, len);
}

static long fat_vfs_write_file(void *fs, const char *path, const void *buf,
                               uint32_t len) {
    (void)fs;
    return vibeos_x86_64_fat_write_file(path, buf, len);
}

static int fat_vfs_list(void *fs, const char *path, uint32_t index, char *name,
                        uint32_t name_cap, uint64_t *out_size, int *out_is_dir) {
    uint32_t size = 0;
    int is_dir = 0;
    char local[VIBEOS_FS_NAME_MAX];
    uint32_t i;

    (void)fs;
    if (name_cap == 0u) {
        return -1;
    }
    if (vibeos_x86_64_fat_list(path, index, local, &size, &is_dir) != 0) {
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
    (void)fs;
    return vibeos_x86_64_fat_unlink(path);
}

static int fat_vfs_mkdir(void *fs, const char *path) {
    (void)fs;
    return vibeos_x86_64_fat_mkdir(path);
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
 * `first_lba` is accepted and not used, and that is a real limitation rather
 * than an oversight: this driver finds its own partition during mount, by
 * parsing the MBR itself. It therefore only works for the volume it would have
 * chosen anyway - which on this medium is the same one the scan is asking
 * about, and on a disk with two FAT partitions would be the wrong answer for
 * the second. Taking the offset properly means threading it through fat.c's
 * globals, and that is a change worth making on its own rather than inside
 * this one. */
static int fat_scan_mount(vibeos_fsmount_t *out, vibeos_blockcache_t *cache,
                          uint64_t first_lba) {
    (void)cache;
    (void)first_lba;
    return vibeos_x86_64_fat_vfs_mount(out);
}

static const vibeos_fs_driver_t g_fat_driver = {
    "fat", fat_probe, fat_scan_mount
};

void vibeos_x86_64_fat_register_driver(void) {
    (void)vibeos_storage_register(&g_fat_driver);
}
