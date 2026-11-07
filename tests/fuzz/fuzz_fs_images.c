/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Coverage-guided fuzzing for the filesystem drivers ext2 does not cover -
 * FAT, exFAT, NTFS, ISO9660 - and for the partition tables, issue #199.
 *
 * One target rather than five, the way fuzz_exec_images offers every input to
 * both executable parsers: each input is a disk image, and every driver is
 * asked whether it can mount it. A mutation that turns a FAT image into
 * something exFAT half-accepts is coverage too, and the corpus stays one
 * directory of images real tools wrote (make_seed_corpus.py).
 *
 * What a successful mount is driven through is the same for every driver -
 * the generic operations a VFS caller uses: the root, a listing, a lookup of
 * each regular file found, two reads of it, and one write on the drivers that
 * write. Mount state, cache slots and disk bytes are reset for every input. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vibeos/blockdev.h"
#include "vibeos/exfat.h"
#include "vibeos/fat.h"
#include "vibeos/iso9660.h"
#include "vibeos/ntfs.h"
#include "vibeos/partition.h"
#include "vibeos/vfs.h"

#define DISK_SECTORS 16384u                    /* 8 MiB */
#define CACHE_SLOTS 32u
#define MAX_INPUT (DISK_SECTORS * VIBEOS_BLOCK_SIZE)

static uint8_t g_disk[MAX_INPUT];
static size_t g_dirty_end = MAX_INPUT;   /* whole disk unknown at the start */
static vibeos_blockcache_t g_cache;
static vibeos_blockdev_t g_dev;
static vibeos_block_slot_t g_slots[CACHE_SLOTS];
static uint8_t g_slot_data[CACHE_SLOTS][VIBEOS_BLOCK_SIZE];
static uint8_t g_read_buf[4096];
static char g_name[VIBEOS_FS_NAME_MAX];
static vibeos_exfat_t g_exfat;
static vibeos_ntfs_t g_ntfs;
static vibeos_iso9660_t g_iso;
static vibeos_parttable_t g_table;

static const uint8_t g_payload[512] = {
    0x56, 0x69, 0x62, 0x65, 0x4f, 0x53, 0x20, 0x66,
    0x75, 0x7a, 0x7a, 0x20, 0x77, 0x72, 0x69, 0x74
};

static int disk_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= DISK_SECTORS) {
        return -1;
    }
    memcpy(buf, g_disk + lba * VIBEOS_BLOCK_SIZE, VIBEOS_BLOCK_SIZE);
    return 0;
}

static int disk_write(void *ctx, uint64_t lba, const void *buf) {
    (void)ctx;
    if (lba >= DISK_SECTORS) {
        return -1;
    }
    memcpy(g_disk + lba * VIBEOS_BLOCK_SIZE, buf, VIBEOS_BLOCK_SIZE);
    if ((lba + 1u) * VIBEOS_BLOCK_SIZE > g_dirty_end) {
        g_dirty_end = (lba + 1u) * VIBEOS_BLOCK_SIZE;
    }
    return 0;
}

/* The input again, and zeros past it as far as anything was ever there: the
 * previous driver may have written. Clearing all 8 MiB four times an input
 * would spend the fuzzer's time on memset. */
static void load(const uint8_t *data, size_t size) {
    if (g_dirty_end > size) {
        memset(g_disk + size, 0, g_dirty_end - size);
    }
    memcpy(g_disk, data, size);
    g_dirty_end = size;
}

/* A fresh cache over the current disk bytes: a driver that wrote must not
 * hand the next driver its cached sectors. */
static int fresh_cache(void) {
    uint32_t i;

    memset(&g_cache, 0, sizeof(g_cache));
    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.read = disk_read;
    g_dev.write = disk_write;
    g_dev.sectors = DISK_SECTORS;
    for (i = 0; i < CACHE_SLOTS; i++) {
        g_slots[i].data = g_slot_data[i];
    }
    return vibeos_blockcache_init(&g_cache, &g_dev, g_slots, CACHE_SLOTS);
}

