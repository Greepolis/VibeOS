/* A disk that fails, swept across which request fails (I7).
 *
 * Every other test of this layer asks what happens when the disk works. This
 * asks what the layer promises when it does not, and the promise is three
 * things at once, which is why they are checked together rather than in three
 * separate tests:
 *
 *   - the caller is told it failed, never that it succeeded;
 *   - the failure carries a *reason* the caller can print;
 *   - nothing is left in flight afterwards.
 *
 * The third is the one a person would forget to check, and it is the one that
 * turns a bad sector into a machine that stops responding.
 *
 * The sweep matters because "the driver fails" is not one situation. Failing
 * the first request of a boot, failing one in the middle of a run of them, and
 * failing after the layer has already recorded a success are three different
 * states of the layer's own bookkeeping, and a single test picks one of them
 * by accident.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/blkdev.h"
#include "vibeos/blockdev.h"
#include "vibeos/io_stats.h"

#define BF_SECTORS 64u

static uint8_t g_disk[BF_SECTORS][VIBEOS_BLOCK_SIZE];
static uint32_t g_requests;        /* submitted since the last reset */
static uint32_t g_fail_at;         /* fail the request with this index */
static int g_fail_short;           /* fail by transferring too little */
static uint32_t g_in_driver;       /* driver entries not yet returned */
static int g_fail_partial;         /* move some sectors, then fail */
static int g_fail_barrier;         /* the barrier refuses */
static uint64_t g_barriers;
static int g_reenter;              /* submit again from inside submit */
static uint32_t g_reenter_depth;
static uint32_t g_reenter_max;

static int bf_submit(void *ctx, vibeos_blk_request_t *req) {
    uint32_t index = g_requests++;
    uint32_t i;

    (void)ctx;
    g_in_driver++;

    if (g_reenter && g_reenter_depth == 0u) {
        /* Submit again from inside the driver.
         *
         * This is not concurrency and the test does not claim it is: a host
         * test has one thread. What it *is* is the layer being re-entered
         * while a request is in flight, which is the same shape as another
         * core submitting - the same globals touched in the same order, with
         * the same question about whether the first request survives it.
         * Real concurrency needs the boot, and is written up as still open. */
        static uint8_t inner[VIBEOS_BLOCK_SIZE];
        vibeos_blk_request_t sub;

        g_reenter_depth++;
        if (g_reenter_depth > g_reenter_max) {
            g_reenter_max = g_reenter_depth;
        }
        memset(&sub, 0, sizeof(sub));
        sub.device = req->device;
        sub.lba = 20u;
        sub.sectors = 1u;
        sub.buf = inner;
        (void)vibeos_blk_submit(&sub);
        g_reenter_depth--;
    }

    if (index == g_fail_at && g_fail_partial) {
        /* Half the transfer landed in the caller's buffer and then the device
         * gave up. The sectors that did arrive are real; the rest is whatever
         * was in the buffer before. A layer that reports this as anything but
         * a failure hands the caller a buffer that is part stale and part
         * fresh, with no way to tell which half is which. */
        uint32_t moved = req->sectors / 2u;
        uint32_t k;
        for (k = 0; k < moved; k++) {
            memcpy((uint8_t *)req->buf + (size_t)k * VIBEOS_BLOCK_SIZE,
                   g_disk[req->lba + k], VIBEOS_BLOCK_SIZE);
        }
        req->sectors_done = moved;
        req->result = VIBEOS_BLK_MEDIUM;
        g_in_driver--;
        return -1;
    }

    if (index == g_fail_at) {
        if (g_fail_short) {
            /* The nastier failure: the driver reports success having moved
             * fewer sectors than it was asked for. A layer that believes the
             * return value alone hands the caller a buffer that is part stale,
             * which is worse than an error because it is silent. */
            req->sectors_done = (req->sectors > 1u) ? req->sectors - 1u : 0u;
            g_in_driver--;
            return 0;
        }
        req->result = VIBEOS_BLK_MEDIUM;
        req->sectors_done = 0;
        g_in_driver--;
        return -1;
    }

    for (i = 0; i < req->sectors; i++) {
        uint8_t *p = (uint8_t *)req->buf + (size_t)i * VIBEOS_BLOCK_SIZE;
        if (req->write) {
            memcpy(g_disk[req->lba + i], p, VIBEOS_BLOCK_SIZE);
        } else {
            memcpy(p, g_disk[req->lba + i], VIBEOS_BLOCK_SIZE);
        }
    }
    req->sectors_done = req->sectors;
    g_in_driver--;
    return 0;
}

static int bf_barrier(void *ctx) {
    (void)ctx;
    g_barriers++;
    return g_fail_barrier ? -1 : 0;
}

