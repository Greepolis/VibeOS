#ifndef VIBEOS_SWAPAREA_H
#define VIBEOS_SWAPAREA_H

#include <stdint.h>

/* Where swap lives — a partition, or a file, or nowhere.
 *
 * The swap map (`swapmap.h`) knows how to hand out slots and move 4 KiB in and
 * out of one. It deliberately does not know where a slot *is*: that is a
 * configuration question, and the answer differs between a machine with a
 * dedicated partition and one that has to borrow space inside a filesystem.
 * This layer is the translation, and it is the only place that turns a slot
 * number into a block address.
 *
 * ## The property this layer exists to guarantee
 *
 * **A transfer never touches a block outside the area.**
 *
 * Everything else here is arithmetic. That one is safety: swap sits on the
 * same device as the filesystem, so an area that miscomputes its bounds does
 * not fail - it writes a page of somebody's memory over a directory, and the
 * damage is discovered when the machine next boots. Every conversion below is
 * checked against the area's length before it reaches the device, and a slot
 * that does not fit is refused rather than clamped.
 *
 * That is why the bounds are checked here and not left to the driver. A driver
 * that refuses an out-of-range LBA protects the *disk*; it cannot protect the
 * filesystem, because the filesystem's blocks are perfectly valid addresses.
 *
 * ## Two kinds, and what each costs
 *
 * A **partition** is a range of sectors that belongs to nothing else. Slot n is
 * at `first_sector + n * SECTORS_PER_SLOT`, the translation is one
 * multiplication, and there is no way for the area to move underneath itself.
 *
 * A **file** is space borrowed from a filesystem, which is what a machine with
 * no spare partition has to use. The cost is that a file is not necessarily
 * contiguous, and a swap that assumed it was would write pages into whatever
 * happens to lie between its extents. This layer therefore requires a
 * contiguous file and refuses one that is not, counting the refusal - a
 * restriction that can be checked, rather than a race that has to be reasoned
 * about. Following an extent list is a later change, and it belongs here
 * rather than in the swap map.
 *
 * ## Not configured is a state, not a failure
 *
 * A kernel with no swap area is the normal case today: nothing is registered,
 * reclaim's anonymous tier is absent, and `skipped_no_swap` counts what that
 * costs. Configuring one is a decision somebody makes about a machine, so this
 * layer is asked rather than assumed.
 */

#define VIBEOS_SWAP_SLOT_BYTES   4096u
#define VIBEOS_SWAP_SECTOR_BYTES 512u
#define VIBEOS_SWAP_SECTORS_PER_SLOT \
    (VIBEOS_SWAP_SLOT_BYTES / VIBEOS_SWAP_SECTOR_BYTES)

typedef enum vibeos_swap_kind {
    VIBEOS_SWAP_NONE = 0,
    VIBEOS_SWAP_PARTITION,   /* a range of sectors that belongs to nothing else */
    VIBEOS_SWAP_FILE         /* space borrowed inside a filesystem              */
} vibeos_swap_kind_t;

typedef struct vibeos_swap_area {
    vibeos_swap_kind_t kind;

    /* Which block device. Meaningless when kind is NONE. */
    uint32_t device;

    /* Where the area starts and how long it is, in 512-byte sectors.
     *
     * A file is described the same way rather than by name and offset,
     * deliberately: by the time this layer is configured somebody has already
     * resolved the file to the blocks it occupies, and carrying a path here
     * would mean this layer had to know about filesystems in order to answer
     * "which sector is slot 12". The path is kept only so a diagnostic can say
     * where the area came from. */
    uint64_t first_sector;
    uint64_t sectors;

    /* True when the blocks are known to be one unbroken run. A file that is
     * not is refused: see the header comment. A partition always is. */
    int contiguous;

    /* For the log line, not for the arithmetic. May be null. */
    const char *origin;
} vibeos_swap_area_t;

/* Read or write `count` sectors at `lba` on `device`. The one thing this layer
 * needs from the block layer, supplied rather than called directly so it can be
 * host-tested - the same arrangement as every other layer here. */
typedef int (*vibeos_swap_block_fn)(void *ctx, uint32_t device, uint64_t lba,
                                    uint32_t count, void *buf, int write);

/* Describe the area and wire it to the swap map.
 *
 * Returns the number of slots the area provides, or 0 if it was refused - and
 * the refusal reasons are counted rather than merged, because "there is no
 * swap area" and "the swap area was rejected as unusable" are different states
 * and only one of them is somebody's mistake.
 *
 * `bitmap` must have room for one bit per slot the area could hold; the caller
 * sizes it from the area's length, since this layer never allocates. */
uint32_t vibeos_swaparea_configure(const vibeos_swap_area_t *area,
                                   vibeos_swap_block_fn block, void *ctx,
                                   uint8_t *bitmap, uint32_t bitmap_bytes);

/* How many slots the configured area holds, or 0 when there is none. */
uint32_t vibeos_swaparea_slots(void);

/* The sector a slot begins at, or 0 when the slot is outside the area. Exposed
 * so the bound can be tested directly rather than only through a transfer. */
uint64_t vibeos_swaparea_slot_sector(uint32_t slot);

typedef struct vibeos_swaparea_stats {
    uint64_t refused_no_area;      /* kind was NONE                           */
    uint64_t refused_too_small;    /* not even one slot fits                  */
    uint64_t refused_fragmented;   /* a file whose blocks are not one run     */
    uint64_t refused_no_bitmap;    /* the caller's bitmap is too small        */
    uint64_t out_of_range;         /* a transfer outside the area. MUST BE 0  */
    uint64_t sectors_written;
    uint64_t sectors_read;
} vibeos_swaparea_stats_t;

vibeos_swaparea_stats_t *vibeos_swaparea_stats(void);

#endif /* VIBEOS_SWAPAREA_H */