/* What a VFS caller does with a mounted volume. */
static void drive(void *fs, const vibeos_fs_ops_t *ops) {
    vibeos_fs_node_t root, node;
    uint64_t entry_size;
    int is_dir;
    uint32_t i;

    if (!ops || !ops->lookup || !ops->list || !ops->read_at) {
        return;
    }
    if (ops->lookup(fs, "/", &root) != 0) {
        return;
    }
    for (i = 0; i < 16u; i++) {
        if (ops->list(fs, "/", i, g_name, sizeof(g_name), &entry_size,
                      &is_dir) != 0) {
            break;
        }
        if (!is_dir && ops->lookup(fs, g_name, &node) == 0) {
            uint32_t take = node.size < sizeof(g_read_buf)
                                ? (uint32_t)node.size
                                : (uint32_t)sizeof(g_read_buf);
            (void)ops->read_at(fs, &node, 0u, g_read_buf, take);
            if (take > 1u) {
                (void)ops->read_at(fs, &node, take / 2u, g_read_buf, take / 2u);
            }
        }
    }
    if (ops->write_file) {
        (void)ops->write_file(fs, "/fuzz-output.bin", g_payload,
                              sizeof(g_payload));
    }
}

/* FAT is reached through its own handle API, not vibeos_fs_ops_t. */
static void drive_fat(void) {
    void *vol;
    uint32_t i, size, cluster;
    int is_dir;

    vibeos_fat_forget_volumes();
    vol = vibeos_fat_mount_volume(&g_cache, 0u, DISK_SECTORS);
    if (!vol) {
        return;
    }
    for (i = 0; i < 16u; i++) {
        if (vibeos_fat_list_on(vol, "/", i, g_name, sizeof(g_name), &size,
                               &is_dir) != 0) {
            break;
        }
        if (!is_dir && vibeos_fat_open_on(vol, g_name, &cluster, &size) == 0) {
            uint32_t take = size < sizeof(g_read_buf)
                                ? size : (uint32_t)sizeof(g_read_buf);
            (void)vibeos_fat_read_at_on(vol, cluster, size, 0u, g_read_buf, take);
        }
    }
    (void)vibeos_fat_write_file_on(vol, "/FUZZOUT.BIN", g_payload,
                                   sizeof(g_payload));
    (void)vibeos_fat_mkdir_on(vol, "/FUZZDIR");
    vibeos_fat_forget_volumes();
}

static void parse_tables(const uint8_t *data, size_t size) {
    int protective = 0;

    memset(&g_table, 0, sizeof(g_table));
    (void)vibeos_partition_parse_mbr(data, &g_table, &protective);
    /* LBA 1 is the GPT header; the entry array sits from LBA 2 in every
     * table a real tool writes, and its length is the rest of the input. */
    if (size >= 3u * 512u) {
        size_t len = size - 2u * 512u;
        if (len > 128u * 128u) {
            len = 128u * 128u;
        }
        memset(&g_table, 0, sizeof(g_table));
        (void)vibeos_partition_parse_gpt(data + 512u, data + 1024u,
                                         (uint32_t)len, DISK_SECTORS, &g_table);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (!data || size < 512u || size > MAX_INPUT) {
        return 0;
    }
    parse_tables(data, size);


    load(data, size);
    if (fresh_cache() == 0) {
        drive_fat();
    }

    load(data, size);
    if (fresh_cache() == 0) {
        memset(&g_exfat, 0, sizeof(g_exfat));
        if (vibeos_exfat_mount(&g_exfat, &g_cache, 0u) == 0) {
            drive(&g_exfat, vibeos_exfat_ops());
        }
    }

    load(data, size);
    if (fresh_cache() == 0) {
        memset(&g_ntfs, 0, sizeof(g_ntfs));
        if (vibeos_ntfs_mount(&g_ntfs, &g_cache, 0u) == 0) {
            drive(&g_ntfs, vibeos_ntfs_ops());
        }
    }

    load(data, size);
    if (fresh_cache() == 0) {
        memset(&g_iso, 0, sizeof(g_iso));
        if (vibeos_iso9660_mount(&g_iso, &g_cache, 0u) == 0) {
            drive(&g_iso, vibeos_iso9660_ops());
        }
    }
    return 0;
}
