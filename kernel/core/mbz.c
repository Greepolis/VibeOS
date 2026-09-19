/* Must-be-zero registry. See include/vibeos/mbz.h.
 *
 * No lock, deliberately: every count is a lock-free atomic add, so no increment
 * is lost, and the witness is a plain store - the last writer wins, which is
 * what "the last value that tripped it" means.
 */
#include "vibeos/mbz.h"

static volatile uint64_t g_count[VIBEOS_MBZ_COUNT];
static volatile uint64_t g_witness[VIBEOS_MBZ_COUNT];

static const char *const g_name[VIBEOS_MBZ_COUNT] = {
    "elf_malformed",
    "exfat_bad_metadata",
    "ext2_bad_metadata",
    "fat_chain_bad",
    "iso9660_bad_metadata",
    "ntfs_bad_metadata",
    "partition_bad_table",
    "parttab_bad_table",
    "journal_bad_record",
    "logsink_bad_record",
    "sched_requeue_failed",
};

void vibeos_mbz_hit(vibeos_mbz_id_t id, uint64_t witness) {
    if ((uint32_t)id >= (uint32_t)VIBEOS_MBZ_COUNT) {
        return;
    }
    g_witness[id] = witness;
    __sync_fetch_and_add(&g_count[id], 1u);
}

uint64_t vibeos_mbz_count(vibeos_mbz_id_t id) {
    return ((uint32_t)id < (uint32_t)VIBEOS_MBZ_COUNT) ? g_count[id] : 0u;
}

uint64_t vibeos_mbz_witness(vibeos_mbz_id_t id) {
    return ((uint32_t)id < (uint32_t)VIBEOS_MBZ_COUNT) ? g_witness[id] : 0u;
}

const char *vibeos_mbz_name(vibeos_mbz_id_t id) {
    return ((uint32_t)id < (uint32_t)VIBEOS_MBZ_COUNT) ? g_name[id] : "?";
}

uint64_t vibeos_mbz_total(void) {
    uint64_t sum = 0;
    uint32_t i;
    for (i = 0; i < (uint32_t)VIBEOS_MBZ_COUNT; i++) {
        sum += g_count[i];
    }
    return sum;
}

vibeos_mbz_id_t vibeos_mbz_first(void) {
    uint32_t i;
    for (i = 0; i < (uint32_t)VIBEOS_MBZ_COUNT; i++) {
        if (g_count[i] != 0u) {
            return (vibeos_mbz_id_t)i;
        }
    }
    return VIBEOS_MBZ_COUNT;
}
