/* B1 — block devices. See include/vibeos/blkdev.h for why a request is a value.
 *
 * The whole of the layer is: validate, dispatch, and check what came back
 * against what was asked for. That third step is the one that does not exist
 * today and is the reason this file does: a driver that moves four sectors of
 * eight and returns success is reporting a success that lost half the data,
 * and every caller above would believe it.
 */

#include "vibeos/blkdev.h"
#include "vibeos/io_stats.h"

/* Small and fixed. This layer is reached from paths that must not allocate -
 * a page fault reading an executable, and eventually a log written from a
 * panic handler - so the table is static and its size is a decision recorded
 * in docs/io/decisions.md rather than a limit discovered at runtime. */
#define BLK_MAX_DEVICES 4u

static vibeos_blk_driver_t g_dev[BLK_MAX_DEVICES];
static uint32_t g_count;

static vibeos_io_stats_t g_stats;

vibeos_io_stats_t *vibeos_io_stats(void) {
    return &g_stats;
}

void vibeos_io_stats_reset(void) {
    uint8_t *p = (uint8_t *)&g_stats;
    uint32_t i;

    for (i = 0; i < sizeof(g_stats); i++) {
        p[i] = 0;
    }
}

const char *vibeos_blk_result_name(vibeos_blk_result_t r) {
    switch (r) {
        case VIBEOS_BLK_OK:           return "ok";
        case VIBEOS_BLK_NOT_ISSUED:   return "not-issued";
        case VIBEOS_BLK_BAD_REQUEST:  return "bad-request";
        case VIBEOS_BLK_NO_DEVICE:    return "no-device";
        case VIBEOS_BLK_OUT_OF_RANGE: return "out-of-range";
        case VIBEOS_BLK_TIMEOUT:      return "timeout";
        case VIBEOS_BLK_MEDIUM:       return "medium";
        case VIBEOS_BLK_SHORT:        return "short";
        default:                      return "unknown";
    }
}

void vibeos_blk_reset(void) {
    uint32_t i;

    for (i = 0; i < BLK_MAX_DEVICES; i++) {
        g_dev[i].name = 0;
        g_dev[i].submit = 0;
        g_dev[i].ctx = 0;
        g_dev[i].sector_bytes = 0;
        g_dev[i].sectors = 0;
    }
    g_count = 0;
}

int vibeos_blk_register(const vibeos_blk_driver_t *drv, uint32_t *out_device) {
    /* A device that cannot say how big it is cannot have its requests
     * bounds-checked, and an unchecked bound is the difference between an
     * error and a disk written at the wrong offset. Refused rather than
     * defaulted: guessing a size here would make every later check a guess. */
    if (!drv || !drv->submit || !drv->name ||
        drv->sector_bytes == 0u || drv->sectors == 0ull) {
        g_stats.register_refused++;
        return -1;
    }
    if (g_count >= BLK_MAX_DEVICES) {
        g_stats.register_refused++;
        return -1;
    }
    g_dev[g_count] = *drv;
    if (out_device) {
        *out_device = g_count;
    }
    g_count++;
    g_stats.devices_registered = g_count;
    return 0;
}

uint32_t vibeos_blk_count(void) {
    return g_count;
}

int vibeos_blk_info(uint32_t device, vibeos_blk_driver_t *out) {
    if (device >= g_count || !out) {
        return -1;
    }
    *out = g_dev[device];
    return 0;
}

const char *vibeos_blk_name(uint32_t device) {
    if (device >= g_count) {
        return "none";
    }
    return g_dev[device].name;
}

/* Count a result, whatever a driver put there.
 *
 * The index comes from the driver on the paths where the driver set it, and a
 * driver is not trusted to return a value from this enum. The first version
 * indexed the array with it directly, and a sabotage case found it the way
 * these things are found: the suite segfaulted rather than failing an
 * assertion, because an out-of-range result wrote past the end of the
 * statistics.
 *
 * A value outside the enum is itself worth knowing about - it means a driver
 * is returning something nobody defined - so it is counted as BAD_REQUEST
 * rather than dropped. */
