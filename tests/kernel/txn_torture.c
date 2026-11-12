/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Write-ahead journal power-cut torture for issue #201.
 *
 * The device model does not make every write durable in one flush. It keeps
 * the writes issued during a flush pending, then lets the test persist any
 * prefix in an arbitrary order before cutting power. Unpersisted writes vanish.
 *
 * The durable commit record is the commit point. After it is durable, recovery
 * must produce the complete new state. A later checkpoint flush must make all
 * targets durable before descriptor retirement; the adversarial retirement
 * order below proves that removing this barrier loses a committed update.
 *
 *   vibeos_txn_torture <seed> [sweeps]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "vibeos/journal.h"

#define DEVICE_BLOCKS 256u
#define JOURNAL_BASE 100u
#define TARGET_A 10u
#define TARGET_B 11u
#define JOURNAL_CAPACITY 4u
#define CACHE_SLOTS 8u
#define PENDING_CAPACITY 8u
#define COMMIT_MAGIC 0x434A4256u

typedef struct {
    uint64_t lba;
    uint8_t bytes[VIBEOS_BLOCK_SIZE];
} pending_write_t;

typedef struct {
    uint8_t durable[DEVICE_BLOCKS][VIBEOS_BLOCK_SIZE];
    pending_write_t pending[PENDING_CAPACITY];
    uint32_t pending_count;
    int32_t cut_flush;
    uint32_t cut_after;
    uint32_t flush_count;
    uint32_t order[PENDING_CAPACITY];
    uint32_t order_count;
} powercut_device_t;

