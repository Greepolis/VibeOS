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

/* ---- and, since I5b, more than one of them ---------------------------------
 *
 * This adapter used to hold one set of function pointers and refuse the second
 * driver outright: "first one to come up owns the disk". That was right while
 * a machine had exactly one disk, and it is the reason I5's filesystem images
 * had to be reached through a loop device rather than attached as real
 * hardware - and it is a hard blocker for I5b, whose whole premise is a log on
 * a medium that survives the machine.
 *
 * Each bind is now its own adapter with its own pointers, and its own device
 * number from the block layer, which was always multi-device underneath. What
 * does not change is which disk "the disk" means: the first to bind still owns
 * that, because it is the one the machine booted from and every caller above
 * assumes it.
 */
#define BLK_MAX_ADAPTERS 4u

typedef struct {
    int (*read)(uint64_t lba, void *buf);
    int (*read_many)(uint64_t lba, void *buf, uint32_t sectors);
    int (*write)(uint64_t lba, const void *buf);
    int (*write_many)(uint64_t lba, const void *buf, uint32_t sectors);
    int (*barrier)(void);
    uint64_t (*timeouts)(void);
    const char *name;
    int device;
} blk_adapter_t;

static blk_adapter_t g_adapters[BLK_MAX_ADAPTERS];
static uint32_t g_adapter_count;

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
    blk_adapter_t *a = (blk_adapter_t *)ctx;
    uint32_t i;
    uint64_t timeouts_before;

    if (a == 0) {
        req->result = VIBEOS_BLK_NO_DEVICE;
        return -1;
    }
    timeouts_before = a->timeouts ? a->timeouts() : 0ull;
    if (req->write) {
        /* Preferred when it exists, for the same reason read_many is: a run
         * done a sector at a time under a lock that masks interrupts was
         * indistinguishable from a hang on the read side, and there is no
         * reason to wait for the write side to teach the same lesson. */
        if (a->write_many) {
            if (a->write_many(req->lba, req->buf, req->sectors) != 0) {
                req->sectors_done = 0;
                if (a->timeouts && a->timeouts() != timeouts_before) {
                    req->result = VIBEOS_BLK_TIMEOUT;
                }
                return -1;
            }
            req->sectors_done = req->sectors;
            return 0;
        }
        if (!a->write) {
            req->result = VIBEOS_BLK_NO_DEVICE;
            return -1;
        }
        for (i = 0; i < req->sectors; i++) {
            const uint8_t *p = (const uint8_t *)req->buf + (uint64_t)i * 512ull;
            if (a->write(req->lba + i, p) != 0) {
                req->sectors_done = i;
                if (a->timeouts && a->timeouts() != timeouts_before) {
                    req->result = VIBEOS_BLK_TIMEOUT;
                }
                return -1;
            }
        }
        req->sectors_done = req->sectors;
        return 0;
    }

    if (a->read_many) {
        if (a->read_many(req->lba, req->buf, req->sectors) != 0) {
            req->sectors_done = 0;
            if (a->timeouts && a->timeouts() != timeouts_before) {
                req->result = VIBEOS_BLK_TIMEOUT;
            }
            return -1;
        }
        req->sectors_done = req->sectors;
        return 0;
    }
    if (!a->read) {
        req->result = VIBEOS_BLK_NO_DEVICE;
        return -1;
    }
    for (i = 0; i < req->sectors; i++) {
        uint8_t *p = (uint8_t *)req->buf + (uint64_t)i * 512ull;
        if (a->read(req->lba + i, p) != 0) {
            req->sectors_done = i;
            if (a->timeouts && a->timeouts() != timeouts_before) {
                req->result = VIBEOS_BLK_TIMEOUT;
            }
            return -1;
        }
    }
    req->sectors_done = req->sectors;
    return 0;
}

static int adapt_barrier(void *ctx) {
    blk_adapter_t *a = (blk_adapter_t *)ctx;
    return (a && a->barrier) ? a->barrier() : -1;
}

