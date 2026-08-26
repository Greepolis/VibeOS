/* Read-only FAT16/FAT32 reader over the virtio-blk driver (image-only).
 *
 * Parses an MBR to find the first FAT partition, reads its BPB, and can look up
 * an 8.3 file in the root directory and read its contents. Enough to load user
 * programs from a real filesystem on a real block device.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"
#include "vibeos/fat_chain.h"

extern int vibeos_x86_64_virtio_blk_read(uint64_t sector, void *buf);

#define SECTOR_SIZE 512u

typedef struct {
    uint32_t part_lba;        /* partition start sector           */
    uint32_t fat_lba;         /* first FAT sector                 */
    uint32_t root_lba;        /* FAT16 root dir sector (0 if F32) */
    uint32_t data_lba;        /* first data sector                */
    uint32_t sectors_per_fat;
    uint32_t max_clusters;
    uint32_t root_cluster;    /* FAT32 root cluster               */
    uint16_t root_entries;    /* FAT16 root entry count           */
    uint8_t  sectors_per_cluster;
    uint8_t  is_fat32;
    int      mounted;
} fat_fs_t;

static fat_fs_t g_fat;
static uint8_t g_secbuf[SECTOR_SIZE] __attribute__((aligned(16)));

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* One cached sector of the file allocation table.
 *
 * Following a chain re-read the same FAT sector for every cluster in it: a
 * two-megabyte file meant about a thousand device round trips that returned
 * identical bytes, on top of the thousand that carried actual data. A single
 * sector of cache removes almost all of them, because a chain walks through
 * the table in order.
 *
 * Invalidated on mount rather than on write, and the write path invalidates it
 * explicitly - a stale entry here would send a read into the wrong cluster,
 * which is the kind of corruption that surfaces far from its cause. */
static uint8_t g_fatsec[SECTOR_SIZE];
static uint32_t g_fatsec_lba;
static int g_fatsec_valid;

/* Sticky "the chain walk went wrong" flag.
 *
 * fat_next_cluster() has to return a cluster number, so it reports a failed
 * FAT read as the end-of-chain marker - which is exactly what a healthy last
 * cluster returns. A reader cannot tell the two apart, so a single failed FAT
 * sector read looked like a complete, shorter file. Callers clear this before
 * a walk and check it after. */
static int g_fat_chain_error;

static void fat_cache_drop(void) {
    g_fatsec_valid = 0;
}

static const uint8_t *fat_table_sector(uint32_t lba) {
    if (g_fatsec_valid && g_fatsec_lba == lba) {
        return g_fatsec;
    }
    if (vibeos_x86_64_virtio_blk_read(lba, g_fatsec) != 0) {
        g_fatsec_valid = 0;
        g_fat_chain_error = 1;
        return 0;
    }
    g_fatsec_lba = lba;
    g_fatsec_valid = 1;
    return g_fatsec;
}

