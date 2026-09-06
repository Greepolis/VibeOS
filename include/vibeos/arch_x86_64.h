#ifndef VIBEOS_ARCH_X86_64_H
#define VIBEOS_ARCH_X86_64_H

#include "vibeos/blockdev.h"
#include "vibeos/vfs.h"
#include <stdint.h>

#define VIBEOS_X86_64_IDT_ENTRIES 256u
#define VIBEOS_X86_64_TIMER_IRQ 32u
#define VIBEOS_X86_64_FEATURE_SSE2 (1u << 0)
#define VIBEOS_X86_64_FEATURE_NX (1u << 1)

typedef struct vibeos_x86_64_idt {
    uint8_t present[VIBEOS_X86_64_IDT_ENTRIES];
} vibeos_x86_64_idt_t;

int vibeos_x86_64_idt_init(vibeos_x86_64_idt_t *idt);
int vibeos_x86_64_idt_set(vibeos_x86_64_idt_t *idt, uint32_t vector);
int vibeos_x86_64_timer_vector(void);
int vibeos_x86_64_validate_boot_environment(uint32_t feature_flags);

/* Early serial I/O for boot logging */
int vibeos_x86_64_serial_init(void);
void vibeos_x86_64_serial_putc(char c);
void vibeos_x86_64_serial_puts(const char *s);
void vibeos_x86_64_serial_print_hex(uint64_t value);
/* Bracket a multi-part message so other CPUs cannot split it. Recursive. */
void vibeos_x86_64_serial_lock(void);
void vibeos_x86_64_serial_unlock(void);
/* Identity of the calling CPU, used to make the console lock recursive.
 * Weakly defined as 0; the on-metal arch layer overrides it. */
uint32_t vibeos_x86_64_cpu_id(void);

/* The tail of the kernel log that lives on disk, newest first. What survived
 * the previous machine, as opposed to what this one has done since. */
void vibeos_x86_64_logdisk_tail(uint32_t want);
/* Mask and restore interrupts around a console critical section. Weakly
 * defined as no-ops for host builds, which cannot execute cli. */
uint64_t vibeos_x86_64_irq_save(void);
void vibeos_x86_64_irq_restore(uint64_t flags);
int vibeos_x86_64_serial_available(void);
int vibeos_x86_64_serial_can_read(void);
int vibeos_x86_64_serial_readc(void);
/* Raised when the interrupt key arrives on a console. Signals the foreground
 * process group; weak where there is no process to signal. */
void vibeos_x86_64_console_interrupt(void);

/* Mark an identity-mapped physical range uncacheable (device registers). */
void vibeos_x86_64_mark_uncacheable(uint64_t phys, uint64_t len);

/* Fixed-delivery IPI to every core but this one. */
void vibeos_x86_64_lapic_ipi_all_but_self(uint8_t vector);
void vibeos_x86_64_lapic_ipi_one(uint32_t lapic_id, uint8_t vector);

/* Console-lock hygiene: unlock calls from a core that did not hold it. */
uint64_t vibeos_x86_64_serial_bad_unlocks(void);

/* How often the console lock's wait gave up and took the lock anyway, and who
 * held it then. This lock cannot panic - a panic prints, and printing goes
 * through it - so it counts and carries on, and the gate asserts the count. */
uint64_t vibeos_x86_64_serial_stuck(void);
uint32_t vibeos_x86_64_serial_stuck_owner(void);

/* Print the most recent ring-3 crash: registers, fault address, stack. */
void vibeos_x86_64_crash_dump(void);

/* Start init and every service. Called by vibeos_kmain once the kernel is up
 * and has said so; returns when every user task has retired. */
void vibeos_x86_64_hw_start_userland(void);

/* Newest entries of the arch log ring: fork, exec, exit, signals, memory. */
void vibeos_x86_64_log_dump_recent(uint32_t want);

/* The block device the filesystem talks to. Bound by whichever driver came up;
 * see kernel/arch/x86_64/blk.c for why this indirection exists. */
/* `sectors` is how big the device is. A driver that cannot say is not
 * registered, because a device with no size cannot have its requests
 * bounds-checked and an unchecked bound is the difference between an error and
 * a disk written at the wrong offset. */
