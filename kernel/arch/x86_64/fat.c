/* Read-only FAT16/FAT32 reader over the virtio-blk driver (image-only).
 *
 * Parses an MBR to find the first FAT partition, reads its BPB, and can look up
 * an 8.3 file in the root directory and read its contents. Enough to load user
 * programs from a real filesystem on a real block device.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

extern int vibeos_x86_64_virtio_blk_read(uint64_t sector, void *buf);

#define SECTOR_SIZE 512u

typedef struct {
    uint32_t part_lba;        /* partition start sector           */
    uint32_t fat_lba;         /* first FAT sector                 */
    uint32_t root_lba;        /* FAT16 root dir sector (0 if F32) */
    uint32_t data_lba;        /* first data sector                */
    uint32_t sectors_per_fat;
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

int vibeos_x86_64_fat_mount(void) {
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
    (void)total_sectors;

    root_dir_sectors = ((uint32_t)g_fat.root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
    g_fat.fat_lba = part_lba + reserved;
    g_fat.root_lba = g_fat.fat_lba + 2u * g_fat.sectors_per_fat;         /* FAT16 root */
    g_fat.data_lba = g_fat.root_lba + root_dir_sectors;                  /* first data */

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

/* Follow the FAT chain: return the next cluster, or >= EOC when the chain ends. */
static uint32_t fat_next_cluster(uint32_t cluster) {
    if (g_fat.is_fat32) {
        uint32_t off = cluster * 4u;
        if (vibeos_x86_64_virtio_blk_read(g_fat.fat_lba + off / SECTOR_SIZE, g_secbuf) != 0) {
            return 0x0FFFFFFFu;
        }
        return rd32(&g_secbuf[off % SECTOR_SIZE]) & 0x0FFFFFFFu;
    } else {
        uint32_t off = cluster * 2u;
        if (vibeos_x86_64_virtio_blk_read(g_fat.fat_lba + off / SECTOR_SIZE, g_secbuf) != 0) {
            return 0xFFFFu;
        }
        return rd16(&g_secbuf[off % SECTOR_SIZE]);
    }
}

static int fat_chain_end(uint32_t cluster) {
    return g_fat.is_fat32 ? (cluster >= 0x0FFFFFF8u) : (cluster >= 0xFFF8u);
}

/* Build the 11-byte 8.3 on-disk name from "NAME.EXT". */
static void fat_make_83(const char *name, uint8_t out[11]) {
    int i = 0, o = 0;
    for (o = 0; o < 11; o++) {
        out[o] = ' ';
    }
    for (o = 0; o < 8 && name[i] && name[i] != '.'; i++, o++) {
        out[o] = (uint8_t)name[i];
    }
    while (name[i] && name[i] != '.') {
        i++;
    }
    if (name[i] == '.') {
        i++;
    }
    for (o = 8; o < 11 && name[i]; i++, o++) {
        out[o] = (uint8_t)name[i];
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

    if (dir_cluster == 0 && !g_fat.is_fat32) {
        uint32_t root_sectors = ((uint32_t)g_fat.root_entries * 32u + (SECTOR_SIZE - 1u)) / SECTOR_SIZE;
        return fat_scan_sectors(g_fat.root_lba, root_sectors, want, out_cluster, out_size, out_attr);
    }
    cl = (dir_cluster == 0) ? g_fat.root_cluster : dir_cluster;
    while (!fat_chain_end(cl) && cl >= 2u) {
        if (fat_scan_sectors(fat_cluster_lba(cl), g_fat.sectors_per_cluster,
                             want, out_cluster, out_size, out_attr) == 0) {
            return 0;
        }
        cl = fat_next_cluster(cl);
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

/* Read a whole file by path (e.g. "EFI/BOOT/INIT.ELF") into buf (up to bufcap).
 * Returns the file size, or -1 on error / too big. */
long vibeos_x86_64_fat_read_file(const char *path, void *buf, uint32_t bufcap) {
    uint32_t cluster = 0, size = 0, copied = 0;
    uint8_t *out = (uint8_t *)buf;

    if (!g_fat.mounted || fat_resolve(path, &cluster, &size) != 0) {
        return -1;
    }
    if (size > bufcap) {
        return -1;
    }
    while (!fat_chain_end(cluster) && cluster >= 2u && copied < size) {
        uint32_t s;
        for (s = 0; s < g_fat.sectors_per_cluster && copied < size; s++) {
            uint32_t n, i;
            if (vibeos_x86_64_virtio_blk_read(fat_cluster_lba(cluster) + s, g_secbuf) != 0) {
                return -1;
            }
            n = size - copied;
            if (n > SECTOR_SIZE) {
                n = SECTOR_SIZE;
            }
            for (i = 0; i < n; i++) {
                out[copied + i] = g_secbuf[i];
            }
            copied += n;
        }
        cluster = fat_next_cluster(cluster);
    }
    return (long)size;
}
