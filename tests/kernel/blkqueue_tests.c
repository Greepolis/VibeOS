/* Submit now, finish later: the contract before the asynchrony (I6).
 *
 * Both drivers are still synchronous, so nothing here is asynchronous yet -
 * and that is the point. The plan puts asynchrony last because it makes every
 * existing defect harder to see, so the *contract* arrives first, while the
 * callers are still synchronous and the properties are still checkable in a
 * host test with no timing in it:
 *
 *   a request is completed exactly once;
 *   a request that was never in flight cannot be completed;
 *   the completion callback runs outside the queue lock;
 *   the synchronous entry points still behave exactly as they did.
 *
 * Every one of these is a defect that an interrupt-driven driver introduces if
 * the contract is not already established, and every one of them is invisible
 * once the driver and the caller stop sharing a call stack. That is the whole
 * argument for writing this test before the interrupt exists rather than after.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/blkdev.h"
#include "vibeos/blockdev.h"
#include "vibeos/io_stats.h"

#define BQ_SECTORS 32u

static uint8_t g_disk[BQ_SECTORS][VIBEOS_BLOCK_SIZE];
static uint32_t g_calls;
static vibeos_blk_request_t *g_last_completed;
static int g_saw_state_done;

static int bq_submit(void *ctx, vibeos_blk_request_t *req) {
    uint32_t i;

    (void)ctx;
    for (i = 0; i < req->sectors; i++) {
        uint8_t *p = (uint8_t *)req->buf + (size_t)i * VIBEOS_BLOCK_SIZE;
        if (req->write) {
            memcpy(g_disk[req->lba + i], p, VIBEOS_BLOCK_SIZE);
        } else {
            memcpy(p, g_disk[req->lba + i], VIBEOS_BLOCK_SIZE);
        }
    }
    req->sectors_done = req->sectors;
    return 0;
}

static uint32_t bq_attach(void) {
    vibeos_blk_driver_t drv;
    uint32_t dev = 0;

    memset(&drv, 0, sizeof(drv));
    drv.name = "queue";
    drv.sector_bytes = VIBEOS_BLOCK_SIZE;
    drv.sectors = BQ_SECTORS;
    drv.submit = bq_submit;
    drv.ctx = 0;
    if (vibeos_blk_register(&drv, &dev) != 0) {
        return 0xFFFFFFFFu;
    }
    return dev;
}

static void bq_done(void *ctx, struct vibeos_blk_request *req) {
    (void)ctx;
    g_calls++;
    g_last_completed = req;
    /* The request must already read as finished when the callback runs. A
     * callback that sees IN_FLIGHT is one whose owner cannot tell, from the
     * request alone, whether it is safe to reuse. */
    if (req->state == VIBEOS_BLK_REQ_DONE) {
        g_saw_state_done = 1;
    }
}

static void bq_reset(void) {
    vibeos_blk_reset();
    memset(g_disk, 0x3C, sizeof(g_disk));
    g_calls = 0;
    g_last_completed = 0;
    g_saw_state_done = 0;
}

/* ---- the tests ----------------------------------------------------------- */

/* The callback runs, exactly once, with the request already marked done. */
static int test_bq_completes_once(void) {
    static uint8_t buf[VIBEOS_BLOCK_SIZE];
    vibeos_blk_request_t req;
    uint32_t dev;

    bq_reset();
    dev = bq_attach();
    memset(&req, 0, sizeof(req));
    req.device = dev;
    req.lba = 3u;
    req.sectors = 1u;
    req.buf = buf;
    req.done = bq_done;

    if (vibeos_blk_enqueue(&req) != 0) {
        printf("FAIL:blkqueue a well-formed request was refused\n");
        return -1;
    }
    if (g_calls != 1u) {
        printf("FAIL:blkqueue the callback ran %u times, expected once\n",
               g_calls);
        return -1;
    }
    if (g_last_completed != &req) {
        printf("FAIL:blkqueue the callback was handed a different request\n");
        return -1;
    }
    if (!g_saw_state_done) {
        printf("FAIL:blkqueue the callback saw a request not yet marked done\n");
        return -1;
    }
    if (req.result != VIBEOS_BLK_OK || req.sectors_done != 1u) {
        printf("FAIL:blkqueue the completed request does not describe the "
               "transfer\n");
        return -1;
    }
    return 0;
}

/* A second completion is refused and counted, not delivered. By the time it
 * arrives the owner may have reused the request. */
