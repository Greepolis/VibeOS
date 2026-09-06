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


/* ---- GPT ------------------------------------------------------------------
 *
 * Rule 4 at the top of parttab.h in code. A GPT is five regions, and the order
 * they are written in is the difference between an interrupted write leaving a
 * disk somebody can recover and one leaving a disk nobody can.
 *
 *   backup entry array
 *   backup header
 *   primary entry array
 *   primary header
 *   protective MBR
 *
 * Every prefix of that leaves the disk in a state a reader can name. Stop
 * after the backup array and the original table is still complete and is what
 * a reader finds. Stop after the backup header and there is a newer backup
 * beside an older primary, which is exactly the case GPT keeps a backup for.
 * The primary header comes after the array it describes for the same reason
 * the journal writes its commit record last: a header is a claim about bytes
 * that have to be there already.
 *
 * A flush between each region, because none of that ordering exists otherwise.
 * The block cache is write-back, so issuing writes in order orders nothing -
 * this is the same lesson the journal records, and it is worth stating twice
 * because the code looks correct without the flushes.
 */

static void wr64(uint8_t *p, uint64_t v) {
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

/* The Basic Data type GUID, in on-disk byte order. Matching what
 * partition.c already recognises, rather than inventing a second opinion. */
static const uint8_t g_gpt_basic_data[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

/* One entry array sector, built on demand rather than held in a 16 KiB buffer.
 * This layer runs on a kernel stack. */
static void gpt_array_sector(const vibeos_parttable_t *table, uint32_t sector,
                             const uint8_t disk_guid[16], uint8_t *out) {
    uint32_t per_sector = VIBEOS_BLOCK_SIZE / VIBEOS_PARTTAB_GPT_ENTRY_BYTES;
    uint32_t k;

    memset(out, 0, VIBEOS_BLOCK_SIZE);
    for (k = 0; k < per_sector; k++) {
        uint32_t index = sector * per_sector + k;
        uint8_t *e = out + k * VIBEOS_PARTTAB_GPT_ENTRY_BYTES;
        const vibeos_partition_t *pp;

        if (index >= table->count) {
            continue;   /* an unused entry: all zero, which is how GPT says so */
        }
        pp = &table->entry[index];
        memcpy(e, g_gpt_basic_data, 16);
        /* The unique GUID is derived from the disk GUID and the index rather
         * than invented. This kernel has no entropy source it can honestly
         * call one, and a uuid sliced out of a hash is not a uuid - that
         * mistake cost three attempts at the OVA. Derived and documented beats
         * random and fictional. */
        memcpy(e + 16, disk_guid, 16);
        e[16] = (uint8_t)(e[16] ^ (index + 1u));
        wr64(e + 32, pp->first_lba);
        wr64(e + 40, pp->first_lba + pp->sector_count - 1ull);
        /* attributes zero, name left empty: this layer does not name volumes,
         * for the same reason it does not format them. */
    }
}

/* The entry array CRC, computed by walking the sectors twice rather than
 * holding the whole array: once here, once when writing. */
static uint32_t gpt_array_crc(const vibeos_parttable_t *table,
                              const uint8_t disk_guid[16]) {
    uint8_t sec[VIBEOS_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i;

    for (i = 0; i < VIBEOS_PARTTAB_GPT_ARRAY_SECTORS; i++) {
        gpt_array_sector(table, i, disk_guid, sec);
        crc = vibeos_partition_crc32_update(crc, sec, VIBEOS_BLOCK_SIZE);
    }
    return crc ^ 0xFFFFFFFFu;
}

static void gpt_header(uint8_t *out, uint64_t my_lba, uint64_t alt_lba,
                       uint64_t first_usable, uint64_t last_usable,
                       uint64_t array_lba, uint32_t array_crc,
                       const uint8_t disk_guid[16]) {
    uint32_t crc;

    memset(out, 0, VIBEOS_BLOCK_SIZE);
    memcpy(out, "EFI PART", 8);
    wr32(out + 8, 0x00010000u);      /* revision 1.0 */
    wr32(out + 12, 92u);             /* header size */
    /* out + 16 is the header CRC, left zero while it is computed */
    wr64(out + 24, my_lba);
    wr64(out + 32, alt_lba);
    wr64(out + 40, first_usable);
    wr64(out + 48, last_usable);
    memcpy(out + 56, disk_guid, 16);
    wr64(out + 72, array_lba);
    wr32(out + 80, VIBEOS_PARTTAB_GPT_ENTRIES);
    wr32(out + 84, VIBEOS_PARTTAB_GPT_ENTRY_BYTES);
    wr32(out + 88, array_crc);

    crc = vibeos_partition_crc32(out, 92u);
    wr32(out + 16, crc);
}

/* Write one region and make it durable before the next one is begun. */
static int gpt_put(vibeos_blockcache_t *bc, uint64_t lba, const uint8_t *sec) {
    return vibeos_blockcache_write(bc, lba, sec);
}

vibeos_parttab_result_t vibeos_parttab_write_gpt(
    vibeos_blockcache_t *bc, uint64_t disk_sectors,
    const vibeos_parttable_t *table, const vibeos_parttab_guard_t *guard,
    const uint8_t disk_guid[16], uint32_t expect_checksum) {
    uint8_t sec[VIBEOS_BLOCK_SIZE];
    uint32_t now = 0;
    vibeos_parttab_result_t rc;
    uint32_t array_crc;
    uint64_t array_sectors = VIBEOS_PARTTAB_GPT_ARRAY_SECTORS;
    uint64_t primary_array = 2ull;
    uint64_t backup_header;
    uint64_t backup_array;
    uint64_t first_usable;
    uint64_t last_usable;
    uint32_t i;

    if (!bc || !table || !disk_guid) {
        return VIBEOS_PARTTAB_BAD_ARGS;
    }
    /* Two headers, two arrays, a protective MBR and at least one usable
     * sector. A disk too small for its own table is refused here rather than
     * discovered by arithmetic that wraps. */
    if (disk_sectors < 2ull * array_sectors + 4ull) {
        return VIBEOS_PARTTAB_BAD_ARGS;
    }
    backup_header = disk_sectors - 1ull;
    backup_array = backup_header - array_sectors;
    first_usable = primary_array + array_sectors;
    last_usable = backup_array - 1ull;

    rc = check_table(table, disk_sectors, guard);
    if (rc != VIBEOS_PARTTAB_OK) {
        return rc;
    }
    /* A GPT reserves both ends of the disk, so an entry that would be legal on
     * an MBR can still land on the table itself. Checked separately rather
     * than folded into check_table, which has no idea a GPT exists. */
    for (i = 0; i < table->count; i++) {
        const vibeos_partition_t *pp = &table->entry[i];
        if (pp->first_lba < first_usable ||
            pp->first_lba + pp->sector_count - 1ull > last_usable) {
            return VIBEOS_PARTTAB_OVERLAP;
        }
    }
    if (vibeos_parttab_checksum(bc, disk_sectors, &now) != 0) {
        return VIBEOS_PARTTAB_IO;
    }
    if (now != expect_checksum) {
        return VIBEOS_PARTTAB_STALE;
    }

    array_crc = gpt_array_crc(table, disk_guid);

    /* 1. The backup array. */
    for (i = 0; i < array_sectors; i++) {
        gpt_array_sector(table, i, disk_guid, sec);
        if (gpt_put(bc, backup_array + i, sec) != 0) {
            return VIBEOS_PARTTAB_IO;
        }
    }
    if (vibeos_blockcache_flush(bc) != 0) {
        return VIBEOS_PARTTAB_IO;
    }

    /* 2. The backup header, which points at the array just written. */
    gpt_header(sec, backup_header, 1ull, first_usable, last_usable,
               backup_array, array_crc, disk_guid);
    if (gpt_put(bc, backup_header, sec) != 0 ||
        vibeos_blockcache_flush(bc) != 0) {
        return VIBEOS_PARTTAB_IO;
    }

    /* 3. The primary array. */
    for (i = 0; i < array_sectors; i++) {
        gpt_array_sector(table, i, disk_guid, sec);
        if (gpt_put(bc, primary_array + i, sec) != 0) {
            return VIBEOS_PARTTAB_IO;
        }
    }
    if (vibeos_blockcache_flush(bc) != 0) {
        return VIBEOS_PARTTAB_IO;
    }

    /* 4. The primary header. */
    gpt_header(sec, 1ull, backup_header, first_usable, last_usable,
               primary_array, array_crc, disk_guid);
    if (gpt_put(bc, 1ull, sec) != 0 || vibeos_blockcache_flush(bc) != 0) {
        return VIBEOS_PARTTAB_IO;
    }

    /* 5. The protective MBR, last. Its only job is to stop an MBR-only tool
     * from believing the disk is empty, so it is the least urgent thing here -
     * and writing it first would tell such a tool the disk is spoken for
     * before it actually is.
     *
     * Read and edited, not built: sector 0 holds boot code, and the rule that
     * a repartition must not unbootable a disk applies to a GPT too. */
    if (vibeos_blockcache_read(bc, 0, sec) != 0) {
        return VIBEOS_PARTTAB_IO;
    }
    memset(&sec[446], 0, 64);
    sec[446 + 4] = 0xEEu;                       /* protective type */
    wr32(&sec[446 + 8], 1u);                    /* starts at LBA 1 */
    wr32(&sec[446 + 12], (disk_sectors - 1ull > 0xFFFFFFFFull)
                        ? 0xFFFFFFFFu : (uint32_t)(disk_sectors - 1ull));
    wr16(&sec[510], 0xAA55u);
    if (gpt_put(bc, 0ull, sec) != 0 || vibeos_blockcache_flush(bc) != 0) {
        return VIBEOS_PARTTAB_IO;
    }
    return VIBEOS_PARTTAB_OK;
}
