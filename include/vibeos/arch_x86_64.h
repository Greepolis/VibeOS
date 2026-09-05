#ifndef VIBEOS_ARCH_X86_64_H
#define VIBEOS_ARCH_X86_64_H

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
                            uint64_t sectors,
                            uint64_t (*timeouts)(void));
uint64_t vibeos_x86_64_virtio_blk_timeouts(void);
uint64_t vibeos_x86_64_ahci_timeouts(void);
uint64_t vibeos_x86_64_virtio_net_tx_timeouts(void);
uint64_t vibeos_x86_64_blk_timeouts(void);
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
