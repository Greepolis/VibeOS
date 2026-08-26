/* MBR and GPT parsing.
 *
 * Pure byte layout over buffers the caller supplies, so the whole thing runs
 * under host tests against fabricated tables. That matters more here than
 * elsewhere: a partition table says where other people's data begins, and a
 * parser that is subtly wrong does not fail, it points somewhere plausible.
 */

#include "vibeos/partition.h"

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

/* CRC-32, the ordinary reflected one, computed without a table. A 256-entry
 * table would be faster and this runs a handful of times per boot. */
uint32_t vibeos_partition_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t i, bit;

    for (i = 0; i < len; i++) {
        crc ^= p[i];
        for (bit = 0; bit < 8u; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

static vibeos_part_kind_t mbr_kind(uint8_t type) {
    switch (type) {
        case 0x01: case 0x04: case 0x06: case 0x0B: case 0x0C: case 0x0E:
            return VIBEOS_PART_FAT;
        case 0x83:
            return VIBEOS_PART_LINUX;
        case 0xEF:
            return VIBEOS_PART_EFI_SYSTEM;
        case 0x05: case 0x0F:
            return VIBEOS_PART_EXTENDED;
        default:
            return VIBEOS_PART_UNKNOWN;
    }
}

int vibeos_partition_parse_mbr(const void *sector0, vibeos_parttable_t *out,
                               int *out_protective) {
    const uint8_t *s = (const uint8_t *)sector0;
    uint32_t i;

    if (!s || !out) {
        return -1;
    }
    out->count = 0;
    out->is_gpt = 0;
    if (out_protective) {
        *out_protective = 0;
    }
    if (rd16(s + 510) != 0xAA55u) {
        return -1;   /* no boot signature: not a partition table */
    }

    for (i = 0; i < 4u; i++) {
        const uint8_t *e = s + 446u + i * 16u;
        uint8_t type = e[4];
        uint32_t first = rd32(e + 8);
        uint32_t count = rd32(e + 12);

        if (type == 0u || count == 0u) {
            continue;   /* an empty slot, which is not the same as the end */
        }
        if (type == 0xEEu) {
            /* The protective entry a GPT disk carries so that older tools see
             * one partition spanning the disk instead of empty space they
             * might offer to fill. Not a partition to report. */
            if (out_protective) {
                *out_protective = 1;
            }
            continue;
        }
        if (out->count >= VIBEOS_PART_MAX) {
            break;
        }
        {
            vibeos_partition_t *p = &out->entry[out->count++];
            uint32_t z;
            p->first_lba = first;
            p->sector_count = count;
            p->mbr_type = type;
            p->kind = mbr_kind(type);
            p->name[0] = 0;
            for (z = 0; z < 16u; z++) {
                p->type_guid[z] = 0;
            }
        }
    }
    return 0;
}

/* The two type GUIDs worth recognising, in on-disk byte order. GPT stores the
 * first three fields little-endian and the rest big-endian, which is why these
 * are written as bytes rather than as a readable GUID: transcribing the text
 * form is where this normally goes wrong. */
static const uint8_t GUID_EFI_SYSTEM[16] = {
    0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
    0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B
};
static const uint8_t GUID_LINUX_DATA[16] = {
    0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
    0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4
};
static const uint8_t GUID_MS_BASIC[16] = {
    0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
    0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7
};

static int guid_eq(const uint8_t *a, const uint8_t *b) {
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static int guid_zero(const uint8_t *g) {
    uint32_t i;
    for (i = 0; i < 16u; i++) {
        if (g[i] != 0u) {
            return 0;
        }
    }
    return 1;
}

int vibeos_partition_parse_gpt(const void *header, const void *entries,
                               uint32_t entries_len, uint64_t disk_sectors,
                               vibeos_parttable_t *out) {
    const uint8_t *h = (const uint8_t *)header;
    const uint8_t *e = (const uint8_t *)entries;
    uint32_t header_size, entry_count, entry_size, stored_crc, computed;
    uint8_t copy[92];
    uint32_t i;

    if (!h || !e || !out) {
        return -1;
    }
    out->count = 0;
    out->is_gpt = 0;

    if (h[0] != 'E' || h[1] != 'F' || h[2] != 'I' || h[3] != ' ' ||
        h[4] != 'P' || h[5] != 'A' || h[6] != 'R' || h[7] != 'T') {
        return -1;
    }
    header_size = rd32(h + 12);
    /* The header CRC covers header_size bytes with the CRC field itself zeroed.
     * Bounding it matters: a crafted header claiming a huge size would other-
     * wise read past the sector it lives in. */
    if (header_size < 92u || header_size > sizeof(copy)) {
        return -1;
    }
    stored_crc = rd32(h + 16);
    for (i = 0; i < header_size; i++) {
        copy[i] = h[i];
    }
    copy[16] = 0; copy[17] = 0; copy[18] = 0; copy[19] = 0;
    if (vibeos_partition_crc32(copy, header_size) != stored_crc) {
        return -1;
    }

    entry_count = rd32(h + 80);
    entry_size = rd32(h + 84);
    if (entry_size < 128u || entry_count == 0u) {
        return -1;
    }
    if (entry_size > entries_len || entry_count > entries_len / entry_size) {
        return -1;   /* the header describes more than the caller supplied */
    }
    computed = vibeos_partition_crc32(e, entry_count * entry_size);
    if (computed != rd32(h + 88)) {
        return -1;
    }

    out->is_gpt = 1;
    for (i = 0; i < entry_count && out->count < VIBEOS_PART_MAX; i++) {
        const uint8_t *ent = e + (uint64_t)i * entry_size;
        uint64_t first = rd64(ent + 32);
        uint64_t last = rd64(ent + 40);
        vibeos_partition_t *p;
        uint32_t z;

        if (guid_zero(ent)) {
            continue;   /* an unused entry; the array is sparse by design */
        }
        if (last < first) {
            return -1;   /* a partition that ends before it starts */
        }
        if (disk_sectors != 0u && last >= disk_sectors) {
            return -1;   /* off the end of the disk */
        }
        p = &out->entry[out->count++];
        p->first_lba = first;
        p->sector_count = last - first + 1u;
        p->mbr_type = 0;
        for (z = 0; z < 16u; z++) {
            p->type_guid[z] = ent[z];
        }
        if (guid_eq(ent, GUID_EFI_SYSTEM)) {
            p->kind = VIBEOS_PART_EFI_SYSTEM;
        } else if (guid_eq(ent, GUID_LINUX_DATA)) {
            p->kind = VIBEOS_PART_LINUX;
        } else if (guid_eq(ent, GUID_MS_BASIC)) {
            p->kind = VIBEOS_PART_FAT;
        } else {
            p->kind = VIBEOS_PART_UNKNOWN;
        }
        /* The name is UTF-16. Anything outside ASCII becomes '?' rather than
         * being truncated at the first wide character - a name is for a human
         * to recognise, and half a name is worse than a marked one. */
        for (z = 0; z < VIBEOS_PART_NAME_MAX; z++) {
            uint16_t wc = rd16(ent + 56u + z * 2u);
            if (wc == 0u) {
                break;
            }
            p->name[z] = (wc < 0x80u) ? (char)wc : '?';
        }
        p->name[z] = 0;
    }
    return 0;
}