static uint32_t bf_attach(void) {
    vibeos_blk_driver_t drv;
    uint32_t dev = 0;

    memset(&drv, 0, sizeof(drv));
    drv.name = "failing";
    drv.sector_bytes = VIBEOS_BLOCK_SIZE;
    drv.sectors = BF_SECTORS;
    drv.submit = bf_submit;
    drv.barrier = bf_barrier;
    drv.ctx = 0;
    if (vibeos_blk_register(&drv, &dev) != 0) {
        return 0xFFFFFFFFu;
    }
    return dev;
}

/* Everything the layer and the fake device carry between runs. Extracted when
 * the three acceptance cases below needed the same setup as the sweep: two
 * copies of it would have been two places to forget a new field. */
static void bf_reset(void) {
    vibeos_blk_reset();
    memset(g_disk, 0x5A, sizeof(g_disk));
    g_requests = 0;
    g_in_driver = 0;
    g_fail_at = 0xFFFFFFFFu;
    g_fail_short = 0;
    g_fail_partial = 0;
    g_fail_barrier = 0;
    g_barriers = 0;
    g_reenter = 0;
    g_reenter_depth = 0;
    g_reenter_max = 0;
}

/* One sweep position. Returns 0 when the layer kept all three promises. */
static int bf_one(uint32_t fail_at, int shortly, uint32_t sectors) {
    /* Two sectors per slot, because the sweep issues multi-sector reads.
     * The first version sized these at one sector and read two into them: a
     * global buffer overflow that the plain host run did not notice and the
     * sanitized nightly caught in eighteen seconds. Sized from the same
     * constant the sweep uses, so the two cannot drift apart again. */
    static uint8_t buf[4][2u * VIBEOS_BLOCK_SIZE];
    uint32_t dev;
    uint32_t i;
    int saw_failure = 0;

    bf_reset();
    g_fail_at = fail_at;
    g_fail_short = shortly;

    dev = bf_attach();
    if (dev == 0xFFFFFFFFu) {
        printf("FAIL:blkfail the layer refused a well-formed driver\n");
        return -1;
    }

    /* Four reads, so a failure can land before, among, or after successes. */
    for (i = 0; i < 4u; i++) {
        int rc = vibeos_blk_read(dev, (uint64_t)i * sectors, sectors, buf[i]);

        if (i == fail_at) {
            if (rc == 0) {
                printf("FAIL:blkfail request %u failed in the driver and the "
                       "layer reported success (short=%d)\n", i, shortly);
                return -1;
            }
            saw_failure = 1;
        } else if (rc != 0) {
            printf("FAIL:blkfail request %u was refused and should not have "
                   "been (failing=%u)\n", i, fail_at);
            return -1;
        }
    }

    if (fail_at < 4u && !saw_failure) {
        printf("FAIL:blkfail the sweep never made request %u fail\n", fail_at);
        return -1;
    }

    /* Nothing left in flight. A driver that returned is a request that is
     * finished, whichever way it went. */
    if (g_in_driver != 0u) {
        printf("FAIL:blkfail %u requests left inside the driver\n",
               g_in_driver);
        return -1;
    }

    /* And the failure carries a reason. A layer that counts failures without
     * naming them turns "the disk is missing" and "the disk is broken" into
     * one number, which is the thing this layer was built to stop. */
    if (fail_at < 4u) {
        const vibeos_io_stats_t *st = vibeos_io_stats();
        uint64_t named = st->results[VIBEOS_BLK_MEDIUM] +
                         st->results[VIBEOS_BLK_SHORT];
        if (named == 0ull) {
            printf("FAIL:blkfail the failure was counted under no reason "
                   "(failing=%u short=%d)\n", fail_at, shortly);
            return -1;
        }
    }
    return 0;
}

/* A transfer that fails after moving some of the data. The bytes that arrived
 * are real and the rest are stale, and the caller must be told both that it
 * failed and how far it got - otherwise the only safe thing to do with the
 * buffer is throw it away, which for a page cache means throwing away a page
 * that is mostly correct. */
