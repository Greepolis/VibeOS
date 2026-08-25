/* Reading a file along a FAT cluster chain, with the device factored out.
 *
 * The reader coalesces consecutive clusters into one device request, because
 * the cost of a request dominates: a two-megabyte program read one sector at a
 * time takes about four thousand round trips and minutes of wall time under
 * emulation.
 *
 * Which clusters a request covers, and how much of the last one counts, is
 * arithmetic that depends on the file's layout - so it fails on one file's
 * chain and not another's, which is the hardest kind of bug to catch on
 * hardware. It is kept here, away from the port I/O, so the host test suite
 * can drive it against a fabricated volume, including chains that are not
 * contiguous.
 */

#ifndef VIBEOS_FAT_CHAIN_H
#define VIBEOS_FAT_CHAIN_H

#include <stdint.h>

typedef struct vibeos_fat_chain_io {
    void *ctx;

    /* Bytes in one cluster. */
    uint32_t cluster_bytes;

    /* The chain entry for `cluster`. On failure it must return a value that
     * chain_end() accepts and report the failure some other way - see the
     * note on the return value of vibeos_fat_chain_read(). */
    uint32_t (*next_cluster)(void *ctx, uint32_t cluster);

    /* Whether a chain entry marks the end of the file. */
    int (*chain_end)(void *ctx, uint32_t cluster);

    /* First device sector of a cluster. */
    uint32_t (*cluster_lba)(void *ctx, uint32_t cluster);

    /* Read `sectors` whole sectors starting at `lba` into `dst`. */
    int (*read_sectors)(void *ctx, uint32_t lba, void *dst, uint32_t sectors);

    /* Read the first `bytes` (fewer than one sector) of the sector at `lba`
     * into `dst`. Kept separate so a partial sector never writes past the end
     * of the caller's buffer. */
    int (*read_partial)(void *ctx, uint32_t lba, void *dst, uint32_t bytes);
} vibeos_fat_chain_io_t;

/* Copy `size` bytes of the chain starting at `first_cluster` into `out`.
 *
 * Returns the number of bytes copied, or -1 if a device read failed. A chain
 * that ends before `size` bytes have been copied is not an error here - it
 * returns the short count, and the caller decides what a short file means. */
long vibeos_fat_chain_read(const vibeos_fat_chain_io_t *io, uint32_t first_cluster,
                           uint32_t size, uint8_t *out);

#endif /* VIBEOS_FAT_CHAIN_H */
