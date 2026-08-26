#ifndef VIBEOS_PARTITION_H
#define VIBEOS_PARTITION_H

/* Partition tables: MBR and GPT.
 *
 * The volume is mounted directly today, which works only because QEMU hands us
 * a bare FAT image. A real disk starts with a table, and every filesystem
 * after the first has to be *found* before it can be mounted - so this is what
 * turns "the disk is a filesystem" into "the disk contains filesystems".
 *
 * Parsing only. Nothing here touches a device: the caller reads the sectors it
 * is asked for and hands them over, which is what lets the whole thing be
 * driven from host tests against fabricated tables. Both formats are pure
 * byte-layout problems, and byte-layout problems are exactly what a boot is
 * bad at telling you about.
 */

#include <stdint.h>

#define VIBEOS_PART_MAX 16u
#define VIBEOS_PART_NAME_MAX 36u   /* GPT names are 36 UTF-16 code units */

/* A partition's kind, reduced to what a mounter cares about. The raw MBR type
 * byte and GPT type GUID are kept too, because a caller that knows more than
 * this enum should not have to re-read the table to act on it. */
typedef enum {
    VIBEOS_PART_UNKNOWN = 0,
    VIBEOS_PART_FAT,
    VIBEOS_PART_EFI_SYSTEM,
    VIBEOS_PART_LINUX,
    VIBEOS_PART_EXTENDED
} vibeos_part_kind_t;

typedef struct {
    uint64_t first_lba;
    uint64_t sector_count;
    vibeos_part_kind_t kind;
    uint8_t mbr_type;          /* 0 for a GPT entry */
    uint8_t type_guid[16];     /* all zero for an MBR entry */
    char name[VIBEOS_PART_NAME_MAX + 1];   /* GPT only; empty for MBR */
} vibeos_partition_t;

typedef struct {
    vibeos_partition_t entry[VIBEOS_PART_MAX];
    uint32_t count;
    int is_gpt;
} vibeos_parttable_t;

/* Parse a 512-byte sector 0 as an MBR. Returns 0 on success.
 *
 * A disk with a GPT still carries an MBR whose single entry has type 0xEE, to
 * stop older tools from believing the disk is empty and offering to help.
 * Recognising that is how the caller knows to go read the GPT, so it is
 * reported through `out_protective` rather than as a partition. */
int vibeos_partition_parse_mbr(const void *sector0, vibeos_parttable_t *out,
                               int *out_protective);

/* Parse a GPT. `header` is LBA 1; `entries` is the entry array the header
 * points at, and `entries_len` its length in bytes. Returns 0 on success.
 *
 * Both CRCs are checked. A table that fails its own checksum is refused rather
 * than used cautiously: the whole point of a partition table is to say where
 * other people's data begins, and being wrong about that writes over it. */
int vibeos_partition_parse_gpt(const void *header, const void *entries,
                               uint32_t entries_len, uint64_t disk_sectors,
                               vibeos_parttable_t *out);

/* CRC-32 as GPT specifies it. Exposed because a caller building a table needs
 * the same function, and two implementations of one checksum is one too many. */
uint32_t vibeos_partition_crc32(const void *data, uint32_t len);

#endif