static int fat_mount_locked(void) {
    uint32_t part_lba = 0;
    uint16_t reserved, bytes_per_sec;
    uint32_t total_sectors, root_dir_sectors;

    g_fat.mounted = 0;

    /* MBR: use the first non-empty partition; fall back to a bare superfloppy. */
    if (vibeos_x86_64_virtio_blk_read(0, g_secbuf) != 0) {
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

    /* Read the volume boot record / BPB. */
    if (vibeos_x86_64_virtio_blk_read(part_lba, g_secbuf) != 0) {
        return -1;
    }
    bytes_per_sec = rd16(&g_secbuf[11]);
    if (bytes_per_sec != SECTOR_SIZE) {
        return -1;
    }
    g_fat.part_lba = part_lba;
    g_fat.sectors_per_cluster = g_secbuf[13];
    reserved = rd16(&g_secbuf[14]);
    g_fat.root_entries = rd16(&g_secbuf[17]);
    g_fat.sectors_per_fat = rd16(&g_secbuf[22]);
    if (g_fat.sectors_per_fat == 0) {
        g_fat.sectors_per_fat = rd32(&g_secbuf[36]);      /* FAT32 */
        g_fat.root_cluster = rd32(&g_secbuf[44]);
        g_fat.is_fat32 = 1;
    } else {
        g_fat.is_fat32 = 0;
    }
    if (g_fat.sectors_per_cluster == 0 || g_fat.sectors_per_fat == 0) {
        return -1;
    }

    total_sectors = rd16(&g_secbuf[19]);
    if (total_sectors == 0) {
        total_sectors = rd32(&g_secbuf[32]);
    }
    root_dir_sectors = ((uint32_t)g_fat.root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
    g_fat.fat_lba = part_lba + reserved;
    g_fat.root_lba = g_fat.fat_lba + 2u * g_fat.sectors_per_fat;         /* FAT16 root */
    g_fat.data_lba = g_fat.root_lba + root_dir_sectors;                  /* first data */
    if (total_sectors <= g_fat.data_lba - part_lba) {
        return -1;
    }
    g_fat.max_clusters = (total_sectors - (g_fat.data_lba - part_lba)) /
                         g_fat.sectors_per_cluster;
    if (g_fat.max_clusters == 0u) {
        return -1;
    }

    fat_cache_drop();
    g_fat.mounted = 1;
    vibeos_x86_64_serial_puts("[FAT] mounted ");
    vibeos_x86_64_serial_puts(g_fat.is_fat32 ? "FAT32" : "FAT16");
    vibeos_x86_64_serial_puts(" part_lba=0x");
    vibeos_x86_64_serial_print_hex(part_lba);
    vibeos_x86_64_serial_puts(" data_lba=0x");
    vibeos_x86_64_serial_print_hex(g_fat.data_lba);
    vibeos_x86_64_serial_puts("\n");
    return 0;
}

static uint32_t fat_cluster_lba(uint32_t cluster) {
    return g_fat.data_lba + (cluster - 2u) * g_fat.sectors_per_cluster;
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
    uint32_t per_sec = g_fat.is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t eoc = g_fat.is_fat32 ? 0x0FFFFFFFu : 0xFFFFu;
    uint32_t sec_index = cluster / per_sec;
    const uint8_t *sec;

    if (cluster < 2u || cluster - 2u >= g_fat.max_clusters ||
        sec_index >= g_fat.sectors_per_fat) {
        g_fat_chain_error = 1;
        return eoc;
    }
    sec = fat_table_sector(g_fat.fat_lba + sec_index);
    if (!sec) {
        return eoc;
    }
    if (g_fat.is_fat32) {
        return rd32(&sec[(cluster % per_sec) * 4u]) & 0x0FFFFFFFu;
    }
    return rd16(&sec[(cluster % per_sec) * 2u]);
}

static int fat_chain_end(uint32_t cluster) {
    return g_fat.is_fat32 ? (cluster >= 0x0FFFFFF8u) : (cluster >= 0xFFF8u);
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

/* Scan a run of directory sectors for an 8.3 name; report cluster/size/attr. */
static int fat_scan_sectors(uint32_t lba, uint32_t sectors, const uint8_t want[11],
                            uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr) {
    uint32_t s, e;
    for (s = 0; s < sectors; s++) {
        if (vibeos_x86_64_virtio_blk_read(lba + s, g_secbuf) != 0) {
            return -1;
        }
        for (e = 0; e < SECTOR_SIZE; e += 32u) {
            const uint8_t *d = &g_secbuf[e];
            int k, match = 1;
            if (d[0] == 0x00) {
                return -1; /* end of directory */
            }
            if (d[0] == 0xE5 || (d[11] & 0x0Fu) == 0x0Fu || (d[11] & 0x08u)) {
                continue;  /* deleted, long-name, or volume label */
            }
            for (k = 0; k < 11; k++) {
                if (d[k] != want[k]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
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
static int fat_dir_find(uint32_t dir_cluster, const uint8_t want[11],
                        uint32_t *out_cluster, uint32_t *out_size, uint8_t *out_attr) {
    uint32_t cl;
    uint32_t steps = 0;

    if (dir_cluster == 0 && !g_fat.is_fat32) {
        uint32_t root_sectors = ((uint32_t)g_fat.root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
        return fat_scan_sectors(g_fat.root_lba, root_sectors, want, out_cluster, out_size, out_attr);
    }
    cl = (dir_cluster == 0) ? g_fat.root_cluster : dir_cluster;
    while (!fat_chain_end(cl) && cl >= 2u && cl - 2u < g_fat.max_clusters &&
           steps++ < g_fat.max_clusters) {
        if (fat_scan_sectors(fat_cluster_lba(cl), g_fat.sectors_per_cluster,
                             want, out_cluster, out_size, out_attr) == 0) {
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
        char comp[13];
        uint8_t want[11], attr = 0;
        uint32_t cl = 0, size = 0;
        int n = 0;
        int last;

        while (*p && *p != '/' && *p != '\\' && n < 12) {
            comp[n++] = *p++;
        }
        comp[n] = 0;
        while (*p == '/' || *p == '\\') {
            p++;
        }
        last = (*p == 0);

        fat_make_83(comp, want);
        if (fat_dir_find(dir, want, &cl, &size, &attr) != 0) {
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

extern int vibeos_x86_64_virtio_blk_write(uint64_t sector, const void *buf);
extern int vibeos_x86_64_virtio_blk_read_many(uint64_t sector, void *buf, uint32_t sectors);

/* Resolve a path to its first cluster and size (a file "open"). */
static int fat_open_locked(const char *path, uint32_t *out_cluster, uint32_t *out_size) {
    if (!g_fat.mounted || !out_cluster || !out_size) {
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

    if (!g_fat.mounted || !buf) {
        return -1;
    }
    if (off >= size) {
        return 0;
    }
    if (len > size - off) {
        len = size - off;
    }
    cluster_bytes = (uint32_t)g_fat.sectors_per_cluster * SECTOR_SIZE;
    cluster = first_cluster;
    g_fat_chain_error = 0;
    for (skip = off / cluster_bytes; skip > 0 && !fat_chain_end(cluster); skip--) {
        cluster = fat_next_cluster(cluster);
    }
    off %= cluster_bytes;

    while (done < len && !fat_chain_end(cluster) && cluster >= 2u) {
        uint32_t s;
        for (s = off / SECTOR_SIZE; s < g_fat.sectors_per_cluster && done < len; s++) {
            uint32_t in_sec = off % SECTOR_SIZE;
            uint32_t n = SECTOR_SIZE - in_sec;
            uint32_t i;
            if (vibeos_x86_64_virtio_blk_read(fat_cluster_lba(cluster) + s, g_secbuf) != 0) {
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

/* Enumerate directory entries: fill name (8.3, NUL-terminated) for entry index
 * `idx` of the directory at `path` (empty/"/" = root). Returns 0 on success,
 * -1 when the index is past the end. */
static int fat_list_locked(const char *path, uint32_t idx, char *name, uint32_t *out_size,
                           int *out_is_dir) {
    uint32_t dir_cluster = 0, sectors, lba, s, e, seen = 0;

    if (!g_fat.mounted || !name) {
        return -1;
    }
    if (path && path[0] && !(path[0] == '/' && path[1] == 0)) {
        uint32_t sz = 0;
        if (fat_resolve(path, &dir_cluster, &sz) != 0) {
            return -1;
        }
    }
    if (dir_cluster == 0 && !g_fat.is_fat32) {
        lba = g_fat.root_lba;
        sectors = ((uint32_t)g_fat.root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
    } else {
        lba = fat_cluster_lba(dir_cluster == 0 ? g_fat.root_cluster : dir_cluster);
        sectors = g_fat.sectors_per_cluster;
    }

    for (s = 0; s < sectors; s++) {
        if (vibeos_x86_64_virtio_blk_read(lba + s, g_secbuf) != 0) {
            return -1;
        }
        for (e = 0; e < SECTOR_SIZE; e += 32u) {
            const uint8_t *d = &g_secbuf[e];
            int k, n = 0;
            if (d[0] == 0x00) {
                return -1; /* end of directory */
            }
            if (d[0] == 0xE5 || (d[11] & 0x0Fu) == 0x0Fu || (d[11] & 0x08u)) {
                continue;
            }
            if (seen++ != idx) {
                continue;
            }
            for (k = 0; k < 8 && d[k] != ' '; k++) {
                name[n++] = (char)d[k];
            }
            if (d[8] != ' ') {
                name[n++] = '.';
                for (k = 8; k < 11 && d[k] != ' '; k++) {
                    name[n++] = (char)d[k];
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

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu); p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu); p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Separate buffer for FAT-table I/O: directory scanning holds g_secbuf. */
static uint8_t g_fatbuf[SECTOR_SIZE] __attribute__((aligned(16)));

/* Write one FAT entry to every FAT copy. */
static int fat_set_entry(uint32_t cluster, uint32_t value) {
    uint32_t per_sec = g_fat.is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t s = cluster / per_sec, i = cluster % per_sec, copy;

    if (s >= g_fat.sectors_per_fat) {
        return -1;
    }
    if (vibeos_x86_64_virtio_blk_read(g_fat.fat_lba + s, g_fatbuf) != 0) {
        return -1;
    }
    fat_cache_drop();
    if (g_fat.is_fat32) {
        wr32(&g_fatbuf[i * 4u], value & 0x0FFFFFFFu);
    } else {
        wr16(&g_fatbuf[i * 2u], (uint16_t)value);
    }
    for (copy = 0; copy < 2u; copy++) {
        if (vibeos_x86_64_virtio_blk_write(g_fat.fat_lba + copy * g_fat.sectors_per_fat + s,
                                           g_fatbuf) != 0) {
            return -1;
        }
    }
    return 0;
}

static uint32_t fat_get_entry(uint32_t cluster) {
    uint32_t per_sec = g_fat.is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t s = cluster / per_sec, i = cluster % per_sec;

    if (s >= g_fat.sectors_per_fat ||
        vibeos_x86_64_virtio_blk_read(g_fat.fat_lba + s, g_fatbuf) != 0) {
        return g_fat.is_fat32 ? 0x0FFFFFFFu : 0xFFFFu;
    }
    return g_fat.is_fat32 ? (rd32(&g_fatbuf[i * 4u]) & 0x0FFFFFFFu) : rd16(&g_fatbuf[i * 2u]);
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
    uint32_t per_sec = g_fat.is_fat32 ? (SECTOR_SIZE / 4u) : (SECTOR_SIZE / 2u);
    uint32_t total = g_fat.sectors_per_fat * per_sec;
    uint32_t first = 0, prev = 0, cl = 2u, got = 0;
    uint32_t eoc = g_fat.is_fat32 ? 0x0FFFFFFFu : 0xFFFFu;

    while (got < count && cl < total) {
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
    return first;
}

/* Map directory sector index `i` to an absolute LBA. dir_cluster 0 means the
 * root (a fixed region on FAT16, the root chain on FAT32). */
static int fat_dir_sector(uint32_t dir_cluster, uint32_t i, uint32_t *out_lba) {
    uint32_t cl;

    if (dir_cluster == 0 && !g_fat.is_fat32) {
        uint32_t root_sectors = ((uint32_t)g_fat.root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
        if (i >= root_sectors) {
            return -1;
        }
        *out_lba = g_fat.root_lba + i;
        return 0;
    }
    cl = (dir_cluster == 0) ? g_fat.root_cluster : dir_cluster;
    while (i >= g_fat.sectors_per_cluster) {
        cl = fat_get_entry(cl);
        if (cl < 2u || fat_chain_end(cl)) {
            return -1;
        }
        i -= g_fat.sectors_per_cluster;
    }
    *out_lba = fat_cluster_lba(cl) + i;
    return 0;
}

/* Find the directory slot for `want`, or the first free slot when `create`.
 * Leaves the containing sector in g_secbuf and reports where the entry sits. */
static int fat_dir_slot(uint32_t dir_cluster, const uint8_t want[11], int create,
                        uint32_t *out_lba, uint32_t *out_off) {
    uint32_t i, lba, e;
    int free_seen = 0;
    uint32_t free_lba = 0, free_off = 0;

    for (i = 0; fat_dir_sector(dir_cluster, i, &lba) == 0; i++) {
        if (vibeos_x86_64_virtio_blk_read(lba, g_secbuf) != 0) {
            return -1;
        }
        for (e = 0; e < SECTOR_SIZE; e += 32u) {
            const uint8_t *d = &g_secbuf[e];
            int k, match = 1;

            if (d[0] == 0x00 || d[0] == 0xE5) {
                if (create && !free_seen) {
                    free_seen = 1;
                    free_lba = lba;
                    free_off = e;
                }
                if (d[0] == 0x00) {
                    goto done; /* nothing beyond here */
                }
                continue;
            }
            if ((d[11] & 0x0Fu) == 0x0Fu) {
                continue; /* long-name fragment */
            }
            for (k = 0; k < 11; k++) {
                if (d[k] != want[k]) {
                    match = 0;
                    break;
                }
            }
            if (match) {
                *out_lba = lba;
                *out_off = e;
                return 0;
            }
        }
    }
done:
    if (create && free_seen) {
        if (vibeos_x86_64_virtio_blk_read(free_lba, g_secbuf) != 0) {
            return -1;
        }
        *out_lba = free_lba;
        *out_off = free_off;
        return 1; /* fresh slot */
    }
    return -1;
}

/* Split "a/b/c" into the parent directory cluster and the final 8.3 name. */
static int fat_split_parent(const char *path, uint32_t *out_dir, uint8_t name83[11]) {
    char parent[96];
    const char *last = path;
    const char *p = path;
    uint32_t n = 0, size = 0;

    while (*p) {
        if (*p == '/' || *p == '\\') {
            last = p + 1;
        }
        p++;
    }
    if (last == path) {
        *out_dir = 0; /* root */
    } else {
        while (path + n < last - 1 && n < sizeof(parent) - 1u) {
            parent[n] = path[n];
            n++;
        }
        parent[n] = 0;
        if (fat_resolve(parent, out_dir, &size) != 0) {
            return -1;
        }
    }
    fat_make_83(last, name83);
    return 0;
}

/* Create or overwrite a file at `path` with `len` bytes (any directory, any
 * length that fits the volume). Returns bytes written. */
static long fat_write_file_locked(const char *path, const void *buf, uint32_t len) {
    uint8_t want[11];
    uint32_t dir_cluster = 0, lba = 0, off = 0, cluster_bytes, need, first = 0;
    uint32_t wrote = 0, cl;
    const uint8_t *in = (const uint8_t *)buf;
    int slot;

    if (!g_fat.mounted || !buf) {
        return -1;
    }
    if (fat_split_parent(path, &dir_cluster, want) != 0) {
        return -1;
    }
    slot = fat_dir_slot(dir_cluster, want, 1, &lba, &off);
    if (slot < 0) {
        return -1; /* directory full */
    }
    if (slot == 0) { /* existing entry: drop its old contents */
        const uint8_t *d = &g_secbuf[off];
        uint32_t old = ((uint32_t)rd16(&d[20]) << 16) | rd16(&d[26]);
        if (old >= 2u) {
            fat_free_chain(old);
        }
    }

    cluster_bytes = (uint32_t)g_fat.sectors_per_cluster * SECTOR_SIZE;
    need = (len + cluster_bytes - 1u) / cluster_bytes;
    if (need > 0) {
        first = fat_alloc_chain(need);
        if (first == 0) {
            return -1;
        }
    }

    /* Write the payload across the chain, zero-padding the last sector. */
    cl = first;
    while (wrote < len && cl >= 2u && !fat_chain_end(cl)) {
        uint32_t s;
        for (s = 0; s < g_fat.sectors_per_cluster && wrote < len; s++) {
            uint32_t n = len - wrote, i;
            if (n > SECTOR_SIZE) {
                n = SECTOR_SIZE;
            }
            for (i = 0; i < SECTOR_SIZE; i++) {
                g_fatbuf[i] = (i < n) ? in[wrote + i] : 0u;
            }
            if (vibeos_x86_64_virtio_blk_write(fat_cluster_lba(cl) + s, g_fatbuf) != 0) {
                return -1;
            }
            wrote += n;
        }
        cl = fat_get_entry(cl);
    }

    /* Re-read the directory sector (FAT I/O reused the buffer) and store the
     * entry. */
    if (vibeos_x86_64_virtio_blk_read(lba, g_secbuf) != 0) {
        return -1;
    }
    {
        uint8_t *d = &g_secbuf[off];
        int k;
        for (k = 0; k < 32; k++) {
            d[k] = 0;
        }
        for (k = 0; k < 11; k++) {
            d[k] = want[k];
        }
        d[11] = 0x20;                                  /* archive */
        wr16(&d[20], (uint16_t)(first >> 16));         /* cluster high (FAT32) */
        wr16(&d[26], (uint16_t)(first & 0xFFFFu));     /* cluster low          */
        wr32(&d[28], len);
        if (vibeos_x86_64_virtio_blk_write(lba, g_secbuf) != 0) {
            return -1;
        }
    }
    return (long)len;
}

/* Delete a file: free its clusters and mark the directory slot deleted. */
static int fat_unlink_locked(const char *path) {
    uint8_t want[11];
    uint32_t dir_cluster = 0, lba = 0, off = 0, cluster;

    if (!g_fat.mounted || fat_split_parent(path, &dir_cluster, want) != 0) {
        return -1;
    }
    if (fat_dir_slot(dir_cluster, want, 0, &lba, &off) != 0) {
        return -1;
    }
    {
        uint8_t *d = &g_secbuf[off];
        if (d[11] & 0x10u) {
            return -1; /* use rmdir semantics for directories */
        }
        cluster = ((uint32_t)rd16(&d[20]) << 16) | rd16(&d[26]);
        d[0] = 0xE5;
        if (vibeos_x86_64_virtio_blk_write(lba, g_secbuf) != 0) {
            return -1;
        }
    }
    if (cluster >= 2u) {
        fat_free_chain(cluster);
    }
    return 0;
}

/* Create a directory: one cluster holding the "." and ".." entries. */
static int fat_mkdir_locked(const char *path) {
    uint8_t want[11];
    uint32_t dir_cluster = 0, lba = 0, off = 0, cluster;
    uint32_t s, i;

    if (!g_fat.mounted || fat_split_parent(path, &dir_cluster, want) != 0) {
        return -1;
    }
    if (fat_dir_slot(dir_cluster, want, 1, &lba, &off) != 1) {
        return -1; /* exists, or no free slot */
    }
    cluster = fat_alloc_chain(1);
    if (cluster == 0) {
        return -1;
    }
    /* Zero the cluster, then lay down "." and "..". */
    for (s = 0; s < g_fat.sectors_per_cluster; s++) {
        for (i = 0; i < SECTOR_SIZE; i++) {
            g_fatbuf[i] = 0;
        }
        if (s == 0) {
            for (i = 0; i < 11u; i++) {
                g_fatbuf[i] = ' ';
                g_fatbuf[32u + i] = ' ';
            }
            g_fatbuf[0] = '.';
            g_fatbuf[11] = 0x10;
            wr16(&g_fatbuf[26], (uint16_t)cluster);
            g_fatbuf[32] = '.';
            g_fatbuf[33] = '.';
            g_fatbuf[43] = 0x10;
            wr16(&g_fatbuf[58], (uint16_t)dir_cluster);
        }
        if (vibeos_x86_64_virtio_blk_write(fat_cluster_lba(cluster) + s, g_fatbuf) != 0) {
            return -1;
        }
    }
    if (vibeos_x86_64_virtio_blk_read(lba, g_secbuf) != 0) {
        return -1;
    }
    {
        uint8_t *d = &g_secbuf[off];
        int k;
        for (k = 0; k < 32; k++) {
            d[k] = 0;
        }
        for (k = 0; k < 11; k++) {
            d[k] = want[k];
        }
        d[11] = 0x10;                              /* directory */
        wr16(&d[20], (uint16_t)(cluster >> 16));
        wr16(&d[26], (uint16_t)(cluster & 0xFFFFu));
        wr32(&d[28], 0);
        if (vibeos_x86_64_virtio_blk_write(lba, g_secbuf) != 0) {
            return -1;
        }
    }
    return 0;
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
    return vibeos_x86_64_virtio_blk_read_many(lba, dst, sectors);
}

static int fat_io_partial(void *ctx, uint32_t lba, void *dst, uint32_t bytes) {
    uint8_t *d = (uint8_t *)dst;
    uint32_t i;
    (void)ctx;
    /* Through the bounce buffer, so a partial sector never writes past the end
     * of what the caller asked for. */
    if (vibeos_x86_64_virtio_blk_read(lba, g_secbuf) != 0) {
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

    if (!g_fat.mounted || fat_resolve(path, &cluster, &size) != 0) {
        return -1;
    }
    if (size > bufcap) {
        return -1;
    }
    io.ctx = 0;
    io.cluster_bytes = (uint32_t)g_fat.sectors_per_cluster * SECTOR_SIZE;
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

static volatile int g_fs_lock;

static void fs_lock(void) {
    while (__sync_lock_test_and_set(&g_fs_lock, 1)) {
        while (g_fs_lock) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
}

static void fs_unlock(void) {
    __sync_lock_release(&g_fs_lock);
}

int vibeos_x86_64_fat_mount(void) {
    int r;
    fs_lock();
    r = fat_mount_locked();
    fs_unlock();
    return r;
}

int vibeos_x86_64_fat_open(const char *path, uint32_t *out_cluster, uint32_t *out_size) {
    int r;
    fs_lock();
    r = fat_open_locked(path, out_cluster, out_size);
    fs_unlock();
    return r;
}

long vibeos_x86_64_fat_read_at(uint32_t first_cluster, uint32_t size, uint32_t off,
                               void *buf, uint32_t len) {
    long r;
    fs_lock();
    r = fat_read_at_locked(first_cluster, size, off, buf, len);
    fs_unlock();
    return r;
}

int vibeos_x86_64_fat_list(const char *path, uint32_t idx, char *name, uint32_t *out_size,
                           int *out_is_dir) {
    int r;
    fs_lock();
    r = fat_list_locked(path, idx, name, out_size, out_is_dir);
    fs_unlock();
    return r;
}

long vibeos_x86_64_fat_write_file(const char *path, const void *buf, uint32_t len) {
    long r;
    fs_lock();
    r = fat_write_file_locked(path, buf, len);
    fs_unlock();
    return r;
}

int vibeos_x86_64_fat_unlink(const char *path) {
    int r;
    fs_lock();
    r = fat_unlink_locked(path);
    fs_unlock();
    return r;
}

int vibeos_x86_64_fat_mkdir(const char *path) {
    int r;
    fs_lock();
    r = fat_mkdir_locked(path);
    fs_unlock();
    return r;
}

long vibeos_x86_64_fat_read_file(const char *path, void *buf, uint32_t bufcap) {
    long r;
    fs_lock();
    r = fat_read_file_locked(path, buf, bufcap);
    fs_unlock();
    return r;
}
