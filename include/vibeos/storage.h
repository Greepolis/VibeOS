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
#include "vibeos/partition.h"
#include "vibeos/vfs.h"

#define VIBEOS_STORAGE_MAX_VOLUMES 8u

/* Room for one mounted filesystem's own state, which the caller supplies - the
 * volume, or whoever mounts a driver by name. The largest today is NTFS's,
 * 4160 bytes because it carries a sector buffer; each driver asserts at compile
 * time that its state fits, so growing one past this is a build error rather
 * than an overrun. */
#define VIBEOS_FS_STATE_BYTES 4608u

typedef struct {
    uint64_t first_lba;
    uint64_t sector_count;       /* the partition's length; bounds every read */
    const char *fs_name;         /* 0 when nothing recognised the volume */
    vibeos_fsmount_t mount;

    /* The mounted driver's own state, whatever driver it is. This used to be
     * one member per filesystem - ext2, ntfs, exfat and iso9660, each named
     * here - which is why adding a filesystem edited this header (C7). */
    uint64_t fs_state[VIBEOS_FS_STATE_BYTES / 8u];
} vibeos_volume_t;

typedef struct {
    vibeos_blockcache_t *cache;
    vibeos_parttable_t table;
    vibeos_volume_t volume[VIBEOS_STORAGE_MAX_VOLUMES];
    uint32_t volume_count;       /* volumes examined */
    uint32_t mounted_count;      /* of those, the ones a driver claimed */
} vibeos_storage_t;

/* A filesystem driver, joining the scan.
 *
 * Every filesystem registers - ext2, ntfs, exfat, iso9660 and FAT alike - and
 * nothing here names one. The four in kernel/fs/ used to be compiled into this
 * file's probe table, each with a mount wrapper here and a member of its own in
 * vibeos_volume_t, so adding a filesystem edited four files; FAT, which lives in
 * the arch layer, was the only one that registered (C7).
 *
 * NTFS and exFAT are tried before FAT, and that is not a preference. Both live
 * in a boot sector that *is* a FAT boot sector with different fields, so a FAT
 * probe checking only the jump instruction and the 0xAA55 signature says yes
 * to an exFAT volume and mounts it wrong - which looks like a working mount
 * until a file comes back as nonsense. The narrower probe goes first, which is
 * what each driver's `order` says.
 */
typedef struct {
    const char *name;
    /* Does this volume look like ours? Reads only; keeps nothing. May be null
     * for a driver whose mount reads its own magic and refuses anything else -
     * then a refusing mount means "not mine", not "mine and broken". */
    int (*probe)(vibeos_blockcache_t *cache, uint64_t first_lba);
    /* Only called after probe said yes, when there is a probe. `sectors` is
     * the volume's length - the authoritative bound on every read the driver
     * makes (H-029) - and `state` is VIBEOS_FS_STATE_BYTES of storage the
     * caller keeps for as long as the mount lives. */
    int (*mount)(vibeos_fsmount_t *out, vibeos_blockcache_t *cache,
                 uint64_t first_lba, uint64_t sectors, void *state);
    /* Put an empty filesystem of this kind on the volume. May be NULL for a
     * driver that cannot create one - iso9660 never will.
     *
     * Here, and not in the volume layer, because a volume layer that wrote a
     * FAT boot sector would be a second place that has to be right about FAT.
     * This project has spent whole phases removing second places, and the two
     * that survived longest - the page cache's knowledge of frames, fat.c's
     * own sector cache - were both found the expensive way. */
    int (*format)(vibeos_blockcache_t *cache, uint64_t first_lba,
                  uint64_t sectors);
    /* Where in the scan this driver is tried; lower first. A decision, not a
     * preference: NTFS and exFAT live in boot sectors that *are* FAT boot
     * sectors, so they must be asked before FAT is. Stated by each driver
     * because the linker section they arrive in has no order of its own. */
    uint32_t order;
} vibeos_fs_driver_t;

/* Every driver the scan knows, in `order`. A small fixed table: this runs at
 * boot on a path that must not allocate. */
#define VIBEOS_STORAGE_MAX_REGISTERED 8u

int vibeos_storage_register(const vibeos_fs_driver_t *drv);

/* A filesystem driver declares itself, in its own file, the way a device does
 * (VIBEOS_DEVICE, include/vibeos/device.h): a pointer in a linker section the
 * arch layer walks and registers at boot. FAT used to need a register function
 * that io_bringup.c called by name, declared in arch_x86_64.h, next to an ops
 * accessor and two exported function pointers for the one caller that mounts a
 * volume itself - four files for a driver that had a registry (C7).
 *
 * ELF only: PE/COFF has no __start_/__stop_ symbols, which is why the portable
 * core takes registrations and never walks the section itself. */
#define VIBEOS_FS_DRIVER(desc) \
    static const vibeos_fs_driver_t *const vibeos_fs_driver_ptr_##desc \
        __attribute__((used, section("vibeos_fs_drivers"))) = &(desc)

/* The registered driver with this name, or null. For a caller that has to
 * choose a filesystem rather than scan for one - the boot volume, which UEFI
 * requires to be FAT, and a volume the boot formats itself. */
const vibeos_fs_driver_t *vibeos_storage_driver(const char *name);

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