void vibeos_x86_64_blk_bind(const char *name,
                            int (*read)(uint64_t, void *),
                            int (*read_many)(uint64_t, void *, uint32_t),
                            int (*write)(uint64_t, const void *),
                            int (*write_many)(uint64_t, const void *, uint32_t),
                            int (*barrier)(void),
                            uint64_t sectors,
                            uint64_t (*timeouts)(void)) {
    vibeos_blk_driver_t drv;
    blk_adapter_t *a;
    uint32_t dev = 0;

    /* A driver that cannot read is not a disk. Refused here rather than
     * registered and discovered later, because the layer answers a null read
     * with NO_DEVICE and that message names the wrong culprit. */
    if (read == 0 && read_many == 0) {
        return;
    }
    if (g_adapter_count >= BLK_MAX_ADAPTERS) {
        return;
    }
    a = &g_adapters[g_adapter_count];
    a->read = read;
    a->read_many = read_many;
    a->write = write;
    a->write_many = write_many;
    a->barrier = barrier;
    a->timeouts = timeouts;
    a->name = name;
    a->device = -1;

    /* A device that would not say how big it is does not get registered, and
     * the machine says so rather than reading past the end of it later. This
     * is the check that made AHCI learn to issue IDENTIFY: it had never asked,
     * so every AHCI read was unbounded. */
    drv.name = name;
    drv.sector_bytes = 512u;
    drv.sectors = sectors;
    drv.submit = adapt_submit;
    /* Registered only when the driver actually has one. A null here is
     * answered with a refusal by the layer, which is the honest answer for a
     * device that cannot order its own writes. */
    drv.barrier = barrier ? adapt_barrier : 0;
    drv.ctx = a;
    if (vibeos_blk_register(&drv, &dev) == 0) {
        a->device = (int)dev;
        g_adapter_count++;   /* published last: the entry is complete first */
    }
}

/* The bound driver's timeout count, or zero if no disk came up. One accessor
 * so kmain does not have to know which driver won. */
/* The boot disk's counters and identity. Adapter 0 is the first driver that
 * came up, which is the one the machine booted from - every caller above this
 * layer means that one when it says "the disk". */
static blk_adapter_t *blk_boot(void) {
    return (g_adapter_count > 0u) ? &g_adapters[0] : 0;
}

uint64_t vibeos_x86_64_blk_timeouts(void) {
    blk_adapter_t *a = blk_boot();
    return (a && a->timeouts) ? a->timeouts() : 0ull;
}

/* How many disks came up, for a boot that wants to say so. */
uint32_t vibeos_x86_64_blk_adapter_count(void) {
    return g_adapter_count;
}

/* The block-layer device number of the nth disk that bound, or -1. */
int vibeos_x86_64_blk_adapter_device(uint32_t n) {
    return (n < g_adapter_count) ? g_adapters[n].device : -1;
}

const char *vibeos_x86_64_blk_adapter_name(uint32_t n) {
    return (n < g_adapter_count) ? g_adapters[n].name : "none";
}

int vibeos_x86_64_blk_device(void) {
    blk_adapter_t *a = blk_boot();
    return a ? a->device : -1;
}

const char *vibeos_x86_64_blk_name(void) {
    blk_adapter_t *a = blk_boot();
    return a ? a->name : "none";
}

int vibeos_x86_64_blk_present(void) {
    return g_adapter_count > 0u;
}

/* The three the filesystem still calls, routed through the layer.
 *
 * They keep returning a bare int because their callers do - fat.c has nine
 * call sites and moving them is its own change. What they gain is everything
 * underneath: a bounds check against the device's real size, a short transfer
 * caught rather than believed, a reason recorded, and a counter moved. The
 * reason is not thrown away, it is simply not asked for here yet. */
int vibeos_x86_64_blk_read(uint64_t lba, void *buf) {
    int dev = vibeos_x86_64_blk_device();

    if (dev < 0) {
        return -1;
    }
    return vibeos_blk_read((uint32_t)dev, lba, 1u, buf);
}

int vibeos_x86_64_blk_read_many(uint64_t lba, void *buf, uint32_t sectors) {
    int dev = vibeos_x86_64_blk_device();

    if (dev < 0) {
        return -1;
    }
    return vibeos_blk_read((uint32_t)dev, lba, sectors, buf);
}

int vibeos_x86_64_blk_write(uint64_t lba, const void *buf) {
    int dev = vibeos_x86_64_blk_device();

    if (dev < 0) {
        return -1;
    }
    return vibeos_blk_write((uint32_t)dev, lba, 1u, buf);
}
