/* Reading a file along a FAT cluster chain. See include/vibeos/fat_chain.h. */

#include "vibeos/fat_chain.h"

#define FAT_SECTOR_SIZE 512u

/* Clusters still needed to cover `remaining` bytes.
 *
 * This is the only bound a run needs, and it is the whole reason this is a
 * named function: the bound used to be an inline byte comparison carrying a
 * spare sector of slack, and working out whether that slack could let a run
 * reach past the end of a file took an exhaustive sweep of chain layouts.
 * Counting clusters needs no such argument. */
static uint32_t fat_clusters_needed(uint32_t remaining, uint32_t cluster_bytes) {
    if (cluster_bytes == 0u || remaining == 0u) {
        return 0u;
    }
    /* Written so the ceiling division cannot wrap: `remaining` comes from a
     * directory entry and is not trusted to be small. */
    return (remaining - 1u) / cluster_bytes + 1u;
}

/* Bytes to take from a run: what the run holds, or what is left, whichever is
 * less. */
static uint32_t fat_run_bytes(uint32_t run, uint32_t cluster_bytes, uint32_t remaining) {
    uint32_t held;

    if (run == 0u || cluster_bytes == 0u) {
        return 0u;
    }
    /* A run long enough to overflow this product would have to be longer than
     * the volume. Clamping beats wrapping: a wrapped length becomes a device
     * request for the wrong number of sectors. */
    if (run > 0xFFFFFFFFu / cluster_bytes) {
        return remaining;
    }
    held = run * cluster_bytes;
    return (held < remaining) ? held : remaining;
}

long vibeos_fat_chain_read(const vibeos_fat_chain_io_t *io, uint32_t first_cluster,
                           uint32_t size, uint8_t *out) {
    uint32_t cluster, copied = 0;
    uint32_t sectors_per_cluster;

    if (!io || !io->next_cluster || !io->chain_end || !io->cluster_lba ||
        !io->read_sectors || !io->read_partial) {
        return -1;
    }
    if (io->cluster_bytes == 0u || (io->cluster_bytes % FAT_SECTOR_SIZE) != 0u) {
        return -1;
    }
    if (size == 0u) {
        return 0;
    }
    if (!out) {
        return -1;
    }
    sectors_per_cluster = io->cluster_bytes / FAT_SECTOR_SIZE;

    cluster = first_cluster;
    while (!io->chain_end(io->ctx, cluster) && cluster >= 2u && copied < size) {
        uint32_t first = cluster;
        uint32_t run = 1u;
        uint32_t needed = fat_clusters_needed(size - copied, io->cluster_bytes);
        uint32_t next = io->next_cluster(io->ctx, cluster);
        uint32_t lba = io->cluster_lba(io->ctx, first);
        uint32_t run_bytes, whole, tail;

        /* Extend the run while the chain stays consecutive and the file still
         * has clusters to fill. A fragmented chain simply stops here with
         * run == 1 and is read one cluster per request. */
        while (run < needed && !io->chain_end(io->ctx, next) && next == first + run) {
            run++;
            next = io->next_cluster(io->ctx, first + run - 1u);
        }

        run_bytes = fat_run_bytes(run, io->cluster_bytes, size - copied);
        whole = run_bytes / FAT_SECTOR_SIZE;
        tail = run_bytes % FAT_SECTOR_SIZE;
        /* whole is bounded by the run's own sectors, so a request never
         * reaches into a cluster the file does not own. */
        if (whole > run * sectors_per_cluster) {
            return -1;
        }
        if (whole > 0u && io->read_sectors(io->ctx, lba, out + copied, whole) != 0) {
            return -1;
        }
        copied += whole * FAT_SECTOR_SIZE;
        if (tail > 0u && io->read_partial(io->ctx, lba + whole, out + copied, tail) != 0) {
            return -1;
        }
        copied += tail;
        cluster = next;
    }
    return (long)copied;
}
