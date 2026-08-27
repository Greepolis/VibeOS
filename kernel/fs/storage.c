/* Disk bring-up. See include/vibeos/storage.h for why this file exists. */

#include "vibeos/storage.h"

#include <string.h>

/* GPT's entry array can be large; only the first sectors of it are read, which
 * is enough for the entry count this build supports and keeps the scan off a
 * multi-kilobyte stack buffer. */
#define STORAGE_GPT_ENTRY_SECTORS 4u

static int storage_try_ext2(vibeos_volume_t *v, vibeos_blockcache_t *bc)
{
    if (vibeos_ext2_mount(&v->ext2, bc, v->first_lba) != 0) {
        return -1;
    }
    return vibeos_fs_mount(&v->mount, vibeos_ext2_ops(), &v->ext2, "ext2");
}

static int storage_try_ntfs(vibeos_volume_t *v, vibeos_blockcache_t *bc)
{
    if (vibeos_ntfs_mount(&v->ntfs, bc, v->first_lba) != 0) {
        return -1;
    }
    return vibeos_fs_mount(&v->mount, vibeos_ntfs_ops(), &v->ntfs, "ntfs");
}

static int storage_try_exfat(vibeos_volume_t *v, vibeos_blockcache_t *bc)
{
    if (vibeos_exfat_mount(&v->exfat, bc, v->first_lba) != 0) {
        return -1;
    }
    return vibeos_fs_mount(&v->mount, vibeos_exfat_ops(), &v->exfat, "exfat");
}

static int storage_try_iso(vibeos_volume_t *v, vibeos_blockcache_t *bc)
{
    if (vibeos_iso9660_mount(&v->iso, bc, v->first_lba) != 0) {
        return -1;
    }
    return vibeos_fs_mount(&v->mount, vibeos_iso9660_ops(), &v->iso, "iso9660");
}

typedef int (*storage_probe_fn)(vibeos_volume_t *v, vibeos_blockcache_t *bc);

static const struct {
    const char *name;
    storage_probe_fn probe;
} g_probes[] = {
    /* NTFS and exFAT first: both live in a FAT-shaped boot sector and are the
     * two most likely to be mistaken for something else, so if either of them
     * is going to claim a volume it should do it where the test can see it.
     * ISO9660 last - its descriptor sits far enough into the volume that a
     * probe costs a real read. */
    { "ntfs", storage_try_ntfs },
    { "exfat", storage_try_exfat },
    { "ext2", storage_try_ext2 },
    { "iso9660", storage_try_iso },
};

static void storage_probe_volume(vibeos_volume_t *v, vibeos_blockcache_t *bc)
{
    uint32_t i;

    v->fs_name = 0;
    for (i = 0; i < sizeof(g_probes) / sizeof(g_probes[0]); i++) {
        if (g_probes[i].probe(v, bc) == 0) {
            v->fs_name = g_probes[i].name;
            return;
        }
    }
}

static int storage_read_table(vibeos_storage_t *st, uint64_t disk_sectors)
{
    uint8_t sector[VIBEOS_BLOCK_SIZE];
    uint8_t header[VIBEOS_BLOCK_SIZE];
    static uint8_t entries[STORAGE_GPT_ENTRY_SECTORS * VIBEOS_BLOCK_SIZE];
    int protective = 0;
    uint32_t i;

    if (vibeos_blockcache_read(st->cache, 0, sector) != 0) {
        return -1;
    }
    if (vibeos_partition_parse_mbr(sector, &st->table, &protective) != 0) {
        /* No table at all. A bare filesystem from sector zero is ordinary on
         * removable media, so it is examined as one volume rather than
         * refused. */
        st->table.count = 0;
        return 0;
    }
    if (!protective) {
        return 0;
    }

    /* A protective MBR means the real table is a GPT, and the GPT's checks are
     * against a disk size - without one there is nothing to check them with. */
    if (disk_sectors == 0u) {
        st->table.count = 0;
        return 0;
    }
    if (vibeos_blockcache_read(st->cache, 1, header) != 0) {
        return -1;
    }
    for (i = 0; i < STORAGE_GPT_ENTRY_SECTORS; i++) {
        if (vibeos_blockcache_read(st->cache, 2u + i,
                                   entries + (size_t)i * VIBEOS_BLOCK_SIZE) != 0) {
            return -1;
        }
    }
    if (vibeos_partition_parse_gpt(header, entries, sizeof(entries),
                                   disk_sectors, &st->table) != 0) {
        /* A table that fails its own checksum says where other people's data
         * begins and is wrong about it. Treating the disk as unpartitioned is
         * the conservative reading, and the volume at LBA 0 will simply fail
         * to mount if there is nothing there. */
        st->table.count = 0;
    }
    return 0;
}

int vibeos_storage_scan(vibeos_storage_t *st, vibeos_blockcache_t *cache,
                        uint64_t disk_sectors)
{
    uint32_t i;

    if (st == 0 || cache == 0) {
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->cache = cache;

    if (storage_read_table(st, disk_sectors) != 0) {
        return -1;
    }

    if (st->table.count == 0u) {
        st->volume_count = 1;
        st->volume[0].first_lba = 0;
        storage_probe_volume(&st->volume[0], cache);
    } else {
        for (i = 0; i < st->table.count && i < VIBEOS_STORAGE_MAX_VOLUMES; i++) {
            /* An extended partition is a container for others, not a place a
             * filesystem lives; probing it reads the logical partition's boot
             * record and would be a needless way to find nothing. */
            if (st->table.entry[i].kind == VIBEOS_PART_EXTENDED) {
                continue;
            }
            st->volume[st->volume_count].first_lba = st->table.entry[i].first_lba;
            storage_probe_volume(&st->volume[st->volume_count], cache);
            st->volume_count++;
        }
    }

    for (i = 0; i < st->volume_count; i++) {
        if (st->volume[i].fs_name != 0) {
            st->mounted_count++;
        }
    }
    return 0;
}

vibeos_fsmount_t *vibeos_storage_first(vibeos_storage_t *st)
{
    uint32_t i;

    if (st == 0) {
        return 0;
    }
    for (i = 0; i < st->volume_count; i++) {
        if (st->volume[i].fs_name != 0) {
            return &st->volume[i].mount;
        }
    }
    return 0;
}
