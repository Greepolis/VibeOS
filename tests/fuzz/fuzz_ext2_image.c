/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fuzz the real ext2/ext4 metadata reader and writer using a bounded in-memory
 * block device. Mount state, cache slots and disk bytes are reset per input. */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vibeos/blockdev.h"
#include "vibeos/ext2.h"
#include "vibeos/vfs.h"

#define DISK_SECTORS 16384u
#define CACHE_SLOTS 32u
#define MAX_INPUT (DISK_SECTORS * VIBEOS_BLOCK_SIZE)

void ext4_w_mount(vibeos_ext2_t *fs);
const vibeos_fs_ops_t *ext4_ops(const vibeos_ext2_t *fs);

static uint8_t g_disk[MAX_INPUT];
static vibeos_ext2_t g_fs;
static vibeos_blockcache_t g_cache;
static vibeos_blockdev_t g_dev;
static vibeos_block_slot_t g_slots[CACHE_SLOTS];
static uint8_t g_slot_data[CACHE_SLOTS][VIBEOS_BLOCK_SIZE];
static uint8_t g_read_buf[4096];
static char g_name[VIBEOS_FS_NAME_MAX];

#ifdef VIBEOS_EXT2_FUZZ_CANARY
/* wbuf2 is a fixed 4 KiB production scratch buffer. The adjacent test-only
 * red zone observes writes past it without calling private writer helpers. */
static void bitmap_redzone_arm(vibeos_ext2_t *fs) {
    memset(fs->fuzz_wbuf2_guard, 0, sizeof(fs->fuzz_wbuf2_guard));
}

static void bitmap_redzone_check(const vibeos_ext2_t *fs) {
    size_t i;

    for (i = 0; i < sizeof(fs->fuzz_wbuf2_guard); i++) {
        if (fs->fuzz_wbuf2_guard[i] != 0u) {
            fputs("BITMAP_REDZONE_CORRUPTION\n", stderr);
            __builtin_trap();
        }
    }
}
#endif

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
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    const vibeos_fs_ops_t *ops;
    vibeos_fs_node_t root, node;
    uint64_t entry_size;
    int is_dir;
    uint32_t i;

    if (!data || size < 2048u || size > MAX_INPUT) {
        return 0;
    }

    memset(g_disk, 0, sizeof(g_disk));
    memcpy(g_disk, data, size);
    memset(&g_fs, 0, sizeof(g_fs));
    memset(&g_cache, 0, sizeof(g_cache));
    memset(&g_dev, 0, sizeof(g_dev));
    g_dev.read = disk_read;
    g_dev.write = disk_write;
    g_dev.sectors = DISK_SECTORS;
    for (i = 0; i < CACHE_SLOTS; i++) {
        g_slots[i].data = g_slot_data[i];
    }
    if (vibeos_blockcache_init(&g_cache, &g_dev, g_slots, CACHE_SLOTS) != 0) {
        return 0;
    }

    if (vibeos_ext2_mount(&g_fs, &g_cache, 0u) != 0) {
        return 0;
    }
    ops = vibeos_ext2_ops();
    if (!ops || !ops->lookup || !ops->list || !ops->read_at) {
        return 0;
    }

    /* Root lookup reaches the inode table and directory block parsers. */
    if (ops->lookup(&g_fs, "/", &root) != 0 || !root.is_dir) {
        return 0;
    }
    for (i = 0; i < 16u; i++) {
        if (ops->list(&g_fs, "/", i, g_name, sizeof(g_name), &entry_size,
                      &is_dir) != 0) {
            break;
        }
        if (!is_dir && ops->lookup(&g_fs, g_name, &node) == 0) {
            uint32_t take = node.size < sizeof(g_read_buf)
                                ? (uint32_t)node.size
                                : (uint32_t)sizeof(g_read_buf);
            (void)ops->read_at(&g_fs, &node, 0u, g_read_buf, take);
            if (take > 1u) {
                (void)ops->read_at(&g_fs, &node, take / 2u,
                                   g_read_buf, take / 2u);
            }
        }
    }

    /* Exercise the public filesystem writer path. The #202 negative control
     * removes the mount-time geometry bound; a malformed image must then
     * reach bitmap construction through write_file(), not a test-only hook. */
    ext4_w_mount(&g_fs);
    ops = ext4_ops(&g_fs);
    if (ops && ops->write_file) {
        static const uint8_t payload[512] = {
            0x56, 0x69, 0x62, 0x65, 0x4f, 0x53, 0x20, 0x66,
            0x75, 0x7a, 0x7a, 0x20, 0x77, 0x72, 0x69, 0x74
        };
#ifdef VIBEOS_EXT2_FUZZ_CANARY
        bitmap_redzone_arm(&g_fs);
#endif
        (void)ops->write_file(&g_fs, "/fuzz-output.bin", payload,
                              sizeof(payload));
#ifdef VIBEOS_EXT2_FUZZ_CANARY
        /* Check after the complete production operation, irrespective of its
         * return code: an error must not hide an earlier scratch-buffer OOB. */
        bitmap_redzone_check(&g_fs);
#endif
    }
    return 0;
}
