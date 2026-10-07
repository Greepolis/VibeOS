#ifndef VIBEOS_FAT_H
#define VIBEOS_FAT_H

/* The FAT driver (kernel/fs/fat.c and fat_vfs.c).
 *
 * It lived in kernel/arch/x86_64 until docs/abi/ L1 step 4, for no reason but
 * history: it was written against the virtio-blk driver's two functions, and
 * the three things it took from the architecture - the boot disk, a lock, and a
 * line on the serial port at mount - are registrations now. That is what lets
 * the host tests mount a FAT image and drive the driver, and the writer was
 * about to grow the code this project can least afford to verify only by
 * booting: writes at an offset, truncation, rename.
 *
 * One operation at a time across all volumes: the driver serialises through a
 * single lock and always has, so a handle selects a volume rather than making
 * two of them concurrent - see fat.c for what lifting that would take. A null
 * handle is the boot volume. */

#include <stdint.h>

#include "vibeos/blockdev.h"
#include "vibeos/storage.h"

/* ---- what the architecture supplies -------------------------------------------- */

/* The boot disk: one sector in, one out, and a run of sectors in. The boot
 * volume cannot be mounted before this is registered. */
void vibeos_fat_set_boot_device(int (*read)(uint64_t lba, void *buf),
                                int (*write)(uint64_t lba, const void *buf),
                                int (*read_many)(uint64_t lba, void *buf, uint32_t sectors));

/* The driver's lock, taken for the whole of every operation. With none
 * registered - the host tests - operations run unlocked. */
void vibeos_fat_set_lock(void (*lock)(void), void (*unlock)(void));

/* Called when a volume mounts, so the architecture can say so on its console. */
void vibeos_fat_set_mount_hook(void (*hook)(int is_fat32, uint32_t part_lba, uint32_t data_lba));

/* ---- the driver, for a caller that chooses FAT by name --------------------------- */

const vibeos_fs_driver_t *vibeos_fat_fs_driver(void);

/* The one block cache, so a second reader of the boot disk uses it rather than
 * standing up its own. Null before the volume is mounted. */
vibeos_blockcache_t *vibeos_fat_cache(void);
void vibeos_fat_cache_stats(uint64_t *hits, uint64_t *misses,
                            uint64_t *evictions, uint64_t *evict_failed);

/* Mount a second FAT volume, from a device the caller names. An opaque handle,
 * or null. `sectors` is the partition's length, 0 if unknown: a boot sector
 * that claims more than its partition is refused, and nothing outside it is
 * read or written. */
void *vibeos_fat_mount_volume(vibeos_blockcache_t *bc, uint32_t first_lba, uint64_t sectors);

/* Sectors refused because they lay outside the mounted volume. */
uint64_t vibeos_fat_out_of_volume(void);

/* Forget every volume but the boot one. For tests. */
void vibeos_fat_forget_volumes(void);

/* ---- the boot volume's spellings ------------------------------------------------- *
 *
 * Every one of these is the matching _on with a null handle. */
int vibeos_fat_mount(void);
int vibeos_fat_list(const char *path, uint32_t idx, char *name,
                    uint32_t *out_size, int *out_is_dir);
long vibeos_fat_write_file(const char *path, const void *buf, uint32_t len);
int vibeos_fat_unlink(const char *path);
int vibeos_fat_mkdir(const char *path);
long vibeos_fat_read_file(const char *path, void *buf, uint32_t bufcap);
int vibeos_fat_open(const char *path, uint32_t *out_cluster, uint32_t *out_size);
long vibeos_fat_read_at(uint32_t first_cluster, uint32_t size,
                        uint32_t off, void *buf, uint32_t len);

int vibeos_fat_open_on(void *vol, const char *path,
                       uint32_t *out_cluster, uint32_t *out_size);
long vibeos_fat_read_at_on(void *vol, uint32_t first_cluster,
                           uint32_t size, uint32_t off,
                           void *buf, uint32_t len);
int vibeos_fat_list_on(void *vol, const char *path, uint32_t idx,
                       char *name, uint32_t name_cap, uint32_t *out_size,
                       int *out_is_dir);
long vibeos_fat_write_file_on(void *vol, const char *path,
                              const void *buf, uint32_t len);
int vibeos_fat_unlink_on(void *vol, const char *path);
int vibeos_fat_mkdir_on(void *vol, const char *path);

/* Why the last write to this filesystem refused. Every failure in that path
 * used to be a bare -1, so a full disk, a name that is not 8.3, a directory
 * with no free slot and a medium that would not take the sector arrived at the
 * caller as one thing. */
const char *vibeos_fat_write_why(void);

/* Where a file's bytes physically are, for swap and for nothing else. See the
 * comment on the definition: `contiguous` is reported rather than assumed,
 * because a swap area that wrote through a gap would write into other files. */
int vibeos_fat_file_extent(const char *path, uint64_t *out_first_lba,
                           uint64_t *out_sectors, int *out_contiguous);

#endif
