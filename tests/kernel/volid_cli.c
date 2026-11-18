/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The storage scan on an image file, saying what identifies each volume it
 * mounted (hierarchy F2 step 1): one line a volume,
 *     fs=<name> uuid=<uuid> label=<label>
 * which scripts/dev/verify-volid.py holds against blkid's answer for the same
 * image. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/fat.h"
#include "vibeos/storage.h"

/* Each driver's accessor, declared where it is used, as the host tests do. */
const vibeos_fs_driver_t *vibeos_ext2_fs_driver(void);
const vibeos_fs_driver_t *vibeos_ext4_fs_driver(void);
const vibeos_fs_driver_t *vibeos_ntfs_fs_driver(void);
const vibeos_fs_driver_t *vibeos_exfat_fs_driver(void);
const vibeos_fs_driver_t *vibeos_iso9660_fs_driver(void);

#define SLOTS 64u

static uint8_t *g_img;
static uint64_t g_sectors;
static uint8_t g_slot_data[SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_slots[SLOTS];
static vibeos_blockdev_t g_dev;
static vibeos_blockcache_t g_bc;
static vibeos_storage_t g_st;

static int img_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= g_sectors) {
        return -1;
    }
    memcpy(buf, g_img + lba * VIBEOS_BLOCK_SIZE, VIBEOS_BLOCK_SIZE);
    return 0;
}

int main(int argc, char **argv) {
    FILE *f;
    long n;
    uint32_t i;

    if (argc != 2 || !(f = fopen(argv[1], "rb")) || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) <= 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fprintf(stderr, "usage: vibeos_volid_cli <image>\n");
        return 2;
    }
    g_img = (uint8_t *)malloc((size_t)n);
    if (!g_img || fread(g_img, 1, (size_t)n, f) != (size_t)n) {
        return 2;
    }
    fclose(f);
    g_sectors = (uint64_t)n / VIBEOS_BLOCK_SIZE;
    for (i = 0; i < SLOTS; i++) {
        g_slots[i].data = g_slot_data[i];
    }
    g_dev.read = img_read;
    g_dev.sectors = g_sectors;
    if (vibeos_blockcache_init(&g_bc, &g_dev, g_slots, SLOTS) != 0) {
        return 2;
    }
    vibeos_storage_reset_drivers();
    (void)vibeos_storage_register(vibeos_ntfs_fs_driver());
    (void)vibeos_storage_register(vibeos_exfat_fs_driver());
    (void)vibeos_storage_register(vibeos_ext4_fs_driver());
    (void)vibeos_storage_register(vibeos_ext2_fs_driver());
    (void)vibeos_storage_register(vibeos_iso9660_fs_driver());
    (void)vibeos_storage_register(vibeos_fat_fs_driver());
    if (vibeos_storage_scan(&g_st, &g_bc, g_sectors) != 0) {
        fprintf(stderr, "scan failed\n");
        return 1;
    }
    for (i = 0; i < g_st.volume_count; i++) {
        if (g_st.volume[i].fs_name) {
            printf("fs=%s uuid=%s label=%s\n", g_st.volume[i].fs_name, g_st.volume[i].id.uuid,
                   g_st.volume[i].id.label);
        }
    }
    return 0;
}
