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

static int bf_submit(void *ctx, vibeos_blk_request_t *req) {
    uint32_t index = g_requests++;
    uint32_t i;

    (void)ctx;
    g_in_driver++;

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

static uint32_t bf_attach(void) {
    vibeos_blk_driver_t drv;
    uint32_t dev = 0;

    memset(&drv, 0, sizeof(drv));
    drv.name = "failing";
    drv.sector_bytes = VIBEOS_BLOCK_SIZE;
    drv.sectors = BF_SECTORS;
    drv.submit = bf_submit;
    drv.barrier = 0;
    drv.ctx = 0;
    if (vibeos_blk_register(&drv, &dev) != 0) {
        return 0xFFFFFFFFu;
    }
    return dev;
}

/* One sweep position. Returns 0 when the layer kept all three promises. */
static int bf_one(uint32_t fail_at, int shortly, uint32_t sectors) {
    static uint8_t buf[4][VIBEOS_BLOCK_SIZE];
    uint32_t dev;
    uint32_t i;
    int saw_failure = 0;

    vibeos_blk_reset();
    memset(g_disk, 0x5A, sizeof(g_disk));
    g_requests = 0;
    g_in_driver = 0;
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
    return 0;
}
