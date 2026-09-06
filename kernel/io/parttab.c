/* Writing a partition table. See include/vibeos/parttab.h for the four rules
 * that make it safe and why this is not in partition.c. */

#include "vibeos/parttab.h"

#include <string.h>

const char *vibeos_parttab_result_name(vibeos_parttab_result_t r) {
    switch (r) {
        case VIBEOS_PARTTAB_OK:       return "ok";
        case VIBEOS_PARTTAB_BAD_ARGS: return "bad-args";
        case VIBEOS_PARTTAB_STALE:    return "stale-view";
        case VIBEOS_PARTTAB_IN_USE:   return "in-use";
        case VIBEOS_PARTTAB_OVERLAP:  return "overlapping-entries";
        case VIBEOS_PARTTAB_PAST_END: return "past-the-end-of-the-disk";
        case VIBEOS_PARTTAB_IO:       return "medium-refused";
        default:                      return "?";
    }
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

int vibeos_parttab_checksum(vibeos_blockcache_t *bc, uint64_t disk_sectors,
                            uint32_t *out_checksum) {
    uint8_t sector[VIBEOS_BLOCK_SIZE];

    (void)disk_sectors;
    if (!bc || !out_checksum) {
        return -1;
    }
    /* Sector 0 today. A GPT's header and entry array join this when the GPT
     * writer lands; the function is shaped for it now so the callers that
     * store a checksum do not have to change then. */
    if (vibeos_blockcache_read(bc, 0, sector) != 0) {
        return -1;
    }
    *out_checksum = vibeos_partition_crc32(sector, VIBEOS_BLOCK_SIZE);
    return 0;
}

/* Do two half-open ranges share a sector? */
static int overlaps(uint64_t a_first, uint64_t a_count,
                    uint64_t b_first, uint64_t b_count) {
    uint64_t a_end, b_end;

    if (a_count == 0ull || b_count == 0ull) {
        return 0;
    }
    a_end = a_first + a_count;
    b_end = b_first + b_count;
    /* Overflow is an overlap for this purpose: a range that wraps covers
     * everything, and treating it as "no overlap" is how a crafted table gets
     * past a check that only compares endpoints. */
    if (a_end < a_first || b_end < b_first) {
        return 1;
    }
    return (a_first < b_end) && (b_first < a_end);
}

static vibeos_parttab_result_t check_table(const vibeos_parttable_t *table,
                                           uint64_t disk_sectors,
                                           const vibeos_parttab_guard_t *guard) {
    uint32_t i, j, g;

    if (table->count > 4u) {
        /* An MBR has four slots. More is not a table this function can write,
         * and silently dropping the rest would be a table that does not say
         * what the caller asked for. */
        return VIBEOS_PARTTAB_BAD_ARGS;
    }
    for (i = 0; i < table->count; i++) {
        const vibeos_partition_t *p = &table->entry[i];
        if (p->sector_count == 0ull) {
            return VIBEOS_PARTTAB_BAD_ARGS;
        }
        if (p->first_lba == 0ull) {
            /* Sector 0 is the table itself. A partition that starts there
             * would be overwritten by the very write that creates it. */
            return VIBEOS_PARTTAB_OVERLAP;
        }
        if (disk_sectors != 0ull &&
            (p->first_lba + p->sector_count > disk_sectors ||
             p->first_lba + p->sector_count < p->first_lba)) {
            return VIBEOS_PARTTAB_PAST_END;
        }
        for (j = i + 1u; j < table->count; j++) {
            if (overlaps(p->first_lba, p->sector_count,
                         table->entry[j].first_lba,
                         table->entry[j].sector_count)) {
                return VIBEOS_PARTTAB_OVERLAP;
            }
        }
        if (guard) {
            for (g = 0; g < guard->count && g < VIBEOS_PARTTAB_MAX_INUSE; g++) {
                if (overlaps(p->first_lba, p->sector_count,
                             guard->range[g].first_lba,
                             guard->range[g].sectors)) {
                    return VIBEOS_PARTTAB_IN_USE;
                }
            }
        }
    }
    /* A range in use that no new entry covers is still in use: the write is
     * removing a partition somebody is standing on. Checked separately from
     * the loop above because "the new table overlaps it" and "the new table
     * has dropped it" are different mistakes with the same consequence. */
    if (guard) {
        for (g = 0; g < guard->count && g < VIBEOS_PARTTAB_MAX_INUSE; g++) {
            int covered = 0;
            if (guard->range[g].sectors == 0ull) {
                continue;
            }
            for (i = 0; i < table->count; i++) {
                if (table->entry[i].first_lba == guard->range[g].first_lba &&
                    table->entry[i].sector_count >= guard->range[g].sectors) {
                    covered = 1;
                    break;
                }
            }
            if (!covered) {
                return VIBEOS_PARTTAB_IN_USE;
            }
        }
    }
    return VIBEOS_PARTTAB_OK;
}

vibeos_parttab_result_t vibeos_parttab_write_mbr(
    vibeos_blockcache_t *bc, uint64_t disk_sectors,
    const vibeos_parttable_t *table, const vibeos_parttab_guard_t *guard,
    uint32_t expect_checksum) {
    uint8_t sector[VIBEOS_BLOCK_SIZE];
    uint32_t now = 0;
    vibeos_parttab_result_t rc;
    uint32_t i;

    if (!bc || !table) {
        return VIBEOS_PARTTAB_BAD_ARGS;
    }
    rc = check_table(table, disk_sectors, guard);
    if (rc != VIBEOS_PARTTAB_OK) {
        return rc;
    }
    /* The stale check comes after the table is validated and before anything
     * is written: a caller that hands over a table this layer will refuse
     * should be told why it was refused, not told its view is stale. */
    if (vibeos_parttab_checksum(bc, disk_sectors, &now) != 0) {
        return VIBEOS_PARTTAB_IO;
    }
    if (now != expect_checksum) {
        return VIBEOS_PARTTAB_STALE;
    }

    /* The existing sector is read and edited rather than built from nothing.
     * Sector 0 holds boot code as well as the table, and a writer that
     * replaced the whole sector would silently unbootable a disk it was only
     * asked to repartition. */
    if (vibeos_blockcache_read(bc, 0, sector) != 0) {
        return VIBEOS_PARTTAB_IO;
    }
    for (i = 0; i < 4u; i++) {
        uint8_t *e = &sector[446 + i * 16u];
        memset(e, 0, 16);
        if (i < table->count) {
            const vibeos_partition_t *p = &table->entry[i];
            e[4] = p->mbr_type ? p->mbr_type : 0x0Cu;   /* FAT32 LBA default */
            /* CHS is left at zero. Every reader that matters has used LBA for
             * twenty years, and a wrong CHS is worse than an absent one: a
             * reader that trusts it goes somewhere real and wrong. */
            wr32(&e[8], (uint32_t)p->first_lba);
            wr32(&e[12], (uint32_t)p->sector_count);
        }
    }
    wr16(&sector[510], 0xAA55u);

    if (vibeos_blockcache_write(bc, 0, sector) != 0) {
        return VIBEOS_PARTTAB_IO;
    }
    if (vibeos_blockcache_flush(bc) != 0) {
        /* A table that is only in the cache is a table the next boot will not
         * see, and the caller has been told the disk is partitioned. */
        return VIBEOS_PARTTAB_IO;
    }
    return VIBEOS_PARTTAB_OK;
}
