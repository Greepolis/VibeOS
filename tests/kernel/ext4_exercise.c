/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The ext4 reader, on a volume mkfs.ext4 made (docs/filesystem-design/ext4.md
 * step 1).
 *
 *     vibeos_ext4_exercise walk                (in the directory that holds fs.img)
 *     vibeos_ext4_exercise refused-file <path> (a path on the volume)
 *     vibeos_ext4_exercise refused-mount
 *
 * scripts/dev/verify-ext4.sh builds a tree on the host, has mkfs.ext4 -d put
 * it on a volume, and holds what this prints against the tree itself: "walk"
 * lists every directory and file under the root, one line each - the path,
 * D or F, the size, and the crc32 of the bytes read through the driver - and
 * the script lists the host's tree the same way. Neither side wrote the
 * volume; mkfs.ext4 did.
 *
 * The other two modes are the refusals, on a volume the script has damaged
 * with debugfs: a file whose inode checksum no longer matches must not be
 * read, and counts as a checksum failure; a volume that claims a feature this
 * driver does not read must not mount, and names the feature bits. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/ext2.h"
#include "vibeos/vfs.h"

#define SLOTS 64u

static uint8_t *g_img;
static uint64_t g_sectors;
static vibeos_blockdev_t g_dev;
static vibeos_blockcache_t g_bc;
static vibeos_block_slot_t g_slots[SLOTS];
static uint8_t g_slot_data[SLOTS][512];
static vibeos_ext2_t g_fs;
static const vibeos_fs_ops_t *g_ops;

static int img_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= g_sectors) {
        return -1;
    }
    memcpy(buf, g_img + lba * 512u, 512);
    return 0;
}

static int img_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    (void)lba;
    (void)buf;
    return -1;   /* read-only: step 1 has no writer, and a write would be a defect */
}

/* zlib's crc32, which the script's Python computes the same way. */
static uint32_t crc32_of(const uint8_t *p, uint64_t n, uint32_t crc) {
    crc = ~crc;
    while (n-- > 0u) {
        uint32_t k;

        crc ^= *p++;
        for (k = 0; k < 8u; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static int load(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;

    if (!f || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET) != 0) {
        return -1;
    }
    g_img = (uint8_t *)malloc((size_t)n);
    if (!g_img || fread(g_img, 1, (size_t)n, f) != (size_t)n) {
        return -1;
    }
    fclose(f);
    g_sectors = (uint64_t)n / 512u;
    for (uint32_t i = 0; i < SLOTS; i++) {
        g_slots[i].data = g_slot_data[i];
    }
    g_dev.read = img_read;
    g_dev.write = img_write;
    g_dev.sectors = g_sectors;
    return vibeos_blockcache_init(&g_bc, &g_dev, g_slots, SLOTS);
}

/* A file's crc32, read in uneven pieces so that a read starting or ending
 * inside a block is exercised, not only whole blocks. -1 if a read failed. */
static long long file_crc(const char *path, uint64_t size) {
    static uint8_t buf[70000];
    vibeos_fs_node_t node;
    uint64_t at = 0;
    uint32_t crc = 0, piece = 4095u;

    if (g_ops->lookup(&g_fs, path, &node) != 0 || node.size != size) {
        return -1;
    }
    while (at < size) {
        long got = g_ops->read_at(&g_fs, &node, at, buf, piece);

        if (got <= 0) {
            return -1;
        }
        crc = crc32_of(buf, (uint64_t)got, crc);
        at += (uint64_t)got;
        piece = piece == 4095u ? 65537u : 4095u;
    }
    return crc;
}

static int walk(const char *dir) {
    char name[256], path[1024];
    uint64_t size;
    int is_dir, bad = 0;

    for (uint32_t i = 0; g_ops->list(&g_fs, dir, i, name, sizeof(name), &size, &is_dir) == 0; i++) {
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strcmp(name, "lost+found") == 0) {
            continue;
        }
        snprintf(path, sizeof(path), "%s%s%s", dir, strcmp(dir, "/") == 0 ? "" : "/", name);
        if (is_dir) {
            printf("%s\tD\n", path);
            bad |= walk(path);
        } else {
            long long crc = file_crc(path, size);

            if (crc < 0) {
                fprintf(stderr, "FAIL:ext4_exercise cannot read %s\n", path);
                bad = 1;
                continue;
            }
            printf("%s\tF\t%llu\t%08llx\n", path, (unsigned long long)size, crc);
        }
    }
    return bad;
}

int main(int argc, char **argv) {
    vibeos_fsmount_t m;

    /* fs.img in the working directory, as fat_exercise does: a program
     * that opened a path from its arguments is a path-injection finding for
     * code scanning, and a test has no reason to take one. */
    if (argc < 2 || load("fs.img") != 0) {
        fprintf(stderr, "usage: vibeos_ext4_exercise walk|refused-file <path>|refused-mount, beside fs.img\n");
        return 2;
    }
    g_ops = vibeos_ext2_ops();
    if (strcmp(argv[1], "refused-mount") == 0) {
        if (vibeos_ext2_mount(&g_fs, &g_bc, 0) == 0) {
            fprintf(stderr, "FAIL:ext4_exercise mounted a volume with a feature it does not read\n");
            return 1;
        }
        printf("refused features=0x%x\n", vibeos_ext2_last_refused_features());
        return vibeos_ext2_last_refused_features() != 0u ? 0 : 1;
    }
    if (vibeos_ext2_mount(&g_fs, &g_bc, 0) != 0 || vibeos_fs_mount(&m, g_ops, &g_fs, "ext4") != 0) {
        fprintf(stderr, "FAIL:ext4_exercise cannot mount what mkfs.ext4 made (refused=0x%x)\n",
                vibeos_ext2_last_refused_features());
        return 1;
    }
    if (!g_fs.csum) {
        fprintf(stderr, "FAIL:ext4_exercise the volume has no metadata_csum: nothing would be checked\n");
        return 1;
    }
    if (strcmp(argv[1], "walk") == 0) {
        int bad = walk("/");

        if (vibeos_ext2_csum_failures() != 0u) {
            fprintf(stderr, "FAIL:ext4_exercise %llu checksum failures on a healthy volume\n",
                    (unsigned long long)vibeos_ext2_csum_failures());
            bad = 1;
        }
        return bad;
    }
    if (strcmp(argv[1], "refused-file") == 0 && argc == 3) {
        vibeos_fs_node_t node;

        if (g_ops->lookup(&g_fs, argv[2], &node) == 0) {
            fprintf(stderr, "FAIL:ext4_exercise read %s through a checksum that does not match\n", argv[2]);
            return 1;
        }
        printf("refused csum_failures=%llu\n", (unsigned long long)vibeos_ext2_csum_failures());
        return vibeos_ext2_csum_failures() != 0u ? 0 : 1;
    }
    return 2;
}
