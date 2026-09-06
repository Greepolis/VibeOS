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

/* A driver that does not live in kernel/fs/, joining the scan.
 *
 * The four compiled in here - ext2, ntfs, exfat, iso9660 - are named directly
 * because this file can see them. FAT cannot be: it lives in the arch layer,
 * and kernel/fs depending on kernel/arch would invert the layering the whole
 * storage refactor is about. So it registers instead.
 *
 * That is not a workaround, it is the measurement from I4b steps 1 and 2 being
 * acted on. The scan found a 504 MB volume, correctly identified it as FAT,
 * and reported `fs=none` - because the only FAT driver on the machine was
 * invisible to the code doing the identifying.
 *
 * ## probe and mount are separate, deliberately
 *
 * They were the same function: each driver's "probe" was its mount, and a
 * volume was claimed by whoever mounted it first. Two things wrong with that.
 * A driver that gets half way through a mount and then fails has left state
 * behind that nobody unwinds, and every probe pays a full mount - which for
 * ISO9660 is a real read a long way into the volume.
 *
 * A probe reads and answers. It must not write, and it must not keep anything.
 *
 * ## Order matters and is recorded
 *
 * NTFS and exFAT are tried before FAT, and that is not a preference. Both live
 * in a boot sector that *is* a FAT boot sector with different fields, so a FAT
 * probe checking only the jump instruction and the 0xAA55 signature says yes
 * to an exFAT volume and mounts it wrong - which looks like a working mount
 * until a file comes back as nonsense. The narrower probe goes first.
 */
typedef struct {
    const char *name;
    /* Does this volume look like ours? Reads only; keeps nothing. */
    int (*probe)(vibeos_blockcache_t *cache, uint64_t first_lba);
    /* Only called after probe said yes. */
    int (*mount)(vibeos_fsmount_t *out, vibeos_blockcache_t *cache,
                 uint64_t first_lba);
} vibeos_fs_driver_t;

/* Registered drivers are tried after the compiled-in ones. A small fixed
 * table: this runs at boot on a path that must not allocate. */
#define VIBEOS_STORAGE_MAX_REGISTERED 4u

int vibeos_storage_register(const vibeos_fs_driver_t *drv);

/* Forget every registered driver. For tests. */
void vibeos_storage_reset_drivers(void);

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
