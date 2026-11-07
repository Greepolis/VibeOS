/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fuzz the real ext2/ext4 metadata reader using a bounded in-memory block
 * device. Mount state, cache slots and disk bytes are reset for every input. */

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "vibeos/blockdev.h"
#include "vibeos/ext2.h"
#include "vibeos/vfs.h"

#define DISK_SECTORS 16384u
#define CACHE_SLOTS 32u
#define MAX_INPUT (DISK_SECTORS * VIBEOS_BLOCK_SIZE)

/* The writer is intentionally exercised too: the #202 bitmap bound protects
 * its fixed-size scratch buffer, not just the read-only mount path. */
void ext4_w_mount(vibeos_ext2_t *fs);
const vibeos_fs_ops_t *ext4_ops(const vibeos_ext2_t *fs);
#ifdef VIBEOS_EXT2_FUZZ_CANARY
int vibeos_ext4_fuzz_build_block_bitmap(vibeos_ext2_t *fs, uint32_t group);
#endif

static uint8_t g_disk[MAX_INPUT];
static vibeos_ext2_t g_fs;
static vibeos_blockcache_t g_cache;
static vibeos_blockdev_t g_dev;
static vibeos_block_slot_t g_slots[CACHE_SLOTS];
static uint8_t g_slot_data[CACHE_SLOTS][VIBEOS_BLOCK_SIZE];
static uint8_t g_read_buf[4096];
static char g_name[VIBEOS_FS_NAME_MAX];

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

    {
        int mount_result = vibeos_ext2_mount(&g_fs, &g_cache, 0u);
        if (mount_result != 0) {
            if (size >= 2048u && data[1080] == 0x53u && data[1081] == 0xEFu) {
                uint32_t bpg = (uint32_t)data[1056] |
                              ((uint32_t)data[1057] << 8) |
                              ((uint32_t)data[1058] << 16) |
                              ((uint32_t)data[1059] << 24);
                fprintf(stderr, "ext4-geometry: mount rejected rc=%d log_bs=%u bpg=%u\n",
                        mount_result, data[1048], bpg);
            }
            return 0;
        }
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
        if (ops->list(&g_fs, "/", i, g_name, sizeof(g_name),
                      &entry_size, &is_dir) != 0) {
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

    /* ext4's production writer builds and pads allocation bitmaps in fs->wbuf2.
     * This reaches the #202 geometry invariant that a read-only mount alone
     * cannot exercise. The fuzz input is private to this testcase. */
    ext4_w_mount(&g_fs);
    ops = ext4_ops(&g_fs);
    if (g_fs.blocks_per_group > g_fs.block_size * 8u) {
        fprintf(stderr, "ext4-geometry: writable=%d block=%u bpg=%u ipg=%u inode=%u desc=%u csum=%u incompat=%x ro=%x write=%d\n",
                g_fs.writable, g_fs.block_size, g_fs.blocks_per_group,
                g_fs.inodes_per_group, g_fs.inode_size, g_fs.desc_size,
                g_fs.csum, g_fs.incompat, g_fs.ro_compat,
                g_fs.cache->dev->write != 0);
#ifdef VIBEOS_EXT2_FUZZ_CANARY
        /* build_block_bitmap is the production helper that pads the short
         * group's bitmap to blocks_per_group. With 32769 bits it sets bit
         * zero in the byte immediately after wbuf2. */
        g_fs.fuzz_wbuf2_guard[0] = 0xFEu;
        if (vibeos_ext4_fuzz_build_block_bitmap(&g_fs, 0u) != 0) {
            fprintf(stderr, "ext4-geometry: bitmap builder rejected group\n");
        } else {
            fprintf(stderr, "ext4-geometry: bitmap redzone=%u\n",
                    g_fs.fuzz_wbuf2_guard[0]);
            if (g_fs.fuzz_wbuf2_guard[0] != 0xFEu) {
                __builtin_trap();
            }
        }
#endif
    }
    if (ops && ops->write_file) {
        static const uint8_t payload[512] = {
            0x56, 0x69, 0x62, 0x65, 0x4f, 0x53, 0x20, 0x66,
            0x75, 0x7a, 0x7a, 0x20, 0x77, 0x72, 0x69, 0x74
        };
        long write_result = ops->write_file(&g_fs, "/fuzz-output.bin",
                                                payload, sizeof(payload));
        if (g_fs.blocks_per_group > g_fs.block_size * 8u) {
            fprintf(stderr, "ext4-geometry: write_file=%ld redzone=%u\n",
                    write_result, g_fs.fuzz_wbuf2_guard[0]);
        }
    }
    return 0;
}
