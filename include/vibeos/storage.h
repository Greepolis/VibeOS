#ifndef VIBEOS_STORAGE_H
#define VIBEOS_STORAGE_H

/* Disk bring-up: from a block device to a mounted filesystem, once.
 *
 * Every piece below this already existed and was tested on the host - the
 * cache, the partition parsers, five filesystem drivers - and none of it was
 * reachable at runtime, because nothing joined them up. The kernel mounted FAT
 * through a path of its own and the other four drivers were dead weight in the
 * image. This is the joining up, and it is deliberately the only place that
 * knows the list of filesystems exists.
 *
 * Probing is safe because refusing is cheap and mounting is not: every driver's
 * mount reads its own magic and geometry and returns non-zero for a volume it
 * does not fully understand. So the order of the table below is a preference,
 * not a correctness argument - a driver that claimed a volume it could not read
 * would be a bug in that driver, and there is a test that says so.
 */

#include <stdint.h>

#include "vibeos/blockdev.h"
#include "vibeos/exfat.h"
#include "vibeos/ext2.h"
#include "vibeos/iso9660.h"
#include "vibeos/ntfs.h"
#include "vibeos/partition.h"
#include "vibeos/vfs.h"

#define VIBEOS_STORAGE_MAX_VOLUMES 8u

typedef struct {
    uint64_t first_lba;
    const char *fs_name;         /* 0 when nothing recognised the volume */
    vibeos_fsmount_t mount;

    /* One driver's state per volume. Which member is live is decided by
     * `fs_name`; they are kept apart rather than in a union so that a stray
     * pointer into a dead driver's state is a bug that shows up as wrong data
     * rather than as another driver's fields. */
    vibeos_ext2_t ext2;
    vibeos_ntfs_t ntfs;
    vibeos_exfat_t exfat;
    vibeos_iso9660_t iso;
} vibeos_volume_t;

typedef struct {
    vibeos_blockcache_t *cache;
    vibeos_parttable_t table;
    vibeos_volume_t volume[VIBEOS_STORAGE_MAX_VOLUMES];
    uint32_t volume_count;       /* volumes examined */
    uint32_t mounted_count;      /* of those, the ones a driver claimed */
} vibeos_storage_t;

/* Read the partition table and mount what can be mounted.
 *
 * A disk with no partition table is not an error: plenty of images are a bare
 * filesystem from sector zero, and refusing those would mean refusing the
 * common case for removable media. Such a disk is examined as a single volume
 * at LBA 0.
 *
 * Returns 0 when the scan itself completed, whether or not anything mounted -
 * a disk full of filesystems nobody here implements is a fact about the disk,
 * not a failure of the scan. `mounted_count` is what a caller checks.
 * `disk_sectors` may be 0 when the size is unknown; GPT is then not parsed,
 * because its checks are against a size. */
int vibeos_storage_scan(vibeos_storage_t *st, vibeos_blockcache_t *cache,
                        uint64_t disk_sectors);

/* The first mounted volume, or 0. What a kernel uses as its root when it has
 * no better opinion. */
vibeos_fsmount_t *vibeos_storage_first(vibeos_storage_t *st);

#endif