static int test_bq_refuses_a_second_completion(void) {
    static uint8_t buf[VIBEOS_BLOCK_SIZE];
    vibeos_blk_request_t req;
    uint64_t before;
    uint32_t dev;

    bq_reset();
    dev = bq_attach();
    memset(&req, 0, sizeof(req));
    req.device = dev;
    req.lba = 1u;
    req.sectors = 1u;
    req.buf = buf;
    req.done = bq_done;
    (void)vibeos_blk_enqueue(&req);

    before = vibeos_io_stats()->completed_twice;
    vibeos_blk_complete(&req, VIBEOS_BLK_OK, 1u);
    if (g_calls != 1u) {
        printf("FAIL:blkqueue a second completion ran the callback again\n");
        return -1;
    }
    if (vibeos_io_stats()->completed_twice != before + 1ull) {
        printf("FAIL:blkqueue a second completion was not counted\n");
        return -1;
    }
    return 0;
}

/* A request nobody submitted cannot be completed. This is what an interrupt
 * naming the wrong tag looks like from the layer's side. */
static int test_bq_refuses_a_stray_completion(void) {
    vibeos_blk_request_t req;
    uint64_t before;

    bq_reset();
    (void)bq_attach();
    memset(&req, 0, sizeof(req));
    req.done = bq_done;

    before = vibeos_io_stats()->completed_not_inflight;
    vibeos_blk_complete(&req, VIBEOS_BLK_OK, 0u);
    if (g_calls != 0u) {
        printf("FAIL:blkqueue a stray completion ran a callback\n");
        return -1;
    }
    if (vibeos_io_stats()->completed_not_inflight != before + 1ull) {
        printf("FAIL:blkqueue a stray completion was not counted\n");
        return -1;
    }
    return 0;
}

/* The callback runs outside the lock, so that the first thing a real one does -
 * submit the next request - does not deadlock the day the queue has a real
 * lock. Checked by the counter the layer keeps, because "we do not do that" is
 * not a property anybody checks. */
static int test_bq_callback_runs_outside_the_lock(void) {
    static uint8_t buf[VIBEOS_BLOCK_SIZE];
    vibeos_blk_request_t req;
    uint32_t dev;

    bq_reset();
    dev = bq_attach();
    memset(&req, 0, sizeof(req));
    req.device = dev;
    req.lba = 0u;
    req.sectors = 1u;
    req.buf = buf;
    req.done = bq_done;
    (void)vibeos_blk_enqueue(&req);

    if (vibeos_io_stats()->callback_under_lock != 0ull) {
        printf("FAIL:blkqueue the completion callback ran holding the queue "
               "lock\n");
        return -1;
    }
    return 0;
}

/* A refusal completes too. A request the layer rejects before any driver sees
 * it must still reach its owner, or a caller that waits for a callback waits
 * for ever - which is the shape of every I/O hang there is. */
static int test_bq_a_refusal_still_completes(void) {
    vibeos_blk_request_t req;

    bq_reset();
    (void)bq_attach();
    memset(&req, 0, sizeof(req));
    req.device = 99u;            /* no such device */
    req.lba = 0u;
    req.sectors = 1u;
    req.buf = (void *)0;         /* and no buffer either */
    req.done = bq_done;

    if (vibeos_blk_enqueue(&req) == 0) {
        printf("FAIL:blkqueue a malformed request was accepted\n");
        return -1;
    }
    if (g_calls != 1u) {
        printf("FAIL:blkqueue a refused request never reached its owner\n");
        return -1;
    }
    if (req.state != VIBEOS_BLK_REQ_DONE) {
        printf("FAIL:blkqueue a refused request was left in flight\n");
        return -1;
    }
    return 0;
}

/* And the synchronous path is unchanged: same result, same bytes, no callback.
 * The whole point of the wrapper is that nothing above had to move. */
static int test_bq_synchronous_path_unchanged(void) {
    static uint8_t buf[VIBEOS_BLOCK_SIZE];
    uint32_t dev;
    uint32_t i;

    bq_reset();
    dev = bq_attach();
    memset(g_disk[7], 0xA7, VIBEOS_BLOCK_SIZE);
    if (vibeos_blk_read(dev, 7u, 1u, buf) != 0) {
        printf("FAIL:blkqueue the synchronous read failed\n");
        return -1;
    }
    for (i = 0; i < VIBEOS_BLOCK_SIZE; i++) {
        if (buf[i] != 0xA7u) {
            printf("FAIL:blkqueue the synchronous read returned wrong bytes\n");
            return -1;
        }
    }
    if (g_calls != 0u) {
        printf("FAIL:blkqueue the synchronous path ran a callback it was "
               "never given\n");
        return -1;
    }
    return 0;
}

int test_blkqueue(void) {
    if (test_bq_completes_once() != 0) { return -1; }
    if (test_bq_refuses_a_second_completion() != 0) { return -1; }
    if (test_bq_refuses_a_stray_completion() != 0) { return -1; }
    if (test_bq_callback_runs_outside_the_lock() != 0) { return -1; }
    if (test_bq_a_refusal_still_completes() != 0) { return -1; }
    if (test_bq_synchronous_path_unchanged() != 0) { return -1; }
    return 0;
}
