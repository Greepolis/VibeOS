/* Disk bring-up. See include/vibeos/storage.h for why this file exists. */

#include "vibeos/storage.h"

#include <string.h>

/* GPT's entry array can be large; only the first sectors of it are read, which
 * is enough for the entry count this build supports and keeps the scan off a
 * multi-kilobyte stack buffer. */
#define STORAGE_GPT_ENTRY_SECTORS 4u

/* Every filesystem driver, kept in `order`. */
static const vibeos_fs_driver_t *g_registered[VIBEOS_STORAGE_MAX_REGISTERED];
static uint32_t g_registered_count;

int vibeos_storage_register(const vibeos_fs_driver_t *drv)
{
    /* A probe is optional - a driver whose mount refuses anything it does not
     * recognise needs none - but a mount is not: a driver the scan cannot
     * mount through is refused whole. */
    if (drv == 0 || drv->name == 0 || drv->mount == 0) {
        return -1;
    }
    if (g_registered_count >= VIBEOS_STORAGE_MAX_REGISTERED) {
        return -1;
    }
    /* Inserted in order, after any of equal order, so the table is the scan's
     * order whatever order the linker - or a test - registered them in. */
    {
        uint32_t at = g_registered_count;

        while (at > 0u && g_registered[at - 1u]->order > drv->order) {
            g_registered[at] = g_registered[at - 1u];
            at--;
        }
        g_registered[at] = drv;
        g_registered_count++;
    }
    return 0;
}

const vibeos_fs_driver_t *vibeos_storage_driver(const char *name)
{
    uint32_t i, k;

    if (name == 0) {
        return 0;
    }
    for (i = 0; i < g_registered_count; i++) {
        const char *n = g_registered[i]->name;
        for (k = 0; n[k] != 0 && n[k] == name[k]; k++) {
        }
        if (n[k] == 0 && name[k] == 0) {
            return g_registered[i];
        }
    }
    return 0;
}

void vibeos_storage_reset_drivers(void)
{
    g_registered_count = 0;
}

static void storage_probe_volume(vibeos_volume_t *v, vibeos_blockcache_t *bc)
{
    uint32_t i;

    v->fs_name = 0;
    /* In the table's order, which is each driver's `order`: NTFS and exFAT
     * live in boot sectors that *are* FAT boot sectors with different fields,
     * so a FAT probe that checks only the jump and the signature claims an
     * exFAT volume and mounts it wrong. The narrower probe has to go first. */
    for (i = 0; i < g_registered_count; i++) {
        const vibeos_fs_driver_t *d = g_registered[i];

        if (d->probe && d->probe(bc, v->first_lba) != 0) {
            continue;
        }
        /* Zeroed before every attempt: the drivers share this storage now,
         * and a refused mount leaves whatever it had filled in - so the next
         * driver would start from the last one's fields, which is the trap of
         * a struct filled field by field. Each used to get a member of its
         * own, zero from the start. */
        memset(v->fs_state, 0, sizeof(v->fs_state));
        if (d->mount(&v->mount, bc, v->first_lba, v->sector_count,
                     v->fs_state) == 0) {
            v->fs_name = d->name;
            return;
        }
        /* A driver with a probe that said yes and then would not mount is
         * worth separating from "nobody claimed it": one is a volume of a kind
         * nothing here implements, the other a driver that recognised its own
         * filesystem and failed on it, which is a defect. A driver with no
         * probe refuses through its mount, and that is simply "not mine". */
        v->fs_name = 0;
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
    /* The storage struct keeps this pointer for as long as it is used, so the
     * cache has to outlive it. That is the contract the whole block layer is
     * built on - the caller supplies the storage, so this imposes no allocator
     * on the kernel - but it was nowhere in the code, only in a header comment
     * about something else.
     *
     * CodeQL is right to flag it: today the only caller is a host test that
     * passes a stack-local cache, and it happens to outlive the scan. A real
     * caller that does not would corrupt quietly, and nothing here would say
     * so. Written down until the ownership is expressed in the types. */
    st->cache = cache;

    if (storage_read_table(st, disk_sectors) != 0) {
        return -1;
    }

    if (st->table.count == 0u) {
        st->volume_count = 1;
        st->volume[0].first_lba = 0;
        st->volume[0].sector_count = disk_sectors;
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
            st->volume[st->volume_count].sector_count = st->table.entry[i].sector_count;
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