static powercut_device_t g_device;
static uint8_t g_slot_bytes[CACHE_SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_slots[CACHE_SLOTS];
static vibeos_blockdev_t g_blockdev;
static vibeos_blockcache_t g_cache;
static vibeos_journal_t g_journal;
static uint64_t g_targets[2];
static uint8_t g_staging[2u * VIBEOS_BLOCK_SIZE];
static uint64_t g_rng;
static uint64_t g_initial_seed;

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rnd64(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static int device_read(void *ctx, uint64_t lba, void *buf) {
    powercut_device_t *dev = (powercut_device_t *)ctx;
    if (!dev || !buf || lba >= DEVICE_BLOCKS) return -1;
    memcpy(buf, dev->durable[lba], VIBEOS_BLOCK_SIZE);
    return 0;
}

static int device_write(void *ctx, uint64_t lba, const void *buf) {
    powercut_device_t *dev = (powercut_device_t *)ctx;
    pending_write_t *write;
    if (!dev || !buf || lba >= DEVICE_BLOCKS ||
        dev->pending_count >= PENDING_CAPACITY) return -1;
    write = &dev->pending[dev->pending_count++];
    write->lba = lba;
    memcpy(write->bytes, buf, VIBEOS_BLOCK_SIZE);
    return 0;
}

/* A flush is a durability barrier, not an atomic batch. The device may have
 * persisted some writes when power disappears, and it may persist them in an
 * order different from the host's submission order. */
static int device_flush(void *ctx) {
    powercut_device_t *dev = (powercut_device_t *)ctx;
    uint32_t i;
    int cut;
    if (!dev) return -1;
    cut = ((int32_t)dev->flush_count == dev->cut_flush);
    dev->flush_count++;
    if (cut) {
        uint32_t count = dev->cut_after < dev->pending_count
            ? dev->cut_after : dev->pending_count;
        for (i = 0; i < count; i++) {
            uint32_t index = i < dev->order_count ? dev->order[i] : i;
            if (index < dev->pending_count) {
                pending_write_t *write = &dev->pending[index];
                memcpy(dev->durable[write->lba], write->bytes, VIBEOS_BLOCK_SIZE);
            }
        }
        /* A cut discards every write not already persisted. */
        dev->pending_count = 0;
        return -1;
    }
    for (i = 0; i < dev->pending_count; i++) {
        pending_write_t *write = &dev->pending[i];
        memcpy(dev->durable[write->lba], write->bytes, VIBEOS_BLOCK_SIZE);
    }
    dev->pending_count = 0;
    return 0;
}

static void fill_block(uint8_t *block, uint8_t value) {
    memset(block, value, VIBEOS_BLOCK_SIZE);
}

static int attach_journal(void) {
    uint32_t i;
    for (i = 0; i < CACHE_SLOTS; i++) {
        g_slots[i].data = g_slot_bytes[i];
    }
    g_blockdev.read = device_read;
    g_blockdev.write = device_write;
    g_blockdev.flush = device_flush;
    g_blockdev.ctx = &g_device;
    g_blockdev.sectors = DEVICE_BLOCKS;
    if (vibeos_blockcache_init(&g_cache, &g_blockdev, g_slots, CACHE_SLOTS) != 0) {
        return -1;
    }
    return vibeos_journal_init(&g_journal, &g_cache, JOURNAL_BASE,
                              JOURNAL_CAPACITY, g_targets, g_staging);
}

static int run_cut_case(uint32_t seed, unsigned sweep, int32_t cut_flush,
                        uint32_t cut_after, const uint32_t *order,
                        uint32_t order_count, unsigned case_no) {
    uint8_t old_a[VIBEOS_BLOCK_SIZE], old_b[VIBEOS_BLOCK_SIZE];
    uint8_t new_a[VIBEOS_BLOCK_SIZE], new_b[VIBEOS_BLOCK_SIZE];
    uint8_t actual[VIBEOS_BLOCK_SIZE];
    uint32_t i;
    int committed;
    int expected_new;

    memset(&g_device, 0, sizeof(g_device));
    g_device.cut_flush = cut_flush;
    g_device.cut_after = cut_after;
    g_device.order_count = order_count;
    for (i = 0; i < order_count && i < PENDING_CAPACITY; i++) {
        g_device.order[i] = order[i];
    }

    fill_block(old_a, (uint8_t)(0x10u + (seed & 0x1Fu)));
    fill_block(old_b, (uint8_t)(0x40u + (sweep & 0x1Fu)));
    fill_block(new_a, (uint8_t)(0x80u | (rnd64() & 0x7Fu)));
    fill_block(new_b, (uint8_t)(0xC0u | (rnd64() & 0x3Fu)));
    memcpy(g_device.durable[TARGET_A], old_a, sizeof(old_a));
    memcpy(g_device.durable[TARGET_B], old_b, sizeof(old_b));

    if (attach_journal() != 0 ||
        vibeos_journal_begin(&g_journal) != 0 ||
        vibeos_journal_stage(&g_journal, TARGET_A, new_a) != 0 ||
        vibeos_journal_stage(&g_journal, TARGET_B, new_b) != 0) {
        printf("FAIL:txn_torture seed=%llu sweep=%u case=%u: setup\n",
               (unsigned long long)g_initial_seed, sweep, case_no);
        return 1;
    }

    (void)vibeos_journal_commit(&g_journal);

    /* Simulate power loss: only durable sectors survive. Reattach a fresh
     * cache and let the production recovery path decide what to do. */
    g_device.pending_count = 0;
    g_device.cut_flush = -1;
    g_device.cut_after = 0;
    g_device.flush_count = 0;
    g_device.order_count = 0;
    if (attach_journal() != 0) {
        printf("FAIL:txn_torture seed=%llu sweep=%u case=%u: recovery attach\n",
               (unsigned long long)g_initial_seed, sweep, case_no);
        return 1;
    }

    committed = rd32(g_device.durable[JOURNAL_BASE + 3u]) == COMMIT_MAGIC;
    expected_new = committed;
    if (device_read(&g_device, TARGET_A, actual) != 0 ||
        memcmp(actual, expected_new ? new_a : old_a, sizeof(actual)) != 0) {
        printf("FAIL:txn_torture seed=%llu sweep=%u case=%u: target A violates commit point\n",
               (unsigned long long)g_initial_seed, sweep, case_no);
        return 1;
    }
    if (device_read(&g_device, TARGET_B, actual) != 0 ||
        memcmp(actual, expected_new ? new_b : old_b, sizeof(actual)) != 0) {
        printf("FAIL:txn_torture seed=%llu sweep=%u case=%u: target B violates commit point\n",
               (unsigned long long)g_initial_seed, sweep, case_no);
        return 1;
    }
    return 0;
}

static int run_sweep(unsigned sweep) {
    static const uint32_t permutations3[6][3] = {
        {0, 1, 2}, {0, 2, 1}, {1, 0, 2},
        {1, 2, 0}, {2, 0, 1}, {2, 1, 0}
    };
    static const uint32_t permutations2[2][2] = {{0, 1}, {1, 0}};
    static const uint32_t retire_first[3] = {2, 0, 1};
    unsigned c = 0, p, cut;

    /* Phase one: descriptor and both staged blocks. Every persistence order
     * and every prefix is cut before the commit record has been issued. */
    for (p = 0; p < 6u; p++) {
        for (cut = 0; cut <= 3u; cut++) {
            if (run_cut_case((uint32_t)g_initial_seed, sweep, 0, cut,
                             permutations3[p], 3, c++)) return 1;
        }
    }

    /* Phase two: the commit record itself is the commit point. */
    for (cut = 0; cut <= 1u; cut++) {
        if (run_cut_case((uint32_t)g_initial_seed, sweep, 1, cut,
                         0, 0, c++)) return 1;
    }

    /* Phase three: target blocks may be checkpointed in either order. A cut
     * after one target has reached disk must replay both from the journal. */
    for (p = 0; p < 2u; p++) {
        for (cut = 0; cut <= 2u; cut++) {
            if (run_cut_case((uint32_t)g_initial_seed, sweep, 2, cut,
                             permutations2[p], 2, c++)) return 1;
        }
    }

    /* Phase four: descriptor retirement. Either the committed descriptor is
     * replayed again or its clear write has reached durable media. */
    for (cut = 0; cut <= 1u; cut++) {
        if (run_cut_case((uint32_t)g_initial_seed, sweep, 3, cut,
                         0, 0, c++)) return 1;
    }

    /* Negative control: if the checkpoint flush is removed, target writes
     * and descriptor retirement share one flush. Persist the descriptor clear
     * first, then cut power: recovery can no longer replay the committed data.
     * The commit record is already durable, so both targets must still be new. */
    if (run_cut_case((uint32_t)g_initial_seed, sweep, 2, 1,
                     retire_first, 3, c++)) return 1;

    /* Clean completion is also checked: no cut, every target is new. */
    if (run_cut_case((uint32_t)g_initial_seed, sweep, -1, 0,
                     0, 0, c++)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    unsigned sweeps = 1u, i;
    g_rng = argc > 1 ? strtoull(argv[1], 0, 0) : 1u;
    if (g_rng == 0) g_rng = 0x9E3779B97F4A7C15ull;
    g_initial_seed = g_rng;
    if (argc > 2) sweeps = (unsigned)strtoul(argv[2], 0, 0);
    printf("TXN_TORTURE_SEED seed=%llu sweeps=%u\n",
           (unsigned long long)g_initial_seed, sweeps);
    for (i = 0; i < sweeps; i++) {
        if (run_sweep(i)) return 1;
    }
    printf("TXN_TORTURE_OK seed=%llu sweeps=%u\n",
           (unsigned long long)g_initial_seed, sweeps);
    return 0;
}
