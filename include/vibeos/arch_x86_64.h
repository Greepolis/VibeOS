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
uint64_t vibeos_x86_64_blk_timeouts(void);

/* The unmap quarantine: frames parked until every other core has flushed.
 *
 * deferred is the mechanism working; overflow is the residual gap, a frame
 * released the old racy way because the quarantine was full. live_peak sizes
 * the table against reality rather than against a guess. */
uint64_t vibeos_x86_64_tlbq_deferred(void);
uint64_t vibeos_x86_64_tlbq_released(void);
uint64_t vibeos_x86_64_tlbq_overflow(void);
uint64_t vibeos_x86_64_tlbq_live_peak(void);

/* What the block cache under the boot filesystem did.
 *
 * Numbers rather than the struct: vibeos_blockcache_t is an anonymous typedef
 * and cannot be forward-declared, and kmain has no business with the cache's
 * internals - only with what it did. All zero before the volume is mounted.
 *
 * Exposed at all because a cache that never hits and a cache that is not wired
 * in look identical from outside, which is the whole reason phase I2 exists. */
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

/* The FAT driver's own declarations are in vibeos/fat.h since it left this
 * directory (docs/abi/ L1 step 4). */

/* The disk drivers' own functions - read, write, the multi-sector pair, the
 * barrier, their size - used to be declared here, one set per driver. Since C7
 * a disk driver is a registered device (include/vibeos/device.h) and hands them
 * over in a vibeos_block_ops_t; nothing outside its file names them. */

const char *vibeos_x86_64_blk_name(void);
int vibeos_x86_64_blk_present(void);
int vibeos_x86_64_blk_read(uint64_t lba, void *buf);
int vibeos_x86_64_blk_read_many(uint64_t lba, void *buf, uint32_t sectors);
int vibeos_x86_64_blk_write(uint64_t lba, const void *buf);

/* Which device index the bound driver was given in the block layer. Negative
 * when nothing bound. */
int vibeos_x86_64_blk_device(void);
/* Point the no-argument disk path at adapter `n` so a caller can try to mount
 * it, and how many adapters were rejected before one mounted. The boot disk is
 * whichever carries a mountable volume, not whichever bound first - see the
 * note in blk.c. */
int vibeos_x86_64_blk_set_boot(uint32_t n);
uint64_t vibeos_x86_64_blk_boot_rejected(void);
uint32_t vibeos_x86_64_blk_boot_adapter(void);

#endif
