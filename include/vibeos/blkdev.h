#ifndef VIBEOS_BLKDEV_H
#define VIBEOS_BLKDEV_H

#include <stdint.h>

/* B1 — block devices: a request, a result, and a reason when it fails.
 *
 * What this replaces is three functions with three shapes - `read(lba, buf)`,
 * `read_many(lba, buf, n)` and `write(lba, buf)` - bound to one global device.
 * They are the same operation, and the asymmetry is informative: there is no
 * `write_many`, and nobody noticed, because nothing on a booting machine ever
 * writes.
 *
 * ## Why a request is a value
 *
 * **So a failure can say why.** Today every one of those returns `int`. A
 * timeout, an absent device, a sector the medium cannot return and a request
 * that was never issued are the same number, at every layer above. This
 * project has already paid for that shape once: `fat_next_cluster` reported a
 * failed table read as the end-of-chain marker, which is the same value a
 * healthy last cluster returns, so a flaky sector produced a short file that
 * said it was complete and execve parsed the previous program's bytes.
 *
 * **So more than one device can exist.** A machine with a disk and a CD, or a
 * disk and a swap partition on another spindle, cannot be described today.
 *
 * **So I6 can queue it.** A request that is a value can be submitted now and
 * completed later without changing every caller. Asynchrony is deliberately
 * last in the plan, and this is the one thing it needs decided early.
 */

typedef enum vibeos_blk_result {
    VIBEOS_BLK_OK = 0,
    /* Never reached a driver. The initial value, so a request that comes back
     * untouched says so rather than looking like a success. */
    VIBEOS_BLK_NOT_ISSUED,
    VIBEOS_BLK_BAD_REQUEST,   /* the request is malformed; see submit()     */
    VIBEOS_BLK_NO_DEVICE,     /* no such device, or none registered         */
    VIBEOS_BLK_OUT_OF_RANGE,  /* past the end of the device                 */
    VIBEOS_BLK_TIMEOUT,       /* the bound fired                            */
    VIBEOS_BLK_MEDIUM,        /* the device says it cannot                  */
    VIBEOS_BLK_SHORT,         /* fewer sectors moved than asked for         */
    VIBEOS_BLK_RESULT_COUNT
} vibeos_blk_result_t;

const char *vibeos_blk_result_name(vibeos_blk_result_t r);

typedef struct vibeos_blk_request {
    /* Filled by the caller. */
    uint32_t device;
    uint64_t lba;
    uint32_t sectors;
    void    *buf;
    int      write;

    /* Filled by the layer and the driver.
     *
     * `sectors_done` is separate from `result` on purpose: a driver that moves
     * four sectors of eight and returns OK is reporting a success that lost
     * half the data. The layer compares the two and turns that into SHORT,
     * which is the check the FAT defect above would have needed. */
    vibeos_blk_result_t result;
    uint32_t sectors_done;
} vibeos_blk_request_t;

/* A driver is a table. Never a weak symbol: the PE/COFF lesson in CLAUDE.md is
 * that a weak definition in a different object from its caller links on Linux
 * and fails on Windows, and the Linux build says nothing about it. */
typedef struct vibeos_blk_driver {
    const char *name;
    uint32_t    sector_bytes;   /* 512 today; not assumed anywhere          */
    uint64_t    sectors;        /* how big the device is                    */
    /* Move req->sectors sectors, set req->sectors_done and req->result.
     * Returning non-zero without setting a result is treated as MEDIUM - a
     * driver that fails silently is still a failure the caller must see. */
    int  (*submit)(void *ctx, vibeos_blk_request_t *req);
    void *ctx;
} vibeos_blk_driver_t;

/* Forget every device. For tests, and for a re-probe. */
void vibeos_blk_reset(void);

/* Register a device and report the index it was given. Refuses a table with no
 * submit, no name, a zero sector size or a zero length: a device that cannot
 * say how big it is cannot have its requests bounds-checked, and an unchecked
 * bound is the difference between an error and a disk written at the wrong
 * offset. */
int vibeos_blk_register(const vibeos_blk_driver_t *drv, uint32_t *out_device);

uint32_t vibeos_blk_count(void);
int vibeos_blk_info(uint32_t device, vibeos_blk_driver_t *out);
const char *vibeos_blk_name(uint32_t device);

/* Submit one request. Returns 0 when `req->result` is OK, non-zero otherwise -
 * so a caller that only wants to know may test the return, and one that needs
 * to say why reads the result. Both are always set.
 *
 * Validated before the driver sees it: a device that exists, a buffer, at
 * least one sector, and a range inside the device. That last one belongs here
 * rather than in each driver, because a driver that checks its own bounds
 * protects the *device* and cannot protect a neighbouring partition - whose
 * sectors are perfectly valid addresses. */
int vibeos_blk_submit(vibeos_blk_request_t *req);

/* The common cases, so callers do not assemble a struct for a one-sector read.
 * They are wrappers and nothing more; there is one path through the layer. */
int vibeos_blk_read(uint32_t device, uint64_t lba, uint32_t sectors, void *buf);
int vibeos_blk_write(uint32_t device, uint64_t lba, uint32_t sectors,
                     const void *buf);

#endif /* VIBEOS_BLKDEV_H */