/* `timeouts` is how the bound firing gets a name. Without it a driver's
 * timeout is a -1 like any other, VIBEOS_BLK_TIMEOUT is produced by nobody,
 * and the gate's assertion that it is zero is one that cannot go red. */
void vibeos_x86_64_blk_bind(const char *name,
                            int (*read)(uint64_t, void *),
                            int (*read_many)(uint64_t, void *, uint32_t),
                            int (*write)(uint64_t, const void *),
                            int (*write_many)(uint64_t, const void *, uint32_t),
                            int (*barrier)(void),
                            uint64_t sectors,
                            uint64_t (*timeouts)(void));

/* Every disk that bound, not just the boot one. The adapter used to refuse the
 * second driver outright, which is why I5's images had to be reached through a
 * loop device and why a log on its own medium was not possible at all. */
uint32_t vibeos_x86_64_blk_adapter_count(void);
int vibeos_x86_64_blk_adapter_device(uint32_t n);
const char *vibeos_x86_64_blk_adapter_name(uint32_t n);
uint64_t vibeos_x86_64_virtio_blk_timeouts(void);
uint64_t vibeos_x86_64_ahci_timeouts(void);
uint64_t vibeos_x86_64_virtio_net_tx_timeouts(void);
uint64_t vibeos_x86_64_blk_timeouts(void);

/* What the block cache under the boot filesystem did.
 *
 * Numbers rather than the struct: vibeos_blockcache_t is an anonymous typedef
 * and cannot be forward-declared, and kmain has no business with the cache's
 * internals - only with what it did. All zero before the volume is mounted.
 *
 * Exposed at all because a cache that never hits and a cache that is not wired
 * in look identical from outside, which is the whole reason phase I2 exists. */
/* The one block cache, so a second reader of this disk uses it rather than
 * standing up its own. Null before the volume is mounted.
 *
 * Declared with a struct pointer the caller must have the definition for; the
 * header for it is vibeos/blockdev.h. */
vibeos_blockcache_t *vibeos_x86_64_fat_cache(void);

/* Let the portable volume scan see this driver. Registration rather than a
 * direct call, because kernel/fs must not depend on kernel/arch. */
void vibeos_x86_64_fat_register_driver(void);

/* Mount a second FAT volume, from a device the caller names.
 *
 * Returns an opaque handle, or null. Everything below takes that handle and
 * acts on the volume it names; passing null means the boot volume, which is
 * what the no-argument spellings above do.
 *
 * One operation at a time across all volumes: this driver serialises through a
 * single lock and always has, so a handle selects a volume rather than making
 * two of them concurrent. That is a limit worth knowing and not a correctness
 * problem - see fat.c for what lifting it would take. */
/* The FAT operations table, for a caller that mounts a volume itself rather
 * than through the scan. Not a `g_` name: it is a function, and the prefix in
 * this kernel means a global. */
const vibeos_fs_ops_t *vibeos_x86_64_fat_ops(void);

/* Attach a file on the boot volume as a read-only block device.
 *
 * The same extent resolution the swap area uses, for reading somebody else's
 * filesystem instead of writing pages. A fragmented file is refused: a loop
 * device that spanned a gap would present another file's bytes as part of the
 * filesystem it is mounting, and the driver above would parse them. */
int vibeos_x86_64_loop_attach(const char *path, uint64_t *out_sectors);
/* Why the last attach failed. "No such file" and "no loop device left" are
 * different facts, and reporting the first for the second cost a boot. */
const char *vibeos_x86_64_loop_why(void);

void *vibeos_x86_64_fat_mount_volume(vibeos_blockcache_t *bc,
                                     uint32_t first_lba);

/* The boot volume's spellings. Every one of these is the matching _on with a
 * null handle, and they exist because everything above this driver still names
 * the boot volume implicitly - the mount table knows about more than one, the
 * syscalls do not yet. */
int vibeos_x86_64_fat_mount(void);
int vibeos_x86_64_fat_list(const char *path, uint32_t idx, char *name,
                           uint32_t *out_size, int *out_is_dir);
long vibeos_x86_64_fat_write_file(const char *path, const void *buf,
                                  uint32_t len);
int vibeos_x86_64_fat_unlink(const char *path);
int vibeos_x86_64_fat_mkdir(const char *path);
long vibeos_x86_64_fat_read_file(const char *path, void *buf, uint32_t bufcap);

