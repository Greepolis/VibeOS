#ifndef VIBEOS_ISO9660_H
#define VIBEOS_ISO9660_H

/* ISO9660, read-only.
 *
 * The cheapest client the filesystem interface will ever have, and worth
 * having early: it is what an installation CD carries, so being able to read
 * one is the difference between a disk image and something a machine can be
 * installed from.
 *
 * It is cheap because a file's extent is contiguous. There is no allocation
 * table, no cluster chain and no indirection - a directory record says where a
 * file starts and how long it is, and that is the whole story. After FAT's
 * chains and ext2's three levels of indirection, this driver is mostly about
 * names.
 *
 * Names are the awkward part. ISO9660 stores them upper case and appends a
 * version suffix, so a file written as "vmlinuz" is on the disc as
 * "VMLINUZ;1". Lookup therefore compares case-insensitively and ignores the
 * suffix; listing reports the name without it, because a name a program cannot
 * pass back to open() is not a useful answer.
 */

#include <stdint.h>

#include "vibeos/blockdev.h"
#include "vibeos/vfs.h"

#define VIBEOS_ISO_SECTOR 2048u
#define VIBEOS_ISO_PVD_SECTOR 16u

typedef struct {
    vibeos_blockcache_t *cache;
    uint64_t part_lba;
    uint32_t root_extent;     /* logical block of the root directory */
    uint32_t root_length;     /* its size in bytes                   */
    int mounted;
} vibeos_iso9660_t;

/* Read the primary volume descriptor. Returns 0 on success. */
int vibeos_iso9660_mount(vibeos_iso9660_t *fs, vibeos_blockcache_t *cache,
                         uint64_t part_lba);

const vibeos_fs_ops_t *vibeos_iso9660_ops(void);

#endif
