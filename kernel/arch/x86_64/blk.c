/* Which block driver the filesystem talks to.
 *
 * fat.c used to call vibeos_x86_64_virtio_blk_read by name, from nine places.
 * That was honest while virtio was the only driver, and it is exactly why the
 * VM images shipped for months without anyone noticing they could not read
 * their own disk: QEMU offers virtio-blk, VirtualBox and VMware offer AHCI,
 * and the bootloader hid the difference because UEFI does the reading up to
 * ExitBootServices. After that the kernel had no disk at all - not a failure,
 * an absence, which is quieter.
 *
 * So the filesystem asks for "the disk" and this decides which one that is.
 * There is deliberately no probing here: a driver that came up binds itself,
 * and the first one to bind wins. Ordering lives at the call site in
 * arch_hw.c, where it can be read.
 */

/* ---- and, since I1, an adapter onto the real block layer -------------------
 *
 * Everything above still calls vibeos_x86_64_blk_read; underneath, that is now
 * a request through kernel/io/blkdev.c, which bounds-checks it, compares what
 * came back against what was asked for, and gives a failure a reason.
 *
 * An adapter rather than a rewrite of the callers, deliberately. fat.c is how
 * this machine boots, and changing the layer beneath it and its own call sites
 * in one step would mean a failure could be attributed to either. The callers
 * move later; this makes the new layer load-bearing today, on every boot, with
 * nothing above it changed.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"
#include "vibeos/blkdev.h"

static int (*g_read)(uint64_t lba, void *buf);
static int (*g_read_many)(uint64_t lba, void *buf, uint32_t sectors);
static int (*g_write)(uint64_t lba, const void *buf);
static const char *g_name = "none";
static int g_device = -1;
static uint64_t (*g_timeouts)(void);

/* The old three functions, behind the one entry point the layer expects.
 *
 * read_many is preferred when it exists: the multi-sector path is what turned
 * a 2 MiB FAT read from something indistinguishable from a hang into an
 * ordinary read, and falling back to one sector at a time here would undo
 * that quietly. */
/* Did the bound fire during this request?
 *
 * Sampled around the call rather than returned through it, because the driver
 * entry points are int-returning and threading a reason through three of them
 * in two drivers would be a wider change than the fact deserves. The counter
 * only grows, so a move across the call means a bound fired.
 *
 * Under concurrency another core's timeout could be attributed to this
 * request. That is accepted and worth stating: the counter the gate asserts is
 * the driver's own and is exact, and mislabelling *which* request timed out
 * matters far less than the alternative, which was not knowing that anything
 * had. */
static int adapt_submit(void *ctx, vibeos_blk_request_t *req) {
    uint32_t i;
    uint64_t timeouts_before = g_timeouts ? g_timeouts() : 0ull;

    (void)ctx;
    if (req->write) {
        if (!g_write) {
            req->result = VIBEOS_BLK_NO_DEVICE;
            return -1;
        }
        for (i = 0; i < req->sectors; i++) {
            const uint8_t *p = (const uint8_t *)req->buf + (uint64_t)i * 512ull;
            if (g_write(req->lba + i, p) != 0) {
                req->sectors_done = i;
                if (g_timeouts && g_timeouts() != timeouts_before) {
                    req->result = VIBEOS_BLK_TIMEOUT;
                }
                return -1;
            }
        }
        req->sectors_done = req->sectors;
        return 0;
    }

    if (g_read_many) {
        if (g_read_many(req->lba, req->buf, req->sectors) != 0) {
            req->sectors_done = 0;
            if (g_timeouts && g_timeouts() != timeouts_before) {
                req->result = VIBEOS_BLK_TIMEOUT;
            }
            return -1;
        }
        req->sectors_done = req->sectors;
        return 0;
    }
    if (!g_read) {
        req->result = VIBEOS_BLK_NO_DEVICE;
        return -1;
    }
    for (i = 0; i < req->sectors; i++) {
        uint8_t *p = (uint8_t *)req->buf + (uint64_t)i * 512ull;
        if (g_read(req->lba + i, p) != 0) {
            req->sectors_done = i;
            if (g_timeouts && g_timeouts() != timeouts_before) {
                req->result = VIBEOS_BLK_TIMEOUT;
            }
            return -1;
        }
    }
    req->sectors_done = req->sectors;
    return 0;
}

void vibeos_x86_64_blk_bind(const char *name,
                            int (*read)(uint64_t, void *),
                            int (*read_many)(uint64_t, void *, uint32_t),
                            int (*write)(uint64_t, const void *),
                            uint64_t sectors,
                            uint64_t (*timeouts)(void)) {
    vibeos_blk_driver_t drv;
    uint32_t dev = 0;

    if (g_read != 0) {
        return;   /* first one to come up owns the disk */
    }
    g_name = name;
    g_read = read;
    g_read_many = read_many;
    g_write = write;
    g_timeouts = timeouts;

    /* A device that would not say how big it is does not get registered, and
     * the machine says so rather than reading past the end of it later. This
     * is the check that made AHCI learn to issue IDENTIFY: it had never asked,
     * so every AHCI read was unbounded. */
    drv.name = name;
    drv.sector_bytes = 512u;
    drv.sectors = sectors;
    drv.submit = adapt_submit;
    drv.ctx = 0;
    if (vibeos_blk_register(&drv, &dev) == 0) {
        g_device = (int)dev;
    }
}

/* The bound driver's timeout count, or zero if no disk came up. One accessor
 * so kmain does not have to know which driver won. */
uint64_t vibeos_x86_64_blk_timeouts(void) {
    return g_timeouts ? g_timeouts() : 0ull;
}

int vibeos_x86_64_blk_device(void) {
    return g_device;
}

const char *vibeos_x86_64_blk_name(void) {
    return g_name;
}

int vibeos_x86_64_blk_present(void) {
    return g_read != 0;
}

/* The three the filesystem still calls, routed through the layer.
 *
 * They keep returning a bare int because their callers do - fat.c has nine
 * call sites and moving them is its own change. What they gain is everything
 * underneath: a bounds check against the device's real size, a short transfer
 * caught rather than believed, a reason recorded, and a counter moved. The
 * reason is not thrown away, it is simply not asked for here yet. */
int vibeos_x86_64_blk_read(uint64_t lba, void *buf) {
    if (g_device < 0) {
        return -1;
    }
    return vibeos_blk_read((uint32_t)g_device, lba, 1u, buf);
}

int vibeos_x86_64_blk_read_many(uint64_t lba, void *buf, uint32_t sectors) {
    if (g_device < 0) {
        return -1;
    }
    return vibeos_blk_read((uint32_t)g_device, lba, sectors, buf);
}

int vibeos_x86_64_blk_write(uint64_t lba, const void *buf) {
    if (g_device < 0) {
        return -1;
    }
    return vibeos_blk_write((uint32_t)g_device, lba, 1u, buf);
}