int vibeos_x86_64_fat_open_on(void *vol, const char *path,
                              uint32_t *out_cluster, uint32_t *out_size);
long vibeos_x86_64_fat_read_at_on(void *vol, uint32_t first_cluster,
                                  uint32_t size, uint32_t off,
                                  void *buf, uint32_t len);
int vibeos_x86_64_fat_list_on(void *vol, const char *path, uint32_t idx,
                              char *name, uint32_t *out_size, int *out_is_dir);
long vibeos_x86_64_fat_write_file_on(void *vol, const char *path,
                                     const void *buf, uint32_t len);
int vibeos_x86_64_fat_unlink_on(void *vol, const char *path);
int vibeos_x86_64_fat_mkdir_on(void *vol, const char *path);

/* The probe and the formatter, for the boot exercise that partitions a scratch
 * device. Exposed rather than reached for: the exercise lives in arch_hw.c and
 * fat_vfs.c owns these, and a second copy of "what a FAT volume looks like" is
 * the thing the format op exists to prevent. */
extern int (*g_fat_driver_probe)(vibeos_blockcache_t *cache, uint64_t first_lba);
extern int (*g_fat_driver_format)(vibeos_blockcache_t *cache, uint64_t first_lba,
                                  uint64_t sectors);

void vibeos_x86_64_fat_cache_stats(uint64_t *hits, uint64_t *misses,
                                   uint64_t *evictions, uint64_t *evict_failed);

/* Why the last write to this filesystem refused. Every failure in that path
 * used to be a bare -1, so a full disk, a name that is not 8.3, a directory
 * with no free slot and a medium that would not take the sector arrived at the
 * caller as one thing. */
const char *vibeos_x86_64_fat_write_why(void);

/* Multi-sector writes, the mirror of the read_many pair. Added at I4: until
 * then nothing wrote enough sectors for the difference to be measurable, and a
 * boot's entire writing was about thirty of them. */
int vibeos_x86_64_virtio_blk_write_many(uint64_t sector, const void *buf,
                                        uint32_t sectors);
int vibeos_x86_64_ahci_write_many(uint64_t lba, const void *buf,
                                  uint32_t sectors);

/* Make everything already written durable on the medium. Not a cache flush of
 * this kernel's own - see vibeos_blk_barrier for the distinction. */
int vibeos_x86_64_virtio_blk_barrier(void);
int vibeos_x86_64_ahci_barrier(void);

/* Where a file's bytes physically are, for swap and for nothing else. See the
 * comment on the definition: `contiguous` is reported rather than assumed,
 * because a swap area that wrote through a gap would write into other files. */
int vibeos_x86_64_fat_file_extent(const char *path, uint64_t *out_first_lba,
                                  uint64_t *out_sectors, int *out_contiguous);
int vibeos_x86_64_fat_open(const char *path, uint32_t *out_cluster,
                           uint32_t *out_size);
long vibeos_x86_64_fat_read_at(uint32_t first_cluster, uint32_t size,
                               uint32_t off, void *buf, uint32_t len);
const char *vibeos_x86_64_blk_name(void);
int vibeos_x86_64_blk_present(void);
int vibeos_x86_64_blk_read(uint64_t lba, void *buf);
int vibeos_x86_64_blk_read_many(uint64_t lba, void *buf, uint32_t sectors);
int vibeos_x86_64_blk_write(uint64_t lba, const void *buf);

/* How many sectors each driver's device holds, or 0 if it would not say. */
uint64_t vibeos_x86_64_virtio_blk_sectors(void);
uint64_t vibeos_x86_64_ahci_sectors(void);

/* Which device index the bound driver was given in the block layer. Negative
 * when nothing bound. */
int vibeos_x86_64_blk_device(void);

/* AHCI (SATA): what VirtualBox, VMware and real machines provide. */
int vibeos_x86_64_ahci_init(void);
int vibeos_x86_64_ahci_read(uint64_t lba, void *buf);
int vibeos_x86_64_ahci_read_many(uint64_t lba, void *buf, uint32_t sectors);
int vibeos_x86_64_ahci_write(uint64_t lba, const void *buf);

#endif
