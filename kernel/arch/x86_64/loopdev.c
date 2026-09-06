/* A block device backed by a file on another volume.
 *
 * ## Why this exists
 *
 * I5 asks for ext2, NTFS, ISO9660 and exFAT to be mounted from real images in
 * CI, and says to attach each as a second device. A second *physical* device
 * needs virtio-blk to stop being a singleton - every piece of its state is a
 * global - and that is a refactor of the driver the machine boots from, which
 * is not a thing to do in passing.
 *
 * A file is a device this machine can already describe. The swap area does
 * exactly this: `vibeos_x86_64_fat_file_extent` resolves a path to its sectors
 * and refuses a file that is not one unbroken run, and swap has been writing
 * through it since it was given somewhere to write. The same resolution, used
 * for reading somebody else's filesystem instead.
 *
 * ## What it is not
 *
 * Not a general loopback. There is no offset arithmetic beyond adding the
 * file's first sector, no partition inside the file, and no writes - the
 * images this mounts are built by the host's own mkfs tools and are read to
 * find out whether the drivers work. A writable loop device is a different
 * thing with different failure modes and it can wait until something needs it.
 *
 * ## The one refusal that matters
 *
 * A fragmented file. The extent resolver reports it and this declines, exactly
 * as the swap area does - and for a sharper reason here, because a loop device
 * that spanned a gap would present another file's bytes as part of the
 * filesystem it is mounting, and the driver above would parse them.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"
#include "vibeos/blkdev.h"
#include "vibeos/blockdev.h"

/* Raised from 2 in the change that earned it. I5 attaches four filesystem
 * images, and at 2 the third and fourth were refused - silently, and a silent
 * refusal here is indistinguishable from the image not being on the medium,
 * which is exactly how the two were confused for a boot. */
#define LOOP_MAX 4u

typedef struct {
    uint64_t first_lba;      /* on the backing device */
    uint64_t sectors;
    int      in_use;
} loop_t;

static loop_t g_loop[LOOP_MAX];
static uint32_t g_loop_count;

static int loop_submit(void *ctx, vibeos_blk_request_t *req) {
    loop_t *l = (loop_t *)ctx;
    uint32_t i;

    if (!l || !l->in_use) {
        req->result = VIBEOS_BLK_NO_DEVICE;
        return -1;
    }
    if (req->write) {
        /* Read-only, and refused rather than ignored. A device that accepts a
         * write it does not perform tells the caller the bytes are safe. */
        req->result = VIBEOS_BLK_BAD_REQUEST;
        return -1;
    }
    /* The layer above has already bounds-checked against l->sectors, because
     * that is what this driver declared. This check is against the *backing*
     * device and exists because the two are different numbers: a file that
     * shrank, or an extent that was wrong, would otherwise read whatever
     * follows it. */
    if (req->lba + req->sectors > l->sectors ||
        req->lba + req->sectors < req->lba) {
        req->sectors_done = 0;
        req->result = VIBEOS_BLK_OUT_OF_RANGE;
        return -1;
    }
    for (i = 0; i < req->sectors; i++) {
        uint8_t *p = (uint8_t *)req->buf + (uint64_t)i * 512ull;
        if (vibeos_x86_64_blk_read(l->first_lba + req->lba + i, p) != 0) {
            req->sectors_done = i;
            req->result = VIBEOS_BLK_MEDIUM;
            return -1;
        }
    }
    req->sectors_done = req->sectors;
    return 0;
}

/* Attach `path` on the boot volume as a block device.
 *
 * Returns the device number, or negative. `out_sectors` is how big it turned
 * out to be, which the caller needs to size a cache and to know whether the
 * image is the one it staged.
 */
/* Why the last attach failed.
 *
 * Every refusal here used to be a bare -1, so "the image is not on this
 * medium", "there are no loop slots left" and "the file is fragmented" all
 * arrived at the caller as the same thing - and the caller reported the first
 * of them, because that is the likeliest. It was wrong for a whole boot: four
 * images were staged, two attached, and the machine said the other two were
 * absent while they were sitting on the disk.
 *
 * An absence is quieter than a failure, which is the whole reason this project
 * keeps writing that sentence down. */
static const char *g_loop_why = "not attempted";

const char *vibeos_x86_64_loop_why(void) {
    return g_loop_why;
}

int vibeos_x86_64_loop_attach(const char *path, uint64_t *out_sectors) {
    uint64_t first = 0, sectors = 0;
    int contiguous = 0;
    vibeos_blk_driver_t drv;
    uint32_t device = 0;
    loop_t *l;

    if (g_loop_count >= LOOP_MAX) {
        g_loop_why = "no loop device left";
        return -1;
    }
    if (vibeos_x86_64_fat_file_extent(path, &first, &sectors,
                                      &contiguous) != 0) {
        g_loop_why = "no such file on this medium";
        return -1;
    }
    if (sectors == 0ull) {
        g_loop_why = "the file is empty";
        return -1;
    }
    if (!contiguous) {
        /* The sharpest edge in this device. A loop that spanned a gap would
         * hand the filesystem above it another file bytes, and that filesystem
         * would parse them - a mount that succeeds and is wrong. */
        g_loop_why = "the file is fragmented";
        return -1;
    }
    l = &g_loop[g_loop_count];
    l->first_lba = first;
    l->sectors = sectors;
    l->in_use = 1;

    drv.name = "loop";
    drv.sector_bytes = 512u;
    drv.sectors = sectors;
    drv.submit = loop_submit;
    drv.barrier = 0;         /* read-only: there is nothing to order */
    drv.ctx = l;
    if (vibeos_blk_register(&drv, &device) != 0) {
        l->in_use = 0;
        g_loop_why = "the block layer refused another device";
        return -1;
    }
    g_loop_count++;
    g_loop_why = "ok";
    if (out_sectors) {
        *out_sectors = sectors;
    }
    return (int)device;
}