static int test_bf_partial_transfer(void) {
    static uint8_t buf[2u * VIBEOS_BLOCK_SIZE];
    vibeos_blk_request_t req;
    uint32_t dev;
    uint64_t before;

    bf_reset();
    dev = bf_attach();
    memset(g_disk[0], 0x11, VIBEOS_BLOCK_SIZE);
    memset(g_disk[1], 0x22, VIBEOS_BLOCK_SIZE);
    memset(buf, 0xEE, sizeof(buf));

    g_fail_at = 0;
    g_fail_partial = 1;
    before = vibeos_io_stats()->results[VIBEOS_BLK_MEDIUM];

    memset(&req, 0, sizeof(req));
    req.device = dev;
    req.lba = 0u;
    req.sectors = 2u;
    req.buf = buf;
    if (vibeos_blk_submit(&req) == 0) {
        printf("FAIL:blkfail a partial transfer was reported as success\n");
        return -1;
    }
    if (req.sectors_done != 1u) {
        printf("FAIL:blkfail a partial transfer did not say how far it got "
               "(sectors_done=%u)\n", req.sectors_done);
        return -1;
    }
    if (vibeos_io_stats()->results[VIBEOS_BLK_MEDIUM] == before) {
        printf("FAIL:blkfail a partial transfer was counted under no reason\n");
        return -1;
    }
    /* The first sector really did arrive, and the second really did not. Both
     * halves are asserted: a layer that zeroed the buffer on failure would
     * pass the first check and lose data the device had actually delivered. */
    if (buf[0] != 0x11u) {
        printf("FAIL:blkfail the sectors that arrived were discarded\n");
        return -1;
    }
    if (buf[VIBEOS_BLOCK_SIZE] != 0xEEu) {
        printf("FAIL:blkfail the buffer past the failure was written\n");
        return -1;
    }
    return 0;
}

/* A barrier that the device refuses. The journal builds its entire recovery
 * argument on the answer to this call, so "yes" when the device said no is the
 * difference between a recoverable medium and a corrupt one. */
static int test_bf_barrier_refused(void) {
    uint32_t dev;
    uint64_t failed_before;

    bf_reset();
    dev = bf_attach();

    if (vibeos_blk_barrier(dev) != 0) {
        printf("FAIL:blkfail a working barrier was refused\n");
        return -1;
    }
    failed_before = vibeos_io_stats()->barriers_failed;
    g_fail_barrier = 1;
    if (vibeos_blk_barrier(dev) == 0) {
        printf("FAIL:blkfail a refused barrier was reported as granted\n");
        return -1;
    }
    if (vibeos_io_stats()->barriers_failed != failed_before + 1ull) {
        printf("FAIL:blkfail a refused barrier was not counted\n");
        return -1;
    }
    if (g_barriers < 2ull) {
        printf("FAIL:blkfail the barrier never reached the driver\n");
        return -1;
    }
    return 0;
}

/* The layer re-entered while a request is in flight. Not concurrency - a host
 * test has one thread - but the same shape: the same globals touched in the
 * same order while an earlier request is unfinished. The outer request must
 * still complete correctly and describe its own transfer, not the inner one's.
 */
static int test_bf_reentrant_submit(void) {
    static uint8_t buf[VIBEOS_BLOCK_SIZE];
    vibeos_blk_request_t req;
    uint32_t dev;
    uint32_t i;

    bf_reset();
    dev = bf_attach();
    memset(g_disk[5], 0x77, VIBEOS_BLOCK_SIZE);
    g_reenter = 1;

    memset(&req, 0, sizeof(req));
    req.device = dev;
    req.lba = 5u;
    req.sectors = 1u;
    req.buf = buf;
    if (vibeos_blk_submit(&req) != 0) {
        printf("FAIL:blkfail a re-entrant submit broke the outer request\n");
        return -1;
    }
    if (g_reenter_max == 0u) {
        printf("FAIL:blkfail the re-entrant submit never happened - the test "
               "proves nothing\n");
        return -1;
    }
    if (req.sectors_done != 1u || req.result != VIBEOS_BLK_OK) {
        printf("FAIL:blkfail the outer request describes the wrong transfer\n");
        return -1;
    }
    for (i = 0; i < VIBEOS_BLOCK_SIZE; i++) {
        if (buf[i] != 0x77u) {
            printf("FAIL:blkfail the outer request got the inner one's data\n");
            return -1;
        }
    }
    return 0;
}

int test_blkfail(void) {
    uint32_t fail_at;
    int shortly;
    uint32_t sectors;

    /* Sweep: which request fails, how it fails, and whether the request is one
     * sector or several - a short transfer cannot be expressed by a
     * single-sector request, and that is exactly the case a sweep over only
     * one size would miss. */
    for (shortly = 0; shortly < 2; shortly++) {
        for (sectors = 1u; sectors <= 2u; sectors++) {
            if (shortly && sectors == 1u) {
                continue;   /* a one-sector transfer cannot be short */
            }
            for (fail_at = 0; fail_at < 5u; fail_at++) {
                if (bf_one(fail_at, shortly, sectors) != 0) {
                    return -1;
                }
            }
        }
    }
    if (test_bf_partial_transfer() != 0) { return -1; }
    if (test_bf_barrier_refused() != 0) { return -1; }
    if (test_bf_reentrant_submit() != 0) { return -1; }
    return 0;
}
