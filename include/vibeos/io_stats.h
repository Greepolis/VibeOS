#ifndef VIBEOS_IO_STATS_H
#define VIBEOS_IO_STATS_H

#include <stdint.h>

#include "vibeos/blkdev.h"

/* The storage path carries no counters at all today. Not few — none. The
 * memory manager carries about twenty, most of them asserted by the boot gate,
 * and the difference is why "the disk is slow", "the disk is retrying" and
 * "the disk is fine" are the same silence.
 *
 * The struct is complete from the start, including fields for layers that do
 * not exist yet. The memory-manager plan did this and it was right: filling in
 * a field later is cheap, migrating a structure and every assertion that reads
 * it is not.
 *
 * ## Every counter is asserted or diagnostic, and it says which
 *
 * A number that is written and never read is not observability. Three
 * subsystems here have shipped one - the scheduler's time slice, the memory
 * watermarks, and reclaim's anonymous tier - each correct in isolation and
 * wired to nothing. So each field below is marked, and a field that is neither
 * should not be added.
 */
typedef struct vibeos_io_stats {
    /* ---- B1: devices ---------------------------------------------------- */
    uint64_t reads;                 /* asserted non-zero                     */
    uint64_t writes;                /* asserted non-zero once I4 lands       */
    uint64_t sectors_read;          /* asserted non-zero                     */
    uint64_t sectors_written;       /* diagnostic until I4                   */

    /* By reason, so "the disk is missing" and "the disk is broken" are
     * different numbers. The gate asserts zero for MEDIUM, SHORT and TIMEOUT;
     * NO_DEVICE is a configuration and only diagnostic. */
    uint64_t results[VIBEOS_BLK_RESULT_COUNT];

    /* How close a bound came to firing, which is the number that says a bound
     * is too tight *before* it fires. This project has tuned a bound twice by
     * watching failures instead of margins: virtio-net went 50M, then 2M which
     * broke the network, then 20M. Diagnostic. */
    uint64_t max_wait_iterations;

    uint64_t requests_in_flight_peak;   /* diagnostic; sizes I6's queue      */
    uint64_t devices_registered;        /* diagnostic                        */
    uint64_t register_refused;          /* asserted zero: a driver that
                                         * cannot describe itself is a bug   */

    /* ---- B2: the block cache (I2) --------------------------------------- */
    uint64_t cache_hits;            /* asserted as a *ratio*, not non-zero   */
    uint64_t cache_misses;
    uint64_t cache_evictions;       /* diagnostic                            */
    uint64_t cache_resident;        /* diagnostic                            */
    uint64_t cache_wrong_key;       /* asserted zero - the page cache once
                                     * returned another file's frames        */
    uint64_t write_back_pending;    /* diagnostic                            */
    uint64_t write_back_failed;     /* asserted zero: a silent write-back
                                     * failure is a lost file                */

    /* ---- B3: volumes (I4b) ---------------------------------------------- */
    uint64_t volumes_found;         /* asserted non-zero once I4b lands      */
    uint64_t mounts;                /* asserted non-zero                     */
    uint64_t probe_rejected;        /* diagnostic: which said no to what     */
    uint64_t table_writes_refused;  /* asserted zero unless a test caused it */

    /* ---- B4: files ------------------------------------------------------ */
    uint64_t file_reads;            /* asserted non-zero                     */
    uint64_t file_bytes_read;       /* asserted non-zero                     */
    uint64_t file_writes;           /* asserted non-zero once I4 lands       */
    uint64_t file_bytes_written;
    /* The defect this project already had: a short read reported as a
     * complete file, so execve parsed the previous program's bytes. Asserted
     * zero. */
    uint64_t short_reads;
} vibeos_io_stats_t;

vibeos_io_stats_t *vibeos_io_stats(void);
void vibeos_io_stats_reset(void);

#endif /* VIBEOS_IO_STATS_H */