static void count_result(vibeos_blk_result_t r) {
    if ((unsigned)r >= (unsigned)VIBEOS_BLK_RESULT_COUNT) {
        g_stats.results[VIBEOS_BLK_BAD_REQUEST]++;
        return;
    }
    g_stats.results[r]++;
}

static int finish(vibeos_blk_request_t *req, vibeos_blk_result_t r) {
    req->result = r;
    count_result(r);
    return (r == VIBEOS_BLK_OK) ? 0 : -1;
}

int vibeos_blk_submit(vibeos_blk_request_t *req) {
    vibeos_blk_driver_t *d;
    int rc;

    if (!req) {
        return -1;
    }
    /* Reset before anything else, so a request that is refused below carries a
     * reason rather than whatever the caller left in the struct. */
    req->result = VIBEOS_BLK_NOT_ISSUED;
    req->sectors_done = 0;

    if (!req->buf || req->sectors == 0u) {
        return finish(req, VIBEOS_BLK_BAD_REQUEST);
    }
    if (req->device >= g_count) {
        return finish(req, VIBEOS_BLK_NO_DEVICE);
    }
    d = &g_dev[req->device];

    /* The bound lives here rather than in each driver.
     *
     * A driver that checks its own bounds protects the *device*; it cannot
     * protect a neighbouring partition, whose sectors are perfectly valid
     * addresses on that device. The same argument the swap area makes about
     * the filesystem, one layer down. */
    if (req->lba > d->sectors ||
        (uint64_t)req->sectors > d->sectors - req->lba) {
        return finish(req, VIBEOS_BLK_OUT_OF_RANGE);
    }

    rc = d->submit(d->ctx, req);

    /* What came back, against what was asked for.
     *
     * A driver may set a reason itself, and if it did that reason stands - it
     * knows more than this layer does about why. What this layer will not
     * accept is silence: a non-zero return with no reason becomes MEDIUM,
     * because a driver that fails without saying so is still a failure the
     * caller must see. */
    if (rc != 0 && req->result == VIBEOS_BLK_NOT_ISSUED) {
        return finish(req, VIBEOS_BLK_MEDIUM);
    }
    if (rc != 0) {
        count_result(req->result);
        return -1;
    }
    if (req->sectors_done != req->sectors) {
        /* Success that lost data. This is the check the FAT defect needed:
         * there, a failed table read came back as the end-of-chain marker and
         * the reader returned the size the directory claimed. */
        return finish(req, VIBEOS_BLK_SHORT);
    }
    if (req->result != VIBEOS_BLK_NOT_ISSUED && req->result != VIBEOS_BLK_OK) {
        /* Zero return, non-OK reason. Trust the reason - but not the index. */
        count_result(req->result);
        return -1;
    }

    if (req->write) {
        g_stats.writes++;
        g_stats.sectors_written += req->sectors_done;
    } else {
        g_stats.reads++;
        g_stats.sectors_read += req->sectors_done;
    }
    return finish(req, VIBEOS_BLK_OK);
}

int vibeos_blk_read(uint32_t device, uint64_t lba, uint32_t sectors,
                    void *buf) {
    vibeos_blk_request_t req;

    req.device = device;
    req.lba = lba;
    req.sectors = sectors;
    req.buf = buf;
    req.write = 0;
    return vibeos_blk_submit(&req);
}

int vibeos_blk_write(uint32_t device, uint64_t lba, uint32_t sectors,
                     const void *buf) {
    vibeos_blk_request_t req;

    req.device = device;
    req.lba = lba;
    req.sectors = sectors;
    /* The cast is the one place this layer admits that a write does not modify
     * its buffer. Keeping `buf` non-const in the request is what lets read and
     * write be one operation with one path, which is the point. */
    req.buf = (void *)(uintptr_t)buf;
    req.write = 1;
    return vibeos_blk_submit(&req);
}
