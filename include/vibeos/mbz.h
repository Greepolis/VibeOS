#ifndef VIBEOS_MBZ_H
#define VIBEOS_MBZ_H

#include <stdint.h>

/* Must-be-zero counters for modules with no state of their own to report from.
 *
 * C2 (docs/core/phases.md) asks every module to say whether it is broken. The
 * parsers, the journal and the block cache have no stats struct to hang a
 * counter on, and giving each its own would be nine copies of the same eight
 * lines. One registry instead: an id per harm, a count, and the *witness* - the
 * last value that tripped it - because a count says how often and this project's
 * history is that a count alone leaves the question open.
 *
 * Each id is a claim that a healthy boot never reaches that line. The boot gate
 * asserts the total is zero; the host suite runs the same code against damaged
 * input on purpose and asserts every id was seen non-zero at least once
 * (test_mbz_all_demonstrated), which is the proof a counter can move.
 */
typedef enum {
    VIBEOS_MBZ_ELF_MALFORMED = 0,   /* a structurally impossible ELF, not merely the wrong kind */
    VIBEOS_MBZ_EXFAT_BAD_METADATA,
    VIBEOS_MBZ_EXT2_BAD_METADATA,
    VIBEOS_MBZ_FAT_CHAIN_BAD,
    VIBEOS_MBZ_ISO9660_BAD_METADATA,
    VIBEOS_MBZ_NTFS_BAD_METADATA,
    VIBEOS_MBZ_PARTITION_BAD_TABLE,
    VIBEOS_MBZ_PARTTAB_BAD_TABLE,
    VIBEOS_MBZ_JOURNAL_BAD_RECORD,
    VIBEOS_MBZ_LOGSINK_BAD_RECORD,
    VIBEOS_MBZ_SCHED_REQUEUE_FAILED,
    VIBEOS_MBZ_COUNT
} vibeos_mbz_id_t;

/* Record one occurrence. `witness` is whatever names the offender: an error
 * code, a sector, a slot - one number, the last one wins. Safe from any core. */
void vibeos_mbz_hit(vibeos_mbz_id_t id, uint64_t witness);

uint64_t vibeos_mbz_count(vibeos_mbz_id_t id);
uint64_t vibeos_mbz_witness(vibeos_mbz_id_t id);
const char *vibeos_mbz_name(vibeos_mbz_id_t id);

/* Sum over every id, and the lowest id that is non-zero (or VIBEOS_MBZ_COUNT). */
uint64_t vibeos_mbz_total(void);
vibeos_mbz_id_t vibeos_mbz_first(void);

#endif
