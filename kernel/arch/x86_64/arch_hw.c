/* Real x86_64 hardware bring-up: GDT, IDT and CPU exception handling.
 *
 * This is the on-metal counterpart to the host-modeled idt.c. It is compiled
 * only into the freestanding kernel image (vibeos_kernel), never into the
 * host test build, so the portable subsystem model stays testable while the
 * actual descriptor tables run under QEMU/hardware.
 *
 * Milestone 1 scope: install a minimal flat GDT (kernel CS=0x08, DS=0x10),
 * install a full 256-entry IDT wired to assembly stubs, and route CPU
 * exceptions 0-31 into a C handler that reports over the serial console.
 * Hardware IRQ delivery (PIC/APIC) is intentionally not enabled yet.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"
#include "vibeos/trap.h"
#include "vibeos/boot.h"
#include "vibeos/mm.h"
#include "vibeos/inet.h"
#include "vibeos/elf.h"
#include "vibeos/services.h"
#include "vibeos/exec_stats.h"
#include "arch_hw_internal.h"
#include "vibeos/account.h"
#include "vibeos/forkguard.h"
#include "vibeos/sched_policy.h"
#include "vibeos/pageinfo.h"
#include "vibeos/rmap.h"
#include "vibeos/reclaim.h"
#include "vibeos/mbz.h"
#include "vibeos/abi.h"
#include "vibeos/abi_linux.h"
#include "vibeos/ceildiv.h"
#include "vibeos/blkdev.h"
#include "vibeos/io_stats.h"
#include "vibeos/blockdev.h"
#include "vibeos/partition.h"
#include "vibeos/parttab.h"
#include "vibeos/ext2.h"
#include "vibeos/iso9660.h"
#include "vibeos/exfat.h"
#include "vibeos/ntfs.h"
#include "vibeos/logsink.h"
#include "vibeos/storage.h"
#include "vibeos/swapmap.h"
#include "vibeos/anon.h"
#include "vibeos/swaparea.h"
#define VIBEOS_HW_IDT_GATES 256u
#define VIBEOS_HW_GATE_INTERRUPT 0x8Eu /* present, DPL=0, 64-bit interrupt gate */
/* 0-31 CPU exceptions, 32-47 the legacy lines, 48-63 the vectors the device
 * registry hands out (C7). One stub each in isr.S. */
#define VIBEOS_HW_WIRED_VECTORS 64u

/* 8259 PIC and 8253/8254 PIT ports. */
#define PIC1_CMD 0x20u
#define PIC1_DATA 0x21u
#define PIC2_CMD 0xA0u
#define PIC2_DATA 0xA1u
#define PIC_EOI 0x20u
#define PIT_CH0 0x40u
#define PIT_CMD 0x43u
#define PIT_BASE_HZ 1193182u
#define VIBEOS_HW_IRQ_BASE 32u
#define VIBEOS_HW_IRQ_TIMER 32u
/* ---- GDT + TSS ---------------------------------------------------------- */

#define VIBEOS_HW_TSS_SEL 0x28u         /* GDT index 5 */

/* null, kcode, kdata, ucode, udata, then one 16-byte (two-slot) TSS descriptor
 * per CPU: every core needs its own task state segment for RSP0. */
static uint64_t g_gdt[5u + 2u * VIBEOS_HW_MAX_CPUS];

#define VIBEOS_HW_TSS_SEL_FOR(cpu) ((uint16_t)((5u + 2u * (cpu)) * 8u))
/* ---- per-CPU state ------------------------------------------------------- */


/* Vectors 33..47, as delivered. See the dispatcher. */
static volatile uint64_t g_ioapic_irqs[15];

/* Readers of the console woken because an input device's interrupt said so.
 * Asserted non-zero by the gate: nobody types in CI, so until the keyboard's
 * probe raised IRQ1 on purpose this path - routing, dispatch, wake - ran in no
 * boot at all, and breaking it was invisible (C7). */
static volatile uint64_t g_input_irq_wakes;

/* An interrupt on a registry vector that no device owns. Must be zero: a
 * device raising one would be a driver routing its line to a vector it was not
 * given, and it would otherwise be acknowledged and forgotten. */
static volatile uint64_t g_device_stray_irqs;

uint64_t vibeos_x86_64_ioapic_irq_count(uint32_t vector) {
    return (vector >= 33u && vector < 48u) ? g_ioapic_irqs[vector - 33u] : 0ull;
}

hw_cpu_t g_cpus[VIBEOS_HW_MAX_CPUS];
uint32_t g_cpu_online_count = 1u;

/* Outstanding acknowledgements for a TLB shootdown, so the core that changed a
 * mapping can wait until every other core has stopped believing the old one. */
/* How many flush requests are outstanding on the whole machine, so a spin loop
 * can ask "is anybody waiting on me?" with one read and no per-CPU lookup. */
volatile uint32_t g_tlb_flush_live;
/* Counted, because a shootdown that never fires and one that is not needed
 * look identical from outside - and the first would leave the bug this exists
 * to fix exactly as it was, with the boot still green. */



/* Ring-0 stacks: one per CPU for the syscall/interrupt entry paths, plus a
 * separate boot stack each application processor starts on. */
static uint8_t g_kernel_syscall_stack[VIBEOS_HW_MAX_CPUS][16384] __attribute__((aligned(16)));
static uint8_t g_ap_boot_stack[VIBEOS_HW_MAX_CPUS][16384] __attribute__((aligned(16)));

/* Dedicated per-CPU stack for the timer interrupt, installed as IST slot 1.
 *
 * The timer is where task switching happens, and a switch must not run on the
 * outgoing task's kernel stack: the moment that task is marked runnable another
 * core can resume it and start writing to the very stack this core is still
 * popping the interrupt frame from. Landing the timer on a stack that belongs
 * to the CPU rather than to a task closes that window. Interrupt gates mask
 * interrupts, so this stack can never be re-entered on the same core. */
static uint8_t g_timer_ist_stack[VIBEOS_HW_MAX_CPUS][16384] __attribute__((aligned(16)));

hw_cpu_t *hw_this_cpu(void) {
    hw_cpu_t *p;
    __asm__ __volatile__("movq %%gs:16, %0" : "=r"(p));
    return p;
}

/* The same value, for the files lifted out of here; see arch_hw_internal.h. */
int hw_current_task(void);

/* Console-lock ownership (overrides the weak default in serial.c). Reads the
 * GS base MSR directly so it is safe to call before the per-CPU block is
 * installed, which happens after the first boot messages. */
/* Who am I, for a lock that has to tell cores apart.
 *
 * This used to read the per-CPU pointer out of KERNEL_GS_BASE and return 0
 * when that MSR was still zero - which it is on every core until its per-CPU
 * area is installed. So more than one core answered "I am cpu 0" at once, and
 * the console lock believed them: its recursion check is `depth > 0 && owner
 * == me`, so while cpu 0 genuinely held the lock, any core still reporting 0
 * took the already-mine branch and walked into the critical section without
 * it. Lines from two cores then interleaved mid-word, splitting the very
 * markers the boot gate matches on - and the gate reported a failure that had
 * not happened, which is the worst kind, because it sends you looking for a
 * bug in whatever the split line was about.
 *
 * The initial APIC id from CPUID leaf 1 is unique per core and valid from the
 * first instruction, with no MSR to set up first. CPUID serializes, which
 * costs something - but this is only reached around serial output, which is
 * orders of magnitude slower than the instruction. */
uint32_t vibeos_x86_64_cpu_id(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ __volatile__("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(1u), "c"(0u));
    (void)eax;
    (void)ecx;
    (void)edx;
    return ebx >> 24;
}

/* Strong versions of the console lock's interrupt hooks (weak no-ops in
 * serial.c so host tests still link). */
uint64_t vibeos_x86_64_irq_save(void) {
    uint64_t flags;
    __asm__ __volatile__("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

void vibeos_x86_64_irq_restore(uint64_t flags) {
    if (flags & 0x200ull) {
        __asm__ __volatile__("sti" ::: "memory");
    }
}

/* ---- SMP locking ---------------------------------------------------------
 *
 * Interrupts are masked for the whole critical section. Most takers already run
 * with IF clear (interrupt gates clear it, and SFMASK clears it on `syscall`),
 * but bring-up code calls into the task table from an ordinary kernel task with
 * interrupts on: if the timer preempted such a holder, the scheduler would spin
 * on a lock its own CPU owns and never make progress. */

/* hw_lock_t now lives in arch_hw_internal.h, so the files lifted out
 * of here can see it. */

hw_lock_t g_sched_lock;
hw_lock_t g_mm_lock;

/* The TCP/IP stack is entered both from syscalls and from the timer interrupt
 * that pumps the device, so it lives behind its own lock. Declared here because
 * the socket syscalls appear before the network bring-up code below. */
vibeos_inet_t g_net;
hw_lock_t g_net_lock;
int g_net_up;

/* Acquire a lock without masking interrupts.
 *
 * The ordinary hw_spin_lock disables interrupts for the whole critical
 * section, which is right for short sections and wrong for long ones: while it
 * is held, this core takes no timer tick, runs no scheduler and pumps no
 * network - and the other cores get nothing either, because the work they are
 * waiting on is here.
 *
 * execve is the long one. It loads a two-megabyte image, allocates and maps
 * six hundred pages, and tears down an address space. Doing that with the
 * timer off stops the whole machine for the duration, which under emulation is
 * long enough to be indistinguishable from a hang - and was reported as one,
 * with five minutes of complete silence.
 *
 * Safe only for a lock no interrupt handler ever takes: with interrupts left
 * enabled, a handler wanting this lock would spin forever against the code it
 * interrupted. g_exec_lock qualifies - execve is its only user. */
void hw_spin_lock_preemptible(hw_lock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
    lock->flags = 0;
}

void hw_spin_unlock_preemptible(hw_lock_t *lock) {
    __sync_lock_release(&lock->locked);
}

int hw_current_task(void) {
    return g_current_task;
}

/* Read by the spinlocks below, so it is defined above them: this is one
 * translation unit and a use before the definition compiles as an implicit
 * declaration and then fails confusingly. */
static volatile int g_sched_running;

/* How long a wait for a lock is allowed to be before it is called a deadlock.
 *
 * Not a timeout in any real sense: no critical section in this kernel is
 * anywhere near this long, and under TCG the slowest one - execve reading two
 * megabytes - is orders of magnitude below it. It is the line between "slow"
 * and "never", and it exists because the alternative to crossing it is
 * silence.
 *
 * That alternative has been paid for repeatedly. A machine that deadlocks stops
 * with no output, no panic and no trap dump, and what is left is a wedge report
 * naming whichever static function precedes the spin address - which in this
 * one-translation-unit kernel is frequently the wrong one. Both of the wedges
 * examined this week reported `hw_task_slots`, a function that returns a
 * constant.
 *
 * Crossing it panics, which parks every core and prints a backtrace. A machine
 * that says "cpu 2 waited on the lock cpu 0 took in hw_mmap_anon" is a machine
 * somebody can fix. */

/* Declared here and defined beside hw_panic, which it calls. This is one
 * translation unit and a helper used above its definition compiles as an
 * implicit declaration and then fails with a confusing "static declaration
 * follows non-static declaration" - a trap CLAUDE.md names outright. */
static void hw_lock_deadlock(hw_lock_t *lock, const char *waiter,
                             const char *holder, int holder_cpu);

/* Answer a TLB flush request addressed to this core, if there is one.
 *
 * A shootdown waits for each target's CR3 generation to move, and a core
 * spinning with interrupts off cannot take the IPI that would move it. Two such
 * cores waiting on each other is a machine that stops: that is what the first
 * load to force reclaim did (a swap-out waiting on a core that was waiting for
 * the swap-out's claim), and the family of timeouts CLAUDE.md records from the
 * broadcast days. So every spin that can wait with interrupts off asks here,
 * and reloads its own CR3 if somebody is waiting on it. Reloading the CR3 that
 * is already loaded is safe anywhere: the kernel is mapped the same way in
 * every address space. */
void hw_tlb_service_flush(void) {
    hw_cpu_t *me;

    if (__atomic_load_n(&g_tlb_flush_live, __ATOMIC_ACQUIRE) == 0u) {
        return;
    }
    me = hw_this_cpu();
    if (!me || __atomic_exchange_n(&me->flush_req, 0u, __ATOMIC_ACQ_REL) == 0u) {
        return;
    }
    __atomic_sub_fetch(&g_tlb_flush_live, 1u, __ATOMIC_ACQ_REL);
    hw_write_cr3(hw_read_cr3());
    __atomic_fetch_add(&vibeos_mm_stats()->tlb_acks, 1ull, __ATOMIC_RELAXED);
}

void hw_spin_lock_named(hw_lock_t *lock, const char *fn) {
    uint64_t flags;
    uint64_t spins = 0;

    __asm__ __volatile__("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked) {
            if (++spins > VIBEOS_HW_LOCK_SPIN_LIMIT) {
                /* Read before panicking: hw_panic parks the other cores, and
                 * the holder's identity is the whole point of the message. */
                const char *held_by = lock->owner_fn;
                int held_cpu = lock->owner_cpu;

                hw_lock_deadlock(lock, fn, held_by, held_cpu);
                spins = 0;   /* reached only if the panic ever returns */
            }
            /* The holder may be waiting for this core to flush. */
            hw_tlb_service_flush();
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
    lock->flags = flags;
    lock->owner_cpu = (int)vibeos_x86_64_cpu_id();
    lock->owner_fn = fn;
}

void hw_spin_lock(hw_lock_t *lock) {
    hw_spin_lock_named(lock, "unnamed");
}

void hw_spin_unlock(hw_lock_t *lock) {
    uint64_t flags = lock->flags;
    /* Cleared before the release, not after: a core that takes the lock the
     * instant it is freed would otherwise have its owner overwritten by the
     * previous holder's tidying, and the deadlock report would name the wrong
     * one. Same shape as the exit ordering this kernel already carries a long
     * comment about. */
    lock->owner_fn = 0;
    lock->owner_cpu = -1;
    __sync_lock_release(&lock->locked);
    if (flags & 0x200ull) {   /* only re-enable if the caller had them on */
        __asm__ __volatile__("sti" ::: "memory");
    }
}

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static void hw_setup_tss_descriptor(uint32_t index, uint64_t base, uint32_t limit) {
    g_gdt[index] = (uint64_t)(limit & 0xFFFFu)
                 | ((base & 0xFFFFull) << 16)
                 | (((base >> 16) & 0xFFull) << 32)
                 | (0x89ull << 40)                      /* present, 64-bit TSS (available) */
                 | (((uint64_t)(limit >> 16) & 0xFull) << 48)
                 | (((base >> 24) & 0xFFull) << 56);
    g_gdt[index + 1] = (base >> 32) & 0xFFFFFFFFull;
}

/* ---- IDT ---------------------------------------------------------------- */

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry g_idt[VIBEOS_HW_IDT_GATES];

/* Populated by isr.S: one assembly stub entry point per wired vector. */
extern uint64_t vibeos_isr_stub_table[VIBEOS_HW_WIRED_VECTORS];

/* Ring-3 support implemented in isr.S. */
extern void vibeos_x86_64_ring3_enter(uint64_t user_rip, uint64_t user_rsp);
extern void vibeos_x86_64_ring3_resume(void);

/* The embedded user program (generated blob).
 *
 * There was a second ELF loader here - kernel/arch/x86_64/elf_load.c, declared
 * just below and called by nothing since it was written. It is deleted rather
 * than repaired: a review found an integer overflow in its program-header
 * check, and repairing dead code buys a fix for a path nobody can reach while
 * leaving the reason it was dangerous - that it exists at all - in place.
 *
 * The loader this kernel actually uses is kernel/core/elf.c, which is
 * host-tested and is what exec goes through. Two loaders is two places that
 * have to be right about ELF, and this project has spent whole phases removing
 * second places.
 */
extern const unsigned char vibeos_user_hello_elf[];
extern const unsigned long vibeos_user_hello_elf_len;

/* vibeos_x86_64_isr_frame_t now lives in arch_hw_internal.h. */

/* The two disk interrupt handlers, declared where the dispatcher can see
 * them. They were being called implicitly: gcc accepts that with a warning,
 * clang refuses it, and code scanning reported both call sites. */

static void hw_schedule(vibeos_x86_64_isr_frame_t *frame); /* defined below */

int hw_user_range_ok(uint64_t va, uint64_t len, int need_write);
/* The fault-tolerant copy's faulting range and recovery point (uaccess.S). */
extern const char vibeos_uaccess_copy_begin[];
extern const char vibeos_uaccess_copy_end[];
extern void vibeos_uaccess_copy_fixup(void);
static uint64_t g_uaccess_recovered;
static void hw_net_pump(void);                             /* defined below */

/* Load the shared GDT on this CPU, install its private TSS, and point GS.base
 * at its per-CPU block. Every core runs this; the shared descriptors are
 * rewritten identically, which is harmless. */
static void hw_load_gdt(uint32_t cpu_index) {
    struct gdt_pointer gdtr;
    hw_cpu_t *cpu = &g_cpus[cpu_index];
    uint64_t kstack_top =
        (uint64_t)(uintptr_t)&g_kernel_syscall_stack[cpu_index][sizeof(g_kernel_syscall_stack[0])];
    uint32_t i;

    g_gdt[0] = 0x0000000000000000ull;        /* null                        */
    g_gdt[1] = 0x00AF9A000000FFFFull;        /* kernel code (0x08, DPL=0)   */
    g_gdt[2] = 0x00CF92000000FFFFull;        /* kernel data (0x10, DPL=0)   */
    g_gdt[3] = 0x00CFF2000000FFFFull;        /* user data   (0x18, DPL=3)   */
    g_gdt[4] = 0x00AFFA000000FFFFull;        /* user code   (0x20, DPL=3)   */

    for (i = 0; i < (uint32_t)sizeof(cpu->tss); i++) {
        ((uint8_t *)(void *)&cpu->tss)[i] = 0;
    }
    cpu->tss.rsp0 = kstack_top;
    /* IST slot 1: the timer interrupt's own stack on this CPU (see above). */
    cpu->tss.ist[0] =
        (uint64_t)(uintptr_t)&g_timer_ist_stack[cpu_index][sizeof(g_timer_ist_stack[0])];
    cpu->tss.iomap_base = (uint16_t)sizeof(cpu->tss);
    cpu->syscall_kstack_top = kstack_top;
    cpu->self = cpu;
    cpu->index = cpu_index;
    cpu->current_task = -1;
    cpu->idle_task = -1;
    hw_setup_tss_descriptor(5u + 2u * cpu_index, (uint64_t)(uintptr_t)&cpu->tss,
                            (uint32_t)(sizeof(cpu->tss) - 1u));

    gdtr.limit = (uint16_t)(sizeof(g_gdt) - 1u);
    gdtr.base = (uint64_t)(uintptr_t)&g_gdt[0];

    __asm__ __volatile__(
        "lgdt %0\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "pushq $0x08\n\t"           /* new CS */
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n\t"                 /* far return reloads CS */
        "1:\n\t"
        :
        : "m"(gdtr)
        : "rax", "memory");

    __asm__ __volatile__("ltr %0" : : "r"(VIBEOS_HW_TSS_SEL_FOR(cpu_index)));

    /* GS.base -> this CPU's block. Must come after the %gs selector load above,
     * which resets the base to zero. IA32_KERNEL_GS_BASE gets the same value so
     * a stray swapgs cannot desynchronize the two. */
    {
        uint64_t v = (uint64_t)(uintptr_t)cpu;
        __asm__ __volatile__("wrmsr" : : "c"(0xC0000101u), "a"((uint32_t)v),
                             "d"((uint32_t)(v >> 32)));
        __asm__ __volatile__("wrmsr" : : "c"(0xC0000102u), "a"((uint32_t)v),
                             "d"((uint32_t)(v >> 32)));
    }
}

static void hw_set_gate_attr(uint32_t vector, uint64_t handler, uint8_t type_attr) {
    struct idt_entry *e = &g_idt[vector];
    e->offset_low = (uint16_t)(handler & 0xFFFFull);
    e->selector = (uint16_t)VIBEOS_HW_KERNEL_CS;
    e->ist = 0;
    e->type_attr = type_attr;
    e->offset_mid = (uint16_t)((handler >> 16) & 0xFFFFull);
    e->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFull);
    e->reserved = 0;
}

static void hw_set_gate(uint32_t vector, uint64_t handler) {
    hw_set_gate_attr(vector, handler, (uint8_t)VIBEOS_HW_GATE_INTERRUPT);
}

extern char vibeos_isr_128[];   /* isr.S: stub for the 0x80 syscall gate      */
extern char vibeos_isr_255[];   /* isr.S: stub for the LAPIC spurious vector  */
extern char vibeos_isr_254[];   /* isr.S: stub for the TLB shootdown IPI      */

/* APIC / SMP (apic.c + ap_boot.S). */
extern int vibeos_x86_64_acpi_init(uint64_t rsdp_addr);
extern uint32_t vibeos_x86_64_acpi_cpu_count(void);
extern uint32_t vibeos_x86_64_acpi_lapic_id(uint32_t index);
extern void vibeos_x86_64_lapic_enable(uint32_t spurious_vector);
extern void vibeos_x86_64_lapic_eoi(void);
extern void vibeos_x86_64_lapic_timer_start(uint32_t hz, uint32_t vector);
extern uint32_t vibeos_x86_64_lapic_id(void);
extern int vibeos_x86_64_ioapic_route(uint8_t irq, uint8_t vector, uint32_t dest);
extern int vibeos_x86_64_smp_start_cpu(uint32_t lapic_id, uint64_t cr3, uint64_t stack_top,
                                       uint64_t entry);
extern void vibeos_x86_64_pic_disable(void);
extern int vibeos_x86_64_apic_available(void);
extern volatile uint32_t vibeos_x86_64_ap_alive;

/* The network interface is whichever device registered one (C7,
 * include/vibeos/device.h); see hw_net_bringup. */

#include "vibeos/log.h"
#include "vibeos/klog.h"
#include "vibeos/crash.h"
#include "vibeos/device.h"

_Static_assert(VIBEOS_DEVICE_VECTOR_BASE + VIBEOS_DEVICE_VECTORS <= VIBEOS_HW_WIRED_VECTORS,
               "every vector the registry hands out needs a stub in isr.S");

#include "vibeos/mm_model.h"
#include "vibeos/frame.h"
#include "vibeos/vmspace.h"
#include "vibeos/vma.h"
#include "vibeos/backing.h"
#include "vibeos/task_stats.h"
#include "vibeos/task.h"
#include "vibeos/runq.h"
#include "vibeos/lifetime.h"


#include "vibeos/vfs.h"

/* The one mounted volume. Everything below reaches the filesystem through
 * this, so no syscall in this file knows which driver is underneath it. */
vibeos_fsmount_t g_rootfs;
/* Whether any disk carried a mountable boot volume. A flag rather than a longer
 * condition at the use site, because the loop that sets it already has to break
 * out of itself. */
static int g_boot_disk_mounted;

/* The desktop's back buffer, kept so the end-of-boot report can ask who else
 * owns its frames. The canary that used to sit here, written and checked by
 * this file, belongs to the GUI since C7 (kernel/io/gui.c): it writes it
 * directly after the pixels it composes into and checks it on every repaint. */
static void *g_gui_back;
static uint64_t g_gui_back_bytes;
static uint64_t g_gui_back_base;
static uint64_t g_gui_back_end;
static uint64_t g_gui_back_shared;
static uint64_t g_gui_back_lost;
/* Ring-3 text writes whose leading bytes read as NUL. See hw_sys_write. */
uint64_t g_ring3_write_nul;


/* The ABI surface's must-be-zero: a syscall number the kernel does not
 * implement. musl probes some and tolerates ENOSYS, but nothing the boot runs
 * should reach one, and a program that does gets -ENOSYS and carries on
 * believing something worked. The boot asks for VIBEOS_ABI_PROBE_NR on purpose
 * (user/prog/hello.c) so the count is seen moving; the gate asserts
 * unimplemented == probes, and last_nr names the number when it is not. */
volatile uint64_t g_abi_unimplemented;
volatile uint64_t g_abi_probes;
volatile uint64_t g_abi_last_nr;
extern int vibeos_x86_64_fb_init(uint64_t base, uint32_t width, uint32_t height);
extern int vibeos_x86_64_fb_ready(void);
extern void vibeos_x86_64_fb_puts(const char *s);

/* Init program read from the on-disk filesystem, if present. */
static uint8_t g_disk_init_elf[65536] __attribute__((aligned(16)));
static long g_disk_init_len = -1;

static void hw_load_idt(void) {
    struct idt_pointer idtr;
    uint32_t i;
    for (i = 0; i < VIBEOS_HW_IDT_GATES; i++) {
        g_idt[i].offset_low = 0;
        g_idt[i].selector = 0;
        g_idt[i].ist = 0;
        g_idt[i].type_attr = 0;
        g_idt[i].offset_mid = 0;
        g_idt[i].offset_high = 0;
        g_idt[i].reserved = 0;
    }
    for (i = 0; i < VIBEOS_HW_WIRED_VECTORS; i++) {
        hw_set_gate(i, vibeos_isr_stub_table[i]);
    }
    /* Syscall gate: DPL=3 so ring-3 `int 0x80` is permitted (0xEE). */
    hw_set_gate_attr(0x80u, (uint64_t)(uintptr_t)vibeos_isr_128, 0xEEu);
    /* Local-APIC spurious interrupt: must be handled, and must not be EOI'd. */
    hw_set_gate(0xFFu, (uint64_t)(uintptr_t)vibeos_isr_255);
    /* TLB shootdown. See hw_tlb_shootdown for why a kernel with more than one
     * core cannot do without it. */
    hw_set_gate(0xFEu, (uint64_t)(uintptr_t)vibeos_isr_254);
    /* The timer runs the scheduler, so it takes IST slot 1 - a stack owned by
     * the CPU rather than by whichever task happened to be interrupted. */
    g_idt[VIBEOS_HW_IRQ_TIMER].ist = 1;
    idtr.limit = (uint16_t)(sizeof(g_idt) - 1u);
    idtr.base = (uint64_t)(uintptr_t)&g_idt[0];
    __asm__ __volatile__("lidt %0" : : "m"(idtr) : "memory");
}

/* Application processors share the BSP's IDT; they only need to point at it. */
static void hw_load_idt_only(void) {
    struct idt_pointer idtr;
    idtr.limit = (uint16_t)(sizeof(g_idt) - 1u);
    idtr.base = (uint64_t)(uintptr_t)&g_idt[0];
    __asm__ __volatile__("lidt %0" : : "m"(idtr) : "memory");
}

static uint64_t hw_read_cr2(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void hw_outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void hw_io_wait(void) {
    /* Write to an unused port to give the PIC time to settle between commands. */
    __asm__ __volatile__("outb %%al, $0x80" : : "a"((uint8_t)0));
}

/* Timer tick counter, incremented from the IRQ0 handler. */
volatile uint64_t g_timer_ticks;

/* Remap the 8259 PIC so IRQ 0-15 arrive as vectors 0x20-0x2F, then mask
 * everything except the timer (IRQ0). */
static void hw_pic_remap(void) {
    hw_outb(PIC1_CMD, 0x11); hw_io_wait();   /* ICW1: init + expect ICW4 */
    hw_outb(PIC2_CMD, 0x11); hw_io_wait();
    hw_outb(PIC1_DATA, 0x20); hw_io_wait();  /* ICW2: master vector offset */
    hw_outb(PIC2_DATA, 0x28); hw_io_wait();  /* ICW2: slave vector offset  */
    hw_outb(PIC1_DATA, 0x04); hw_io_wait();  /* ICW3: slave on IRQ2         */
    hw_outb(PIC2_DATA, 0x02); hw_io_wait();  /* ICW3: slave cascade id      */
    hw_outb(PIC1_DATA, 0x01); hw_io_wait();  /* ICW4: 8086 mode             */
    hw_outb(PIC2_DATA, 0x01); hw_io_wait();
    hw_outb(PIC1_DATA, 0xF8);   /* IRQ0 timer, IRQ1 keyboard, IRQ2 cascade */
    hw_outb(PIC2_DATA, 0xEF);   /* IRQ12 mouse, on the slave controller */
    hw_outb(PIC2_DATA, 0xFF);                /* mask all slave IRQs          */
}

/* Program PIT channel 0 to a periodic square wave at VIBEOS_HW_TIMER_HZ. */
static void hw_pit_init(void) {
    uint32_t divisor = PIT_BASE_HZ / VIBEOS_HW_TIMER_HZ;
    hw_outb(PIT_CMD, 0x36);                       /* ch0, lo/hi byte, mode 3 */
    hw_outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    hw_outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
}

/* Set once the APIC pair has taken over from the 8259s. */
int g_apic_mode;

static void hw_pic_send_eoi(uint32_t vector) {
    if (g_apic_mode) {
        vibeos_x86_64_lapic_eoi();
        return;
    }
    if (vector >= 40u) {          /* IRQ came via the slave PIC */
        hw_outb(PIC2_CMD, PIC_EOI);
    }
    hw_outb(PIC1_CMD, PIC_EOI);
}
/* Defined with the rest of the signal code, far below; the timer path needs it
 * here so a signal raised while a task was running is delivered on the way
 * back to ring 3 rather than at the next syscall. */
int hw_signal_deliver(vibeos_x86_64_isr_frame_t *frame);
/* Defined with the task code, because it needs the signal numbers that are
 * #defined a thousand lines below here and C only reads the file once. */
static void hw_panic_cpu_summary(void);   /* defined with the task table */
/* Defined with the task table: answers whether a frame about to be freed
 * is still mapped by a live process. */
/* How many owned mappings the last walk found. One is a lost reference; more
 * than one says how many were lost, which is the difference between "somebody
 * forgot a get" and "a whole fork's worth went missing". */
/* The mapper count was a global, written by whichever core happened to be
 * inside this walk. Two cores checking a free at the same moment overwrote each
 * other's count, so the report printed numbers that were not the ones it
 * decided on - a line reading "mappers=0 owners=0" from a check that only fires
 * when mappers exceeds owners. Rare while the check sampled one free in
 * sixteen, and immediate once it looked at every one, which is how it was
 * found. It is a local now, returned to the caller. */

static void hw_log_field(const char *name, uint64_t value) {
    vibeos_x86_64_serial_puts(name);
    vibeos_x86_64_serial_puts("=0x");
    vibeos_x86_64_serial_print_hex(value);
}

/* Bring-up-local trap sink. On-metal CPU exceptions are routed through the
 * portable decision model (vibeos_trap_dispatch_ex) so the same classify /
 * action logic that host tests cover also governs real faults. A later
 * milestone will point this at the live kernel trap_state and current PID
 * once user processes exist. */
static vibeos_trap_state_t g_arch_trap_state;
static int g_arch_trap_ready;

static const char *hw_action_name(vibeos_trap_action_t action) {
    switch (action) {
        case VIBEOS_TRAP_ACTION_CONTINUE: return "CONTINUE";
        case VIBEOS_TRAP_ACTION_KILL_CURRENT: return "KILL_CURRENT";
        case VIBEOS_TRAP_ACTION_PANIC: return "PANIC";
        default: return "UNKNOWN";
    }
}

/* ---- the kernel log's device --------------------------------------------
 *
 * The log itself - the ring, its lock, the formatting, the sinks and the dumps -
 * is kernel/diag/klog.c (C6). What stays here is what is different on another
 * machine: the UART it writes to, and the spin lock it is serialised with.
 *
 * Events are always recorded into the ring, which costs a memcpy, and only
 * those at or above INFO are also written to the serial line: the line is slow
 * enough under emulation to change the timing of the bug being hunted. When the
 * machine dies the ring is dumped, including everything that was too quiet to
 * print at the time, which is usually the part worth reading. */

static hw_lock_t g_klog_lock;

static void hw_klog_lock(void) {
    hw_spin_lock_named(&g_klog_lock, "vibeos_klog");
}

static void hw_klog_unlock(void) {
    hw_spin_unlock(&g_klog_lock);
}

/* One complete line, in one call to serial_puts - which is one critical section
 * on the console lock. There is nothing to bracket, so nothing to forget to
 * bracket: that is the defect this sink exists not to be able to have. Its
 * predecessor, hw_log_emit, built each line from eight writes and ended with an
 * unlock it had never taken - see "An unmatched unlock" in CLAUDE.md.
 *
 * It never logs from inside itself, so it is registered with no reentrancy
 * guard (below) and its `reentered` count is zero by construction; the gate
 * asserts it anyway, so turning the guard back on for it would be seen. */
static int hw_serial_sink_write(void *ctx, const char *line, uint32_t len) {
    (void)ctx;
    (void)len;
    vibeos_x86_64_serial_puts(line);
    return 0;
}

/* No reentrancy guard: serial_puts never logs, so this sink cannot be re-entered,
 * and a sink with no guard is never skipped. The guard used to be on for every
 * sink and keyed by core, which could skip serial lines for a reason that did
 * not apply to it. Designated, because a positional initialiser silently shifts
 * the day the struct gains a field - it just did. */
static const vibeos_klog_sink_t g_serial_sink = {
    .name = "serial",
    .min_level = VIBEOS_LOG_INFO,
    .prefix = "[LOG]",
    .numbered = 1,
    .newline = 1,
    .guard_reentry = 0,
    .write = hw_serial_sink_write,
    .ctx = 0,
};

/* Who is logging, for the module's reentrancy guard: the running task, which
 * stays the same when the scheduler moves it to another core and which an
 * interrupt nested in it shares (a write nested in a write is recursion). Before
 * this core's per-CPU block exists - GS.base is still zero - or before it runs a
 * task, one number per core past the task slots.
 *
 * GS.base is read from the MSR rather than dereferenced, because an application
 * processor logs before its block is installed, and following a zero GS there
 * is how a previous diagnostic wrote through garbage into low memory. */
_Static_assert(VIBEOS_HW_MAX_TASKS + VIBEOS_HW_MAX_CPUS <= VIBEOS_KLOG_MAX_CONTEXTS,
               "the kernel log's reentrancy guard must tell every context apart");

static uint32_t hw_klog_context(void) {
    uint32_t lo, hi;
    uint32_t cpu = vibeos_x86_64_cpu_id() % VIBEOS_HW_MAX_CPUS;

    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101u));
    if (lo != 0u || hi != 0u) {
        int t = hw_this_cpu()->current_task;
        if (t >= 0 && t < VIBEOS_HW_MAX_TASKS) {
            return (uint32_t)t;
        }
    }
    return (uint32_t)VIBEOS_HW_MAX_TASKS + cpu;
}

static void hw_klog_init(void) {
    vibeos_klog_set_lock(hw_klog_lock, hw_klog_unlock);
    vibeos_klog_set_context(hw_klog_context);
    vibeos_klog_reset();
    if (vibeos_klog_add_sink(&g_serial_sink) < 0) {
        hw_panic("kernel log: the serial sink did not register");
    }
}

void hw_log(vibeos_log_level_t level, uint32_t code, uint64_t a0,
                   uint64_t a1, const char *message) {
    vibeos_klog(level, code, a0, a1, message);
}

/* The ring, on demand, newest `want` entries, for the console's `log` command.
 * Bracketed as a whole so the listing arrives unbroken; the module takes its own
 * lock inside, one event at a time, which is the lock order it documents. */
void vibeos_x86_64_log_dump_recent(uint32_t want) {
    vibeos_x86_64_serial_lock();
    vibeos_klog_dump_recent(want, hw_serial_sink_write, 0);
    vibeos_x86_64_serial_unlock();
}

/* Walk the saved frame pointers and print the return addresses.
 *
 * A panic used to say what went wrong and nothing about how the machine got
 * there, which for a fault inside one of two hundred static helpers is most of
 * the question. The addresses are raw on purpose: almost everything in this
 * file is static, so naming them from the nearest preceding symbol is
 * frequently wrong - and a confidently wrong name costs more than a number.
 * scripts/dev/symbolize.py turns them into file and line with addr2line, which
 * has the debug info and does not guess.
 *
 * Every read is checked before it happens. This runs inside a fault handler,
 * so following a corrupt chain would fault again and the second fault would
 * replace the first one's evidence with a reset. */
#define HW_BT_MAX_FRAMES 16u
#define HW_BT_CODE_LO 0x4000000ull
#define HW_BT_CODE_HI 0x5000000ull

static int hw_bt_plausible_code(uint64_t addr) {
    return addr >= HW_BT_CODE_LO && addr < HW_BT_CODE_HI;
}

static int hw_bt_plausible_frame(uint64_t rbp, uint64_t prev) {
    if (rbp == 0 || (rbp & 7u) != 0) {
        return 0;   /* a frame pointer is always 8-aligned */
    }
    if (rbp < 0x1000ull || rbp >= 0x8000000000ull) {
        return 0;   /* not kernel memory */
    }
    /* Stacks grow down, so each caller's frame sits above the callee's. A
     * chain that goes backwards is a loop, and a loop here never ends. */
    return prev == 0 || rbp > prev;
}

static void hw_backtrace(uint64_t rbp, uint64_t rip) {
    uint64_t prev = 0;
    uint32_t depth;

    /* The whole trace under one lock. It printed unlocked, so another core's
     * output landed between two frames of it - and a backtrace with a foreign
     * line in the middle is not a backtrace, it is a puzzle. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[BT] rip=0x");
    vibeos_x86_64_serial_print_hex(rip);
    vibeos_x86_64_serial_puts("\n");

    for (depth = 0; depth < HW_BT_MAX_FRAMES; depth++) {
        const uint64_t *frame;
        uint64_t ret;

        if (!hw_bt_plausible_frame(rbp, prev)) {
            break;
        }
        frame = (const uint64_t *)(uintptr_t)rbp;
        ret = frame[1];             /* [rbp+8] is the return address */
        if (!hw_bt_plausible_code(ret)) {
            break;
        }
        vibeos_x86_64_serial_puts("[BT]   #");
        vibeos_x86_64_serial_print_hex(depth);
        vibeos_x86_64_serial_puts(" 0x");
        vibeos_x86_64_serial_print_hex(ret);
        vibeos_x86_64_serial_puts("\n");
        prev = rbp;
        rbp = frame[0];             /* [rbp] is the caller's frame pointer */
    }
    vibeos_x86_64_serial_puts("[BT] end depth=0x");
    vibeos_x86_64_serial_print_hex(depth);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* Set by hw_panic before it halts. Every core checks it on the way out of an
 * interrupt and parks itself.
 *
 * A panic used to stop one core. The message even said so - "halting on cpu 2" -
 * and it was read for weeks as though it said "halting". The other three cores
 * carried on, the log kept moving, and the machine went quiet some seconds
 * later at a place with no connection to the fault. That is the whole silent
 * wedge family: the evidence and the failure were separated by however long it
 * took the survivors to need the core that had died.
 *
 * Not an IPI, deliberately. Every core already takes a timer interrupt a
 * hundred times a second, so this needs no new vector, no acknowledgement
 * protocol and no timeout to get wrong. A core sitting in a syscall with
 * interrupts masked answers when it returns to ring 3; a core that never
 * returns was stuck with or without this. */
static volatile int g_panicked;


/* Decision T7: no lock may be held across a scheduling point.
 *
 * The classic priority inversion - a low-priority task holding a lock a
 * high-priority task wants, while something in between runs - cannot happen
 * here in its usual form, because these spinlocks mask interrupts: a core
 * inside one never takes the timer, so it is never preempted while holding.
 *
 * What *can* happen is worse. A path that takes a lock and then blocks or
 * exits leaves the lock held by a task that is no longer running, and every
 * core that wants it spins with interrupts off, forever. That is not a task
 * being delayed; it is the machine stopping. The virtio-net wedge found this
 * evening was exactly that shape, one layer down.
 *
 * So the answer is not inheritance or a priority ceiling - both of which would
 * make the situation survivable - but making it impossible to enter, and
 * saying so when somebody tries.
 *
 * Counted rather than fatal, for the reason the transition table gives: a count
 * can be asserted across a whole boot, and a panic can only be met once, on
 * somebody else's investigation. */
void hw_sched_point(const char *where) {
    (void)where;
    /* Not implemented yet, and the first attempt is why.
     *
     * The check needs a per-core count of held locks, so the first version
     * incremented one in hw_spin_lock through hw_this_cpu(). That reads the
     * per-CPU pointer out of GS - and an application processor takes locks
     * before it installs its own area, so the increment wrote through a garbage
     * pointer into low memory. The boot died with a #GP at rip=0x39: the kernel
     * jumping to an address that is not code.
     *
     * A diagnostic that corrupts is worse than no diagnostic, which this
     * project has already learned three times over, so it came straight back
     * out rather than being guarded into working.
     *
     * What the next attempt needs is a core identity that is valid before the
     * per-CPU area exists. `vibeos_x86_64_cpu_id()` is exactly that - it reads
     * the initial APIC id from CPUID leaf 1, which is valid from the first
     * instruction - but CPUID is serialising and this would be on every lock,
     * so the honest options are a cheap per-core index established earlier in
     * bring-up, or counting only in the handful of layers that can block.
     *
     * The decision itself stands and is not in question: no lock may be held
     * across a scheduling point. Nothing in the kernel does it today. What is
     * missing is the check that would notice if something started. */
}

void hw_panic(const char *why) {
    /* The medium first, before anything else this function does.
     *
     * A panic is the one moment the on-disk log exists for, and it is also the
     * moment when the least is working: another core may hold any lock and the
     * scheduler is about to be parked. vibeos_klog_panic takes no lock - see
     * kernel/diag/klog.c - so this line can be written from here. It goes first
     * because every line after it is one more chance to stop before reaching
     * the disk. */
    vibeos_klog_panic(why);

    uint64_t rbp;
    uint64_t rip;

    __asm__ __volatile__("movq %%rbp, %0" : "=r"(rbp));
    /* The panic site itself, so the first line of the trace is this call and
     * not whatever the compiler left in a register. */
    __asm__ __volatile__("leaq (%%rip), %0" : "=r"(rip));

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts(" FATAL: ");
    vibeos_x86_64_serial_puts(why);
    vibeos_x86_64_serial_puts(", halting on cpu 0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_x86_64_cpu_id());
    vibeos_x86_64_serial_puts("\n");

    hw_panic_cpu_summary();
    vibeos_x86_64_serial_unlock();

    hw_backtrace(rbp, rip);
    vibeos_klog_dump_unlocked(hw_serial_sink_write, 0);

    /* Announced after the dump, so the evidence is out before anything else
     * stops, and on one line because three other cores may still be writing. */
    g_panicked = 1;
    __asm__ __volatile__("sfence" ::: "memory");
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[PANIC] MACHINE_STOPPED every core parks on its "
                              "next interrupt\n");
    vibeos_x86_64_serial_unlock();

    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}
/* A lock nobody released, reported instead of waited on forever.
 *
 * Everything is read from the lock before the panic, because hw_panic parks
 * every other core and the holder's identity is the whole message.
 *
 * The names are function names captured at acquisition rather than addresses.
 * Almost everything in this file is static, so an address resolves to whichever
 * symbol happens to precede it, and a confidently wrong name is worse than a
 * number: both of the wedges examined this week reported `hw_task_slots`, a
 * function that returns a constant.
 *
 * ## Written without taking the console lock, deliberately
 *
 * The first version of this used serial_puts, which locks. That deadlocked the
 * deadlock reporter: under svc-press the console lock is one of the jammed
 * ones, so the core that had detected the problem sat in
 * vibeos_x86_64_serial_lock and the message never appeared. The wedge report
 * caught it in the act - CPU#3 stopped inside this function.
 *
 * So it writes raw bytes. The output can interleave with another core's, and
 * that is the correct trade for the same reason the console lock's own bound
 * takes the lock rather than waiting: interleaving is detected by the boot
 * gate's first check, and silence is the failure where every piece of evidence
 * is missing rather than wrong. A lock reporter that can only speak when the
 * locks are healthy reports nothing on exactly the boots it exists for. */
static void hw_raw_puts(const char *s) {
    if (!s) {
        return;
    }
    while (*s) {
        if (*s == '\n') {
            vibeos_x86_64_serial_putc('\r');
        }
        vibeos_x86_64_serial_putc(*s++);
    }
}

static void hw_raw_hex(uint64_t v) {
    const char *d = "0123456789abcdef";
    int i;
    for (i = 60; i >= 0; i -= 4) {
        vibeos_x86_64_serial_putc(d[(v >> i) & 0xFull]);
    }
}

static void hw_lock_deadlock(hw_lock_t *lock, const char *waiter,
                             const char *holder, int holder_cpu) {
    hw_raw_puts("\n[LOCK] DEADLOCK lock=0x");
    hw_raw_hex((uint64_t)(uintptr_t)lock);
    hw_raw_puts(" waiter_cpu=0x");
    hw_raw_hex((uint64_t)vibeos_x86_64_cpu_id());
    hw_raw_puts(" waiting_in=");
    hw_raw_puts(waiter ? waiter : "?");
    hw_raw_puts(" held_by_cpu=0x");
    hw_raw_hex((uint64_t)(holder_cpu < 0 ? 255 : holder_cpu));
    hw_raw_puts(" taken_in=");
    hw_raw_puts(holder ? holder : "(released, or never named)");
    hw_raw_puts("\n");
    hw_panic("spinlock held too long: deadlock");
}


/* ---- the three hot paths, counted and timed (core plan C0) -----------------
 *
 * This kernel had no performance measurement of any kind. Not a benchmark, not
 * a timing, not a budget - so every statement anybody could make about its
 * speed, including a reassuring one, was invented. The core refactor adds
 * indirect calls to paths that run thousands of times a boot, and without a
 * baseline it could not be shown *not* to have cost anything.
 *
 * What the first two measured boots actually showed, which is not what this
 * comment said before they were taken:
 *
 *   syscalls 1416 / 1381   faults 396 / 378   switches 516 / 497
 *
 * The counts were going to be described here as deterministic, and ratcheted on
 * that basis. They are not - they move three to four per cent boot to boot,
 * because what the machine does depends on how the services interleave. So
 * neither the counts nor the cycles can be ratcheted from a single sample, and
 * the gate asserts only that they are non-zero until the spread is known. That
 * is the same mistake the memory manager's P2 made, caught this time by taking
 * a second measurement before writing the criterion rather than after.
 *
 * The **cycles** are noisier still, and the mean is not a baseline at all: a
 * syscall that blocks is timed across the block, so the first boot read 46
 * million cycles per syscall and was reporting how long waitpid waited. The
 * minimum is the usable one - 728 cycles for a syscall, against a mean sixty
 * thousand times larger. A blocked task cannot lower a minimum, and an added
 * lookup or lock on the path raises it.
 *
 * The fault minimum came out at 808,000 cycles, which is large enough to be
 * worth a note rather than an explanation: the copy-on-write fault sends a TLB
 * shootdown IPI and waits for the acknowledgements, so most of that is likely
 * the round trip. Likely, not measured - under TCG the emulated cost of an IPI
 * bears no fixed relation to a real one, and this is exactly the kind of number
 * this project has a rule against reasoning about without a second source.
 *
 * Cost of the instrumentation itself: two rdtsc and one atomic add per event.
 * Under TCG that is far below the cost of the event being measured. On metal it
 * would be worth revisiting, and this comment is the note saying so. */
typedef struct {
    uint64_t count;
    uint64_t cycles;
    /* The fastest traversal seen, and the only one of these three that is a
     * baseline.
     *
     * The mean is not: a syscall that blocks is timed across the block, so on
     * the first measured boot it read 46 million cycles per syscall and was
     * reporting how long waitpid waited rather than what dispatch costs. That
     * number would have moved with anything that changed scheduling, which is
     * the opposite of what a performance ratchet is for.
     *
     * The minimum cannot be inflated by a blocked task, and an added lookup or
     * lock on the path moves it. Both are kept: the mean is still worth reading
     * next to the minimum, because the two diverging is itself information. */
    uint64_t min;
} hw_perf_t;

static hw_perf_t g_perf_syscall;
static hw_perf_t g_perf_fault;
static hw_perf_t g_perf_switch;

static inline uint64_t hw_tsc(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Atomic because all four cores take syscalls and faults. A lost increment here
 * would be the same defect the copy-on-write refcounts had, with a much smaller
 * blast radius - but a counter that is wrong is worse than no counter, because
 * this project's whole method is believing them. */
static inline void hw_perf_add(hw_perf_t *p, uint64_t started) {
    uint64_t took = hw_tsc() - started;
    uint64_t seen;

    __sync_fetch_and_add(&p->count, 1ull);
    __sync_fetch_and_add(&p->cycles, took);
    /* Compare-exchange rather than a read and a store: four cores are in here,
     * and a lost minimum is a number that silently reports somebody else's
     * sample. The loop retries only when another core lowered it in between,
     * which by construction cannot spin for long - each iteration means the
     * value strictly decreased. */
    for (;;) {
        seen = p->min;
        if (seen != 0ull && took >= seen) {
            break;
        }
        if (__sync_bool_compare_and_swap(&p->min, seen, took)) {
            break;
        }
    }
}
void vibeos_x86_64_syscall_dispatch(vibeos_x86_64_isr_frame_t *frame);

/* Called from the assembly common stub with a pointer to the saved frame. */
void vibeos_x86_64_isr_handler(vibeos_x86_64_isr_frame_t *frame) {
    vibeos_trap_frame_t tf;
    vibeos_trap_decision_t decision;
    uint64_t fault_address;

    /* Software syscall gate (int 0x80), Linux argument order. */
    if (frame->vector == 0x80u) {
        frame->rax = (uint64_t)vibeos_x86_64_linux_syscall(frame, frame->rax, frame->rdi, frame->rsi, frame->rdx);
        return;
    }

    /* Hardware IRQs (post-remap vectors 0x20-0x2F): acknowledge to the PIC. The
     * timer (IRQ0) additionally drives the preemptive scheduler. */
    /* Local-APIC spurious interrupt: by architecture it must NOT be EOI'd. */
    if (frame->vector == 0xFFu) {
        return;
    }

    /* TLB shootdown: another core changed a mapping in an address space this
     * one may be running, and a stale entry here would let it keep writing
     * through permissions that no longer exist. Reloading CR3 drops every
     * non-global entry, which is heavier than invalidating one page and is
     * what makes it correct without the sender having to say which page. */
    if (frame->vector == 0xFEu) {
        /* The request may already have been answered by a spin loop on this
         * core before the IPI landed; then there is nothing left to do. */
        hw_tlb_service_flush();
        vibeos_x86_64_lapic_eoi();
        return;
    }

    if (frame->vector >= VIBEOS_HW_IRQ_BASE && frame->vector < VIBEOS_HW_WIRED_VECTORS) {
        /* Somebody has panicked. Stop being a running machine.
         *
         * Checked here rather than only in the scheduler because a core that is
         * idle still takes this interrupt, and a half-stopped machine is what
         * made the original failure so hard to read. */
        if (g_panicked) {
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[PANIC] parked cpu 0x");
            vibeos_x86_64_serial_print_hex((uint64_t)vibeos_x86_64_cpu_id());
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
            for (;;) {
                __asm__ __volatile__("cli; hlt");
            }
        }
        if (frame->vector == VIBEOS_HW_IRQ_TIMER) {
            /* Every core, before the clock-owner test: a core that keeps
             * running the same task never reloads CR3 on its own, and that is
             * the core most likely to be holding the stale translation. */
            hw_tlbq_help_quiesce();
            /* Every core's LAPIC timer lands here; only one may own the clock. */
            if (!g_apic_mode || hw_this_cpu()->index == 0u) {
                g_timer_ticks++;
                /* Give back the frames whose stale translations have expired.
                 *
                 * On the clock owner only, and deliberately: this walks 512
                 * slots, and doing it on four cores buys nothing - the frames
                 * become releasable when *other* cores advance, which they do
                 * whether or not anybody is looking. munmap drains too, so a
                 * machine that is unmapping hard does not wait for a tick. */
                hw_tlbq_drain();
            }
            /* Accounting is charged by *every* core, unlike the clock above.
             *
             * The distinction matters and is easy to get backwards: the clock
             * owner counts wall time, and accounting counts CPU time. On four
             * cores those differ by a factor of four, and charging only the
             * owner would report a machine as three quarters idle no matter
             * what it was doing. */
            {
                hw_cpu_t *acpu = hw_this_cpu();
                int cur = acpu->current_task;
                int idle = 0;

                /* Bounded before it indexes the table, not after. This runs on
                 * every timer interrupt on every core, including before the
                 * task table means anything, and an interrupt handler that
                 * indexes an array with a value it has not checked is how a
                 * boot goes quiet somewhere unrelated. */
                if (cur < 0 || cur >= VIBEOS_HW_MAX_TASKS) {
                    cur = -1;
                } else {
                    idle = g_tasks[cur].id.is_idle;
                }
                vibeos_account_tick(acpu->index, cur, idle);
            }
            hw_pic_send_eoi((uint32_t)frame->vector);
            /* One core owns the network clock: draining the device from every
             * core would just contend on the same lock. */
            if (!g_apic_mode || hw_this_cpu()->index == 0u) {
                hw_net_pump();
                /* Repaint the display, whichever one is registered. Cheap by
                 * construction: the pointer's two small rectangles, and the
                 * text window when its contents changed. */
                vibeos_display_tick();
            }
            hw_schedule(frame); /* may rewrite the frame to switch tasks */
            /* Only when returning to ring 3: a signal frame goes on the user
             * stack, and there is not one to build on if the interrupt hit
             * kernel code. */
            if ((frame->cs & 3u) == 3u) {
                (void)hw_signal_deliver(frame);
            }
            return;
        }
        /* Every IOAPIC-delivered vector, counted once. The discriminating
         * measurement for "the device raises no interrupt": if nothing above
         * 32 but the timer ever arrives, delivery is broken for everything;
         * if the keyboard's arrives, delivery works and the fault is the
         * device's. */
        if (frame->vector > 32u && frame->vector < 48u) {
            g_ioapic_irqs[frame->vector - 33u]++;
        }
        /* A vector the device registry handed out: to the device in that slot.
         * The disks used to route their PCI lines to 42 and 43, chosen by hand
         * inside the legacy range, and each had its own branch here (C7). */
        if (frame->vector >= VIBEOS_DEVICE_VECTOR_BASE &&
            frame->vector < VIBEOS_DEVICE_VECTOR_BASE + VIBEOS_DEVICE_VECTORS) {
            if (vibeos_device_irq_vector((uint32_t)frame->vector) < 0) {
                g_device_stray_irqs++;
            }
            hw_pic_send_eoi((uint32_t)frame->vector);
            return;
        }
        /* A legacy line, to every device registered on it (C7). This used to
         * be one branch per driver, each naming it - the mouse on 44, the
         * keyboard on 33 - so a new input device meant editing this handler. */
        if (frame->vector >= 32u && frame->vector < 48u &&
            (vibeos_device_irq((int)frame->vector - 32) & VIBEOS_DEV_IRQ_INPUT)) {
            g_input_irq_wakes++;
            hw_keyboard_wake();
        }
        hw_pic_send_eoi((uint32_t)frame->vector);
        return;
    }

    fault_address = (frame->vector == 14u) ? hw_read_cr2() : 0u;

    /* A write to a shared page is not an error, it is the mechanism: fork
     * leaves both processes pointing at the same read-only frame, and this is
     * where the copy actually happens. Resolved faults must be handled before
     * anything is reported, or every fork would look like a crash. */
    if (frame->vector == 14u) {
        /* Timed and counted here rather than inside hw_handle_cow_fault, which
         * has five returns: at the call site the measurement is one pair of
         * reads and cannot miss an exit path. Counted only when the fault was
         * *resolved* - an unresolved one ends in a kill or a panic, and folding
         * those into the same number would make the mean drift with how often
         * the machine crashed rather than with how fast it faults. */
        uint64_t t0 = hw_tsc();
        int resolved = hw_handle_cow_fault(fault_address, frame->error_code,
                                           frame->rip);
        if (resolved) {
            hw_perf_add(&g_perf_fault, t0);
            return;
        }
        /* A user copy that faulted (H-003, H-010): the range was valid when it
         * was checked and is not now. Resume at the copy's recovery point, which
         * returns -1 to its caller. Only for the one instruction that copies,
         * only from ring 0, and only on a user address - a kernel address
         * faulting there is a kernel bug and still panics below. */
        if ((frame->cs & 3u) == 0u &&
            frame->rip >= (uint64_t)(uintptr_t)vibeos_uaccess_copy_begin &&
            frame->rip < (uint64_t)(uintptr_t)vibeos_uaccess_copy_end &&
            hw_user_addr_ok(fault_address)) {
            g_uaccess_recovered++;
            frame->rip = (uint64_t)(uintptr_t)&vibeos_uaccess_copy_fixup;
            return;
        }
    }

    /* A fault report is many small writes; keep another core from splitting it. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[HW][TRAP] ");
    hw_log_field("cpu", hw_this_cpu()->index);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("task", (uint64_t)(int64_t)hw_this_cpu()->current_task);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("vector", frame->vector);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("err", frame->error_code);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("rip", frame->rip);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("cs", frame->cs);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("rsp", frame->rsp);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("ss", frame->ss);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("rflags", frame->rflags);
    if (frame->vector == 14u) {
        vibeos_x86_64_serial_puts(" ");
        hw_log_field("cr2", fault_address);
    }

    if (!g_arch_trap_ready) {
        /* Handler ran before the trap model was initialized. Fail safe. */
        hw_panic("trap before model init");
    }

    tf.rip = frame->rip;
    tf.rsp = frame->rsp;
    tf.rflags = frame->rflags;
    tf.error_code = frame->error_code;
    tf.cs = frame->cs;
    tf.fault_address = fault_address;
    tf.vector = (uint32_t)frame->vector;

    /* No log is handed to the model. It used to write into the ring directly,
     * which bypassed the ring's lock and every sink, and - because it is asked
     * with pid 0 and knows nothing of privilege - recorded `kernel_fault_panic`
     * at FATAL for every ring-3 fault the branch below then correctly survives.
     * One false FATAL per svc-crash, every boot, in the ring a panic dumps. What
     * the machine actually does is logged below, when it does it. */
    if (vibeos_trap_dispatch_ex(&g_arch_trap_state, &tf, 0,
                                0, &decision) != 0) {
        hw_panic("trap dispatch failed");
    }

    /* What is about to happen, not what the model proposed.
     *
     * These are not the same, and for a while this line said so without anyone
     * reading it. The trap model has no notion of privilege level, so it
     * answers PANIC for a page fault; the branch below then overrides that for
     * a ring-3 fault and kills the task instead. The dump printed the model's
     * answer, so a boot that correctly killed one program logged
     * `action=PANIC` and then, on the next line, `killing task, not the
     * machine`. Two statements, one of them false, and the false one is the
     * one a reader reaches first.
     *
     * That mattered: chasing the svc-press failure, `action=PANIC` was read as
     * "the machine died here" for a whole investigation, when the machine had
     * done exactly the right thing and the wedge came later and elsewhere.
     *
     * The override is decided here, before the line is written, so the line is
     * true when it is printed rather than corrected afterwards. */
    {
        int ring3 = ((frame->cs & 3u) == 3u);
        const char *acted =
            (decision.action == VIBEOS_TRAP_ACTION_CONTINUE) ? "CONTINUE"
            : ring3 ? "KILL_CURRENT" : "PANIC";

        vibeos_x86_64_serial_puts(" -> action=");
        vibeos_x86_64_serial_puts(acted);
        if (ring3 && decision.action != VIBEOS_TRAP_ACTION_CONTINUE) {
            /* The model's answer is kept too. A fault the model would have
             * panicked on and a fault it would not are different situations,
             * and collapsing them would lose the distinction the override
             * exists to make. */
            vibeos_x86_64_serial_puts(" model=");
            vibeos_x86_64_serial_puts(hw_action_name(decision.action));
        }
        vibeos_x86_64_serial_puts(" count=0x");
        vibeos_x86_64_serial_print_hex(g_arch_trap_state.trap_count);
        vibeos_x86_64_serial_puts("\n");
    }
    vibeos_x86_64_serial_unlock();

    if (decision.action == VIBEOS_TRAP_ACTION_CONTINUE) {
        /* Resumable trap (e.g. #BP): iretq returns to the saved RIP. */
        return;
    }

    /* The trace comes from the interrupted frame's own base pointer, not from
     * this handler's: what is wanted is the path that reached the fault, and
     * the handler's own frames are noise on top of it. */
    hw_backtrace(frame->rbp, frame->rip);

    /* A fault in ring 3 is the program's fault, not the machine's.
     *
     * This used to panic unconditionally, under a comment saying KILL_CURRENT
     * had no meaning because there were no user processes on metal. That
     * stopped being true when init started supervising services, and nothing
     * noticed, because the services in the manifest all died by exiting - a
     * cooperative death that never reaches this code. So "the kernel stays up
     * after a service crashes" was gated, green, and false.
     *
     * It was found from the other end: musl's stack-check stub is a `hlt`, a
     * spurious canary failure in a ring-3 test binary executed it, and one
     * privileged instruction from an unprivileged program halted the whole
     * machine with no output. Any null dereference would have done the same.
     *
     * A kernel-mode fault is still fatal - there is nothing to kill but
     * itself, and continuing would be guessing. */
    if ((frame->cs & 3u) == 3u) {
        hw_fault_kill_current_user(frame, fault_address);  /* no return, if it can */
    }
    /* The code is the one the trap model used ('TTFK'), so a dump reads the
     * same as it always has; a1 is the faulting rip, which is where the gate
     * looks for the free-page poison executed. */
    hw_log(VIBEOS_LOG_FATAL, 0x5454464bu, frame->vector, frame->rip,
           "kernel_fault_panic");
    hw_panic("unrecoverable CPU exception");
}
/* setjmp-style kernel context saved by ring3_enter (see isr.S). Global so the
 * assembly can reference it by name. Layout: rbx,rbp,r12,r13,r14,r15,rsp. */
uint64_t g_ring3_kctx[8];

/* Remap the PIC, start the PIT, enable interrupts, and confirm the timer IRQ
 * actually fires by watching the tick counter advance (bounded so we never
 * hang if delivery is broken). */
static void hw_enable_timer_irq(void) {
    uint64_t start;
    uint32_t spins;

    vibeos_x86_64_serial_puts("[HW] enabling timer IRQ (PIC remap + PIT @100Hz)\n");
    g_timer_ticks = 0;
    hw_pic_remap();
    hw_pit_init();
    __asm__ __volatile__("sti");

    start = g_timer_ticks;
    for (spins = 0; spins < 1000000000u; spins++) {
        if (g_timer_ticks - start >= 3u) {
            break;
        }
    }

    if (g_timer_ticks >= 3u) {
        vibeos_x86_64_serial_puts("[HW] TIMER_IRQ_OK ticks=0x");
        vibeos_x86_64_serial_print_hex(g_timer_ticks);
        vibeos_x86_64_serial_puts("\n");
    } else {
        vibeos_x86_64_serial_puts("[HW] TIMER_IRQ_FAIL (no ticks observed)\n");
    }
}

/* ---- SYSCALL/SYSRET (native Linux ABI) ---------------------------------- */

#define MSR_EFER   0xC0000080u
#define MSR_STAR   0xC0000081u
#define MSR_LSTAR  0xC0000082u
#define MSR_SFMASK 0xC0000084u

extern void vibeos_x86_64_syscall_entry(void); /* trampoline in isr.S */

static uint64_t hw_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void hw_wrmsr(uint32_t msr, uint64_t value) {
    __asm__ __volatile__("wrmsr"
                         : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* Enable the `syscall`/`sysret` fast path. STAR selects the CS/SS pairs:
 * SYSCALL loads kernel CS=0x08 (SS=0x10); SYSRET loads user CS=0x20|3 and
 * SS=0x18|3 from base 0x10 - which matches the data-then-code user GDT order. */
static void hw_enable_syscall(void) {
    /* NXE as well as SCE (M-036). PTE_NX in a user leaf is a reserved bit - a
     * page fault on first touch - unless the core has it enabled, and firmware
     * leaves it enabled on the bootstrap processor but the application-processor
     * trampoline does not. Every core comes through here before it loads an
     * address space that carries the bit. */
    {
        uint32_t eax = 0x80000001u, ebx = 0, ecx = 0, edx = 0;
        __asm__ __volatile__("cpuid" : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));
        if ((edx & (1u << 20)) == 0u) {
            hw_panic("cpu has no NX: cannot enforce PROT_EXEC");
        }
    }
    hw_wrmsr(MSR_EFER, hw_rdmsr(MSR_EFER) | 1ull | (1ull << 11));     /* SCE | NXE */
    hw_wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    hw_wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)vibeos_x86_64_syscall_entry);
    hw_wrmsr(MSR_SFMASK, 0x200ull);                                /* clear IF on entry */
    if (hw_this_cpu()->index == 0u) {
        vibeos_x86_64_serial_puts("[HW] syscall/sysret enabled (LSTAR set)\n");
    }
}
/* ---- Process creation (ELF -> private address space) --------------------- */


/* hw_proc_t now lives in arch_hw_internal.h. */

typedef struct {
    vibeos_hw_aspace_t *as;
} hw_load_ctx_t;


/* Sixteen bytes for AT_RANDOM.
 *
 * This is not a random number generator and must not be used as one. There is
 * no entropy source in this system yet, so the bytes come from mixing the
 * timestamp counter - which differs between boots and between processes, and
 * is nothing better than that.
 *
 * It exists because AT_RANDOM is not optional in practice. A C runtime reads
 * the pointer the kernel puts there and dereferences it to seed the
 * stack-protector canary before it runs any of the program. Omitting the entry
 * leaves that pointer NULL, and the program dies on its own canary setup with
 * a null read - which is exactly how this was found.
 *
 * Supplying zeros would avoid the crash and be worse than the crash: every
 * process would run with an identical, known canary while appearing protected.
 * A varying value is honest about what it is. getrandom() still returns ENOSYS
 * for the same reason - this is good enough to make canaries differ, and not
 * good enough for anything a program would call getrandom for. */
void hw_seed_at_random(uint8_t out[16]) {
    uint32_t i;
    uint64_t mix = 0x9E3779B97F4A7C15ull;

    for (i = 0; i < 16u; i++) {
        uint32_t lo, hi;
        __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
        mix ^= ((uint64_t)hi << 32) | lo;
        mix *= 0xFF51AFD7ED558CCDull;
        mix ^= mix >> 33;
        out[i] = (uint8_t)(mix >> 24);
    }
}

/* Build a process: private address space, the ELF image loaded into it, and a
 * user stack mapped just below VIBEOS_HW_USER_STACK_TOP. */

/* Map `pages` fresh zeroed user pages at `va` in an address space. */
int hw_map_user_pages(vibeos_hw_aspace_t *as, uint64_t va, uint64_t pages) {
    uint64_t i;
    for (i = 0; i < pages; i++) {
        void *page = hw_alloc_page();
        if (!page || hw_map_page(as, va + i * 4096ull, (uint64_t)(uintptr_t)page,
                                 PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX) != 0) {
            /* The same two leaks mmap had, in the function brk grows through.
             * A frame allocated and not mapped had no owner to release it, and
             * a partial mapping was left for a caller just told the whole
             * operation failed. */
            uint64_t j;
            vibeos_vmspace_t v = hw_vm(as);

            if (page) {
                hw_page_put((uint64_t)(uintptr_t)page);
            }
            for (j = 0; j < i; j++) {
                (void)vibeos_vmspace_unmap(&v, va + j * 4096ull);
            }
            hw_tlbq_drain();
            return -1;
        }
        hw_page_put((uint64_t)(uintptr_t)page);   /* D9: the mapping owns it now */
    }
    return 0;
}
/* ---- Task table + preemptive scheduler ---------------------------------- */


static hw_lock_t g_pipe_lock;

/* The pipe module serialises itself with this lock (vibeos_pipe_set_lock, registered
 * at boot). It is the architecture's because a spin lock that masks interrupts is. */
static void hw_pipe_lock(void) {
    hw_spin_lock_named(&g_pipe_lock, "vibeos_pipe");
}

static void hw_pipe_unlock(void) {
    hw_spin_unlock(&g_pipe_lock);
}

void hw_pipe_init(void) {
    vibeos_pipe_set_lock(hw_pipe_lock, hw_pipe_unlock);
    vibeos_pipe_reset();
}
extern const unsigned char vibeos_user_task_elf[];
extern const unsigned long vibeos_user_task_elf_len;

/* hw_task_t now lives in arch_hw_internal.h, so the files lifted out
 * of here can see it. */

/* Point the CPU at a task's ring-0 stack: the TSS one is used when ring 3 is
 * interrupted, the syscall one when it issues `syscall`. */
static void hw_set_kernel_stack(uint64_t top) {
    hw_cpu_t *cpu = hw_this_cpu();
    cpu->tss.rsp0 = top;
    cpu->syscall_kstack_top = top;
}


/* Two contiguous pages when the PMM can give them, one otherwise. Reports the
 * base and page count so the stack can be reclaimed when the task exits. */
uint64_t hw_alloc_kstack(uint64_t *out_base, uint32_t *out_pages) {
    uint8_t *p = 0;
    uint64_t size = 8192ull;
    uint32_t pages = 2u;

    /* Two contiguous frames from the frame layer, not from the bump allocator
     * underneath it. The bump allocator is closed once the layer is up, and a
     * kernel stack taken from it would be a frame the layer believes is free. */
    p = (uint8_t *)hw_alloc_pages_contig(2u);
    if (!p) {
        p = (uint8_t *)hw_alloc_page();
        size = 4096ull;
        pages = 1u;
    }
    if (!p) {
        return 0;
    }
    if (out_base) {
        *out_base = (uint64_t)(uintptr_t)p;
    }
    if (out_pages) {
        *out_pages = pages;
    }
    return (uint64_t)(uintptr_t)p + size;
}

static void hw_free_kstack_pages(uint64_t base, uint32_t pages) {
    uint32_t i;
    for (i = 0; i < pages; i++) {
        hw_free_page((void *)(uintptr_t)(base + (uint64_t)i * 4096ull));
    }
}

void hw_drain_dead_kstack(void) {
    hw_cpu_t *cpu = hw_this_cpu();
    uint64_t base;
    uint32_t pages;

    if (cpu == 0 || cpu->dead_kstack_base == 0 || cpu->dead_kstack_pages == 0) {
        return;
    }
    /* Taken out of the slot before the pages are freed. hw_free_page can
     * schedule nothing and takes only the memory lock, but a slot still
     * holding an address whose pages are on the freelist is a second chance to
     * free them, and this is the one place that would ever get. */
    base = cpu->dead_kstack_base;
    pages = cpu->dead_kstack_pages;
    cpu->dead_kstack_base = 0;
    cpu->dead_kstack_pages = 0;

    /* The one assertion worth making here: this core must not be standing on
     * the stack it is about to free. If it is, the parking is wrong somewhere
     * and the machine should say so rather than corrupt itself quietly. */
    {
        uint64_t rsp;
        __asm__ __volatile__("mov %%rsp, %0" : "=r"(rsp));
        if (rsp >= base && rsp < base + (uint64_t)pages * 4096ull) {
            hw_panic("draining the kernel stack this core is standing on");
        }
    }
    hw_free_kstack_pages(base, pages);
    vibeos_task_stats()->dead_kstacks_freed++;
}

hw_task_t g_tasks[VIBEOS_HW_MAX_TASKS];

/* Recorded by hw_task_release when it finds a core still current on the slot it
 * is about to free, and printed by hw_panic_cpu_summary - which runs with the
 * cores quiesced, so the line comes out intact instead of interleaved with the
 * other cores' simultaneous panic the way the raw print did. */
static volatile uint64_t g_relrace_rip;
static volatile int g_relrace_curcpu = -1;
static volatile int g_relrace_relcpu = -1;
static volatile int g_relrace_slot = -1;
static uint32_t g_alloc_seq;
uint32_t g_console_foreground_pgid;
vibeos_service_supervisor_t g_runtime_supervisor;
uint8_t g_runtime_supervisor_ready;

/* Claim a free slot atomically: two cores can fork at the same time, so the
 * slot is marked RESERVED (never schedulable, never reapable) until the caller
 * has finished filling it in. */
/* Answer the portable task view. Phase S-P0 of docs/sched/.
 *
 * This is the whole of what the architecture layer owes the rest of the kernel
 * about a task: a copy of one slot. Deciding what to print is portable and
 * lives in kernel/sched/view.c; reading the table is not and lives here. The
 * split is the first piece of the scheduler rewrite, and it is deliberately the
 * cheapest one - a subsystem that cannot be looked at is a subsystem that gets
 * debugged by adding print statements to an eight-thousand-line file, which is
 * how every task defect in this project has actually been found.
 *
 * A copy, not a pointer: the console prints this while other cores create and
 * destroy tasks, and a pointer would name a slot that can be recycled between
 * one field and the next. */
const char *hw_task_state_name(int state) {
    switch (state) {
        case HW_TASK_FREE:     return "free";
        case HW_TASK_READY:    return "ready";
        case HW_TASK_RUNNING:  return "running";
        case HW_TASK_ZOMBIE:   return "zombie";
        case HW_TASK_BLOCKED:  return "blocked";
        case HW_TASK_RESERVED: return "reserved";
        default:               return "?";
    }
}

static uint32_t hw_task_slots(void) {
    return (uint32_t)VIBEOS_HW_MAX_TASKS;
}

/* The only function that changes a task's state. Phase S-P1 of docs/sched/.
 *
 * It asks the portable transition table first, so an illegal change is refused
 * and counted rather than performed - and the two most expensive defects this
 * kernel has had were illegal changes that nothing refused: a slot released
 * while its task was still on a CPU, and a dying task announced before its
 * address space was gone.
 *
 * Both the layer's copy and this file's field are written here, in one place,
 * so they cannot disagree. The field stays because forty reads use it and
 * moving storage is S-P3's work; what has moved already is the decision about
 * whether the change is allowed at all.
 *
 * `why` is kept and printed by `tasks`. Three fields in this struct exist
 * because somebody once needed exactly that and did not have it. */
int hw_task_set_state(int slot, vibeos_task_state_t to, const char *why) {
    if (slot < 0 || slot >= (int)VIBEOS_HW_MAX_TASKS) {
        return -1;
    }
    if (vibeos_task_transition((uint32_t)slot, to, why) != 0) {
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[TASKS] ILLEGAL slot=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)slot);
        vibeos_x86_64_serial_puts(" from=");
        vibeos_x86_64_serial_puts(vibeos_task_state_name(
            (vibeos_task_state_t)hw_slot_state(slot)));
        vibeos_x86_64_serial_puts(" to=");
        vibeos_x86_64_serial_puts(vibeos_task_state_name(to));
        vibeos_x86_64_serial_puts(" by=");
        vibeos_x86_64_serial_puts(why ? why : "-");
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
        return -1;
    }
    return 0;
}
static void hw_fpu_save(unsigned char *area);

int hw_task_alloc_guarded(int guarded, int privileged, uint32_t parent_pid,
                                 vibeos_fork_verdict_t *out_verdict) {
    int i;
    if (out_verdict) {
        *out_verdict = VIBEOS_FORK_OK;
    }
    hw_spin_lock_named(&g_sched_lock, __func__);
    if (guarded) {
        uint32_t in_use = 0, kids = 0;
        vibeos_fork_verdict_t v;

        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (hw_slot_state(i) == HW_TASK_FREE) {
                continue;
            }
            in_use++;
            /* What this thread group is accountable for, which is *not* just
             * its children.
             *
             * A thread inherits its creator's parent rather than becoming its
             * child - `child->ppid = parent->ppid` in clone - so counting by
             * ppid alone bounds fork and does not bound threads at all. With
             * the ceiling set to two, a boot that creates four threads produced
             * no clone refusal whatsoever: the reserve stopped a thread bomb
             * from taking the machine, and the per-task ceiling did nothing.
             *
             * So both are counted: tasks whose parent is this group, and other
             * tasks in this group. Counted rather than tracked in a field,
             * because a counter has to be right on every exit path including
             * the ones that fail half way, and this table is twenty-four
             * entries long. Zombies count, because a zombie still holds a
             * slot. */
            if (vibeos_task_accountable_to(&g_tasks[i].id, parent_pid)) {
                kids++;
            }
        }
        v = vibeos_forkguard_check(in_use, kids, privileged);
        if (v != VIBEOS_FORK_OK) {
            hw_spin_unlock(&g_sched_lock);
            if (out_verdict) {
                *out_verdict = v;
            }
            return -1;
        }
    }
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (hw_slot_state(i) == HW_TASK_FREE) {
            /* A recycled slot must not keep the previous tenant's address
             * space. Leaving cr3 behind is not a tidiness problem: the page it
             * names has been freed and handed back to the allocator, so a slot
             * scheduled before its creator finishes filling it in installs a
             * page table that belongs to somebody else now. Zeroing it makes
             * that a named panic on the next context switch instead of a
             * machine that stops without a word.
             *
             * The three markers go with it, or a diagnostic reads as the
             * history of whoever had the slot last. */
            g_tasks[i].cr3 = 0;
            g_tasks[i].cr3_set_by = 0;
            g_tasks[i].ready_by = 0;
            /* Marked, not cleared: this is the other way pml4 reaches zero,
             * and leaving it blank made the guard's report ambiguous between
             * "the space was destroyed" and "the slot was handed out again". */
            g_tasks[i].aspace_killed_by = "task_alloc_clear";
            /* A defined FPU state, not the previous tenant's. Same family as
             * interp_base: a field written on one path and read on all of them
             * hands out whatever was there. */
            hw_fpu_init_area(g_tasks[i].fpu);
            g_tasks[i].alloc_seq = (uint32_t)__sync_add_and_fetch(&g_alloc_seq, 1u);
            g_tasks[i].proc.as.pml4 = 0;
            g_tasks[i].ps = 0;
            /* Every identity field, not the three that happened to be remembered:
             * a recycled slot otherwise starts with the previous tenant's pending
             * signals, exit status and name. */
            vibeos_task_identity_reset(&g_tasks[i].id);
            g_tasks[i].abi = vibeos_abi_linux();   /* bound once, here (C4) */
            (void)hw_task_set_state(i, HW_TASK_RESERVED, __func__);
            /* Admitted here because this is the only place a slot is claimed,
             * so no way of making a task can forget to. The class is corrected
             * by the idle-task path, which is the only caller that knows it is
             * making one; everything else is a normal task at nice 0 until
             * something says otherwise. */
            (void)vibeos_sched_policy_admit((uint32_t)i, VIBEOS_SCHED_NORMAL, 0, 0u);
            vibeos_task_stats()->created++;
            hw_spin_unlock(&g_sched_lock);
            return i;
        }
    }
    vibeos_task_stats()->slot_refused++;
    hw_spin_unlock(&g_sched_lock);
    if (out_verdict) {
        *out_verdict = VIBEOS_FORK_NO_SLOTS;
    }
    return -1;
}

/* Unguarded, for the kernel's own tasks and the idle tasks. */
int hw_task_alloc(void) {
    return hw_task_alloc_guarded(0, 1, 0u, 0);
}

/* Give a reserved slot back after a failed creation. */
void hw_task_release(int i) {
    /* No core may still call this slot its current task. The releasing core has
     * already made `next` current (hw_schedule/the exit path set current_task
     * before this), so it never matches; a *different* core that does is running
     * on a slot about to go FREE and have its kernel stack reclaimed - the
     * four-worker use-after-free, named at its source rather than later at the
     * kstack free. */
    if (i >= 0 && i < VIBEOS_HW_MAX_TASKS) {
        uint32_t c;
        for (c = 0; c < VIBEOS_HW_MAX_CPUS; c++) {
            if (g_cpus[c].current_task != i) {
                continue;
            }
            /* hw_task_release runs under g_sched_lock, so the console lock would
             * deadlock here; record the culprit in globals instead and let
             * hw_panic_cpu_summary print them once the cores are quiesced -
             * intact, and with the caller of hw_task_release named. */
            g_relrace_rip = (uint64_t)(uintptr_t)__builtin_return_address(0);
            g_relrace_curcpu = (int)c;
            g_relrace_relcpu = (int)hw_this_cpu()->index;
            g_relrace_slot = i;
            hw_panic("releasing a slot a cpu is still current on");
        }
    }
    /* Forgotten before the slot is published, not after: a slot the policy
     * still knows about is one it will schedule the moment somebody else
     * claims it, charging the new tenant's time to the old one's history. */
    if (i >= 0 && i < VIBEOS_HW_MAX_TASKS) {
        vibeos_sched_policy_forget((uint32_t)i);
    }
    if (i >= 0) {
        (void)hw_task_set_state(i, HW_TASK_FREE, __func__);
    }
}

/* Next task to run on `cpu`, round robin. Only READY tasks are candidates: a
 * RUNNING one is owned by some core (possibly another), so on SMP it must never
 * be picked twice. Idle tasks are skipped unless nothing else is available.
 * Returns -1 when the caller should simply keep running what it has. */
/* Is this slot something `cpu` may run?
 *
 * The one thing the run queue cannot answer for itself, and deliberately so:
 * the state machine is the single source of truth about what READY means, and
 * a queue that kept its own opinion would be a second one. An idle task is
 * excluded because it is the fallback, not a candidate; a task already on a
 * CPU is excluded because two cores running one task is a defect this kernel
 * has had. */
static int hw_task_runnable(void *ctx, uint32_t slot, uint32_t cpu) {
    (void)ctx;
    (void)cpu;
    if (slot >= (uint32_t)VIBEOS_HW_MAX_TASKS) {
        return 0;
    }
    return vibeos_task_state(slot) == VIBEOS_TASK_READY &&
           !g_tasks[slot].id.is_idle && !g_tasks[slot].on_cpu;
}

vibeos_runq_t g_runq;

/* Now a wrapper. The scan, the cursor and the idle fallback are in
 * kernel/sched/runq.c, where "two CPUs never pick the same slot" is a unit test
 * rather than a boot somebody hopes goes wrong.
 *
 * One behaviour changed in the move, and it is a fix: the old scan started from
 * the *current* task, which is fair while something is running and degenerates
 * when the current task is idle - the search then begins at the same place
 * every time and the high-numbered slots wait behind the low ones. The queue
 * keeps a cursor per CPU instead. */
/* How long this task may hold a core, in ticks. An idle task gets the
 * shortest slice there is: it must give the core back the moment anything else
 * wants it, and its slice only bounds how long a core stays idle when work has
 * appeared and no timer has fired yet. */
uint32_t hw_slice_for(int slot) {
    vibeos_sched_class_t cls;

    if (slot < 0 || slot >= VIBEOS_HW_MAX_TASKS) {
        return 1u;
    }
    /* Asked, not derived. Deriving it from is_idle was the same mistake in
     * two places: it cannot see a KERNEL task, so the top class got a NORMAL
     * slice. */
    cls = vibeos_sched_policy_class((uint32_t)slot);
    return vibeos_sched_policy_quantum(cls);
}

/* Is anything runnable that outranks what this core is running?
 *
 * Only a class comparison, deliberately. Preempting for a *better-weighted*
 * task in the same class would make the slice meaningless - the whole point of
 * a slice is that a task keeps the core for it - and weights are already
 * honoured when the next task is chosen. A class is different: it is a promise
 * that higher work runs first, and a promise that waits for a slice boundary is
 * a promise with an asterisk. */
static int hw_higher_class_runnable(hw_cpu_t *cpu, int current) {
    uint32_t i;
    uint64_t runnable = 0;

    if (current < 0 || current >= VIBEOS_HW_MAX_TASKS) {
        return 1;
    }
    for (i = 0; i < (uint32_t)VIBEOS_HW_MAX_TASKS; i++) {
        if ((int)i != current && hw_task_runnable(0, i, (uint32_t)cpu->index)) {
            runnable |= 1ull << i;
        }
    }
    /* The rule itself lives with the policy, so this cannot half-implement it:
     * this function tested only "current is IDLE", and a KERNEL task waited
     * behind a NORMAL one for the rest of its slice (M-035). */
    return vibeos_sched_policy_should_preempt((uint32_t)cpu->index, current, runnable);
}

int hw_pick_next(hw_cpu_t *cpu) {
    uint64_t runnable = 0;
    uint32_t i;
    int chosen;

    /* The run queue decides *who may run*; the policy decides *which of them*.
     *
     * Keeping those two apart is the reason the policy could be written as a
     * pure function and tested exhaustively: it never asks about task state,
     * only about the set it is handed. hw_task_runnable stays the single
     * definition of runnable, so a task the scheduler would not have run
     * cannot become runnable by being favoured.
     */
    /* The bitmask is 64 bits wide, so the table must fit in it. This was a
     * runtime `i < 64u` that has never once been false, which is worse than no
     * check at all: it reads as a guard, it costs a comparison per slot, and
     * it would have silently dropped slots 64 and up rather than failing. A
     * static assertion cannot be true-by-accident. */
    _Static_assert(VIBEOS_HW_MAX_TASKS <= 64,
                   "the runnable bitmask is 64 bits wide");
    for (i = 0; i < (uint32_t)VIBEOS_HW_MAX_TASKS; i++) {
        if (hw_task_runnable(0, i, (uint32_t)cpu->index)) {
            runnable |= (1ull << i);
        }
    }

    chosen = vibeos_sched_policy_pick((uint32_t)cpu->index, runnable);
    if (chosen >= 0) {
        return chosen;
    }

    /* Nothing the policy knows about. Fall back rather than idle: a task the
     * policy was never told about is a bug in admission, and refusing to run it
     * would turn that bug into a hang instead of a slower machine. The run
     * queue also owns the "keep running the current task" answer, which the
     * policy has no way to express. */
    return vibeos_runq_pick(&g_runq, (uint32_t)cpu->index, cpu->current_task);
}

/* Wake every task blocked in read() on stdin (called from the keyboard IRQ). */
/* The console as the syscall layer sees it: a byte in, a byte echoed. The Linux ABI
 * layer used to name the keyboard and the framebuffer directly, which was invisible
 * inside one 12,000-line file and became a dependency of its own the moment the
 * handlers moved out (check-blast-radius counted an "input device" edit in fs.c).
 * What a read() from the console needs is these two, and which device supplies
 * them is the architecture layer's business. */
extern void vibeos_x86_64_fb_putc(char c);

int hw_console_getc(void) {
    return vibeos_input_getc();
}

void hw_console_echo(char c) {
    vibeos_x86_64_fb_putc(c);
}

/* Load the CPU state that belongs to a task rather than to the CPU: its page
 * tables, its kernel stack, and its thread-local storage base.
 *
 * This exists as one function because there is more than one way to resume a
 * task - the timer switch, the exit path, and the idle entry - and each of
 * them has to reload all of it. Missing the TLS base in one of them is not
 * visible at the point of the mistake: the task simply reads through whatever
 * base the previous occupant of the CPU left behind, which is a fault if that
 * was zero and someone else's thread state if it was not. */
/* ---- the x87/SSE register file across a context switch --------------------
 *
 * The kernel enabled SSE and never saved it. See
 * docs/implementation_progress/mm_no_fpu_context.md: a task's vector registers
 * survived a syscall or an interrupt by luck, and because clang stores 16-byte
 * stack buffers with movdqa, a user buffer that lost a store lost exactly
 * sixteen bytes - one register - which is the width that had no explanation
 * while the search was in the memory manager.
 *
 * Eager, not lazy. CR0.TS with a trap on first use is the classic optimisation
 * and it is deliberately not taken here: it needs a fault handler, a per-core
 * notion of who owns the registers, and cross-core invalidation when a task
 * migrates - three mechanisms, on the path this kernel has the most defects in.
 * fxsave and fxrstor are about a hundred cycles each on a switch that already
 * costs far more, and C0's histogram is there to say if that is wrong.
 */
static void hw_fpu_save(unsigned char *area) {
    __asm__ __volatile__("fxsave (%0)" :: "r"(area) : "memory");
}

void hw_fpu_restore(const unsigned char *area) {
    __asm__ __volatile__("fxrstor (%0)" :: "r"(area) : "memory");
}

/* The state a task starts with, built by hand rather than by fxsave.
 *
 * Zeroing the area is not enough and the reason is easy to miss: MXCSR lives at
 * offset 24, and zero there unmasks every SSE exception, so the first floating
 * point operation in a fresh program faults. 0x1F80 is the reset value - all
 * exceptions masked - and 0x037F at offset 0 is the x87 control word's.
 *
 * By hand rather than fxsave-ing a freshly initialised unit, because doing that
 * would mean disturbing the FPU of whatever task happens to be running when a
 * new one is created. */
void hw_fpu_init_area(unsigned char *area) {
    uint32_t i;

    for (i = 0; i < 512u; i++) {
        area[i] = 0u;
    }
    area[0] = 0x7Fu;  area[1] = 0x03u;   /* FCW  = 0x037F */
    area[24] = 0x80u; area[25] = 0x1Fu;  /* MXCSR = 0x1F80 */
    area[28] = 0xFFu; area[29] = 0xFFu;  /* MXCSR_MASK = 0xFFFF */
}

void hw_task_load_cpu_state(int idx) {
    /* Counted, not timed. This function *is* the switch - it ends by installing
     * cr3 and returning into the next task - so there is no second point at
     * which to stop a clock: the cycles after the cr3 write belong to whoever
     * runs next. A count is still the number that matters here, because what
     * the refactor could change is how often the machine switches, and that is
     * deterministic. */
    __sync_fetch_and_add(&g_perf_switch.count, 1ull);

    /* Refuse to install an address space that no longer maps the kernel.
     *
     * This is the check that turned the intermittent wedge from a mystery into
     * an event with a name. Asking the emulator where a stopped guest's cores
     * were put one of them, every time, on the CR3 write below - holding a
     * PML4 whose entry 0, the kernel's own mapping, was not present. After
     * that write the next instruction fetch has nowhere to come from, and the
     * machine cannot even report a fault, because reporting one means running
     * kernel code. Hence silence rather than a panic.
     *
     * Entry 0 goes missing because a freed page is where the allocator keeps
     * its freelist link: hw_free_page writes the next pointer into offset zero
     * of the page it is reclaiming, and offset zero of a PML4 is exactly the
     * kernel's entry. So an address space that has been destroyed does not
     * merely become stale, it becomes an address space with no kernel in it -
     * and the pointer stored there is page-aligned, so the present bit reads
     * as clear.
     *
     * Panicking here is not a fix; it converts an unexplainable silence into a
     * backtrace, a log dump and the identity of the task involved, which is
     * what the fix will be built from. */
    {
        const uint64_t *pml4 =
            (const uint64_t *)(uintptr_t)(g_tasks[idx].cr3 & ~0xFFFull);

        if (pml4 == 0 || (pml4[0] & PTE_PRESENT) == 0) {
            vibeos_x86_64_serial_puts("[SCHED] address space has no kernel:"
                                      " task=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)idx);
            vibeos_x86_64_serial_puts(" pid=0x");
            vibeos_x86_64_serial_print_hex(g_tasks[idx].id.pid);
            vibeos_x86_64_serial_puts(" cr3=0x");
            vibeos_x86_64_serial_print_hex(g_tasks[idx].cr3);
            vibeos_x86_64_serial_puts(" pml4[0]=0x");
            vibeos_x86_64_serial_print_hex(pml4 ? pml4[0] : 0);
            /* What *kind* of task this is decides where to look. A cr3 of zero
             * and a cr3 pointing at a recycled page are different bugs wearing
             * the same symptom: the first is a task made runnable before its
             * address space was installed, the second one whose space was
             * taken away afterwards. Reading the code did not separate them,
             * so the guard says which it is holding. */
            vibeos_x86_64_serial_puts(" state=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)hw_slot_state(idx));
            vibeos_x86_64_serial_puts(" user=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].id.is_user);
            vibeos_x86_64_serial_puts(" idle=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].id.is_idle);
            vibeos_x86_64_serial_puts(" on_cpu=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].on_cpu);
            vibeos_x86_64_serial_puts(" ppid=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].id.ppid);
            vibeos_x86_64_serial_puts(" pml4=0x");
            vibeos_x86_64_serial_print_hex(
                (uint64_t)(uintptr_t)g_tasks[idx].proc.as.pml4);
            vibeos_x86_64_serial_puts(" cr3_set_by=");
            vibeos_x86_64_serial_puts(g_tasks[idx].cr3_set_by
                                      ? g_tasks[idx].cr3_set_by : "never");
            vibeos_x86_64_serial_puts(" ready_by=");
            vibeos_x86_64_serial_puts(g_tasks[idx].ready_by
                                      ? g_tasks[idx].ready_by : "never");
            vibeos_x86_64_serial_puts(" aspace_killed_by=");
            vibeos_x86_64_serial_puts(g_tasks[idx].aspace_killed_by
                                      ? g_tasks[idx].aspace_killed_by : "never");
            vibeos_x86_64_serial_puts(" last_destroy=");
            vibeos_x86_64_serial_puts(g_last_destroy_why ? g_last_destroy_why
                                                         : "none");
            vibeos_x86_64_serial_puts(" of=0x");
            vibeos_x86_64_serial_print_hex(g_last_destroy_pml4);
            vibeos_x86_64_serial_puts(" on_cpu=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_last_destroy_cpu);
            vibeos_x86_64_serial_puts("\n");
            hw_log(VIBEOS_LOG_FATAL, 5u, g_tasks[idx].cr3,
                   (uint64_t)g_tasks[idx].id.pid,
                   "scheduler asked to install a freed address space");
            hw_panic("address space freed while still schedulable");
        }
    }
    if (g_tasks[idx].id.is_thread && vibeos_task_first_run((uint32_t)idx)) {
        hw_log(VIBEOS_LOG_DEBUG, 24u, (uint64_t)g_tasks[idx].id.pid,
               g_tasks[idx].ctx.rip, "thread scheduled for the first time");
    }
    hw_write_cr3(g_tasks[idx].cr3);
    hw_set_kernel_stack(g_tasks[idx].kstack_top);
    if (g_tasks[idx].id.is_user) {
        hw_wrmsr(MSR_FS_BASE, g_tasks[idx].fs_base);
    }
}

/* Timer-driven preemption: save the interrupted task, pick the next runnable
 * one, and resume it by rewriting the live IRQ frame and switching CR3. Runs
 * for the life of the system - there is no "demo over" exit.
 *
 * On SMP every core's local-APIC timer calls this independently; the run queue
 * is shared, so the pick-and-claim step is done under the scheduler lock. */
/* Refuse to enter a context that cannot be a context. Phase S-P1 of docs/sched/.
 *
 * The failure this exists for: a #GP inside vibeos_x86_64_task_enter, and then
 * a second #GP at rip=0xe4e4e4e4e4e4e4e4 - a saved register context holding a
 * repeated fill byte. iretq consumed it, so the machine died on the way into
 * ring 3 with the panic handler running on the same broken state, and every
 * boot it happened in reported nothing but silence.
 *
 * iretq is the wrong place to find out. It has no way to say which task, which
 * slot, or who filled it in; it just faults, and by then the state that would
 * have named the culprit is the state that caused the fault. Six comparisons
 * before the jump turn that into a sentence.
 *
 * What is checked is only what the hardware requires and this kernel controls:
 * the two selector pairs it ever uses, a canonical rip and rsp, and the two
 * rflags bits that are not negotiable (bit 1 is always set; IF must be on or
 * the task can never be preempted again). Anything else a task may legitimately
 * hold. */
void hw_ctx_check(int slot, const char *where) {
    const vibeos_x86_64_isr_frame_t *c;
    const char *bad = 0;

    if (slot < 0 || slot >= VIBEOS_HW_MAX_TASKS) {
        hw_panic("task_enter on a slot that does not exist");
    }
    c = &g_tasks[slot].ctx;

    if (c->cs == VIBEOS_HW_USER_CODE_SEL) {
        if (c->ss != VIBEOS_HW_USER_DATA_SEL) { bad = "user cs with a non-user ss"; }
    } else if (c->cs == VIBEOS_HW_KERNEL_CS) {
        if (c->ss != 0x10u) { bad = "kernel cs with a non-kernel ss"; }
    } else {
        bad = "cs is not a selector this kernel issues";
    }
    /* Canonical: bits 63..47 all equal. A fill byte fails this, and so does a
     * pointer read out of a page that has been recycled. */
    if (!bad && ((c->rip >> 47) != 0ull && (c->rip >> 47) != 0x1FFFFull)) {
        bad = "rip is not canonical";
    }
    if (!bad && ((c->rsp >> 47) != 0ull && (c->rsp >> 47) != 0x1FFFFull)) {
        bad = "rsp is not canonical";
    }
    if (!bad && (c->rflags & 0x2ull) == 0ull) { bad = "rflags bit 1 clear"; }
    if (!bad && (c->rflags & 0x200ull) == 0ull) { bad = "rflags has interrupts off"; }
    if (!bad) {
        return;
    }

    /* One call, because a report assembled from a dozen serial_puts is a dozen
     * critical sections and three other cores are writing their own. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[CTX] refusing to enter task: ");
    vibeos_x86_64_serial_puts(bad);
    vibeos_x86_64_serial_puts(" from=");
    vibeos_x86_64_serial_puts(where ? where : "-");
    vibeos_x86_64_serial_puts(" slot=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)slot);
    vibeos_x86_64_serial_puts(" gen=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[slot].alloc_seq);
    vibeos_x86_64_serial_puts(" state=");
    vibeos_x86_64_serial_puts(hw_task_state_name(hw_slot_state(slot)));
    vibeos_x86_64_serial_puts(" state_by=");
    vibeos_x86_64_serial_puts(vibeos_task_last_why((uint32_t)slot));
    vibeos_x86_64_serial_puts(" ready_by=");
    vibeos_x86_64_serial_puts(g_tasks[slot].ready_by ? g_tasks[slot].ready_by : "-");
    vibeos_x86_64_serial_puts(" on_cpu=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[slot].on_cpu);
    vibeos_x86_64_serial_puts(" pid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[slot].id.pid);
    vibeos_x86_64_serial_puts(" cr3=0x");
    vibeos_x86_64_serial_print_hex(g_tasks[slot].cr3);
    vibeos_x86_64_serial_puts(" cr3_by=");
    vibeos_x86_64_serial_puts(g_tasks[slot].cr3_set_by ? g_tasks[slot].cr3_set_by : "-");
    vibeos_x86_64_serial_puts(" exe=");
    vibeos_x86_64_serial_puts(g_tasks[slot].proc.exe_path[0] ?
                              g_tasks[slot].proc.exe_path : "-");
    vibeos_x86_64_serial_puts(" rip=0x");
    vibeos_x86_64_serial_print_hex(c->rip);
    vibeos_x86_64_serial_puts(" cs=0x");
    vibeos_x86_64_serial_print_hex(c->cs);
    vibeos_x86_64_serial_puts(" rflags=0x");
    vibeos_x86_64_serial_print_hex(c->rflags);
    vibeos_x86_64_serial_puts(" rsp=0x");
    vibeos_x86_64_serial_print_hex(c->rsp);
    vibeos_x86_64_serial_puts(" ss=0x");
    vibeos_x86_64_serial_print_hex(c->ss);
    vibeos_x86_64_serial_puts(" rax=0x");
    vibeos_x86_64_serial_print_hex(c->rax);
    vibeos_x86_64_serial_puts(" rbp=0x");
    vibeos_x86_64_serial_print_hex(c->rbp);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    /* A panic, not a skip. The context is already unusable, and a task quietly
     * dropped from the run queue is the silence this was written to end. */
    hw_panic("a task context was not enterable");
}

static void hw_schedule(vibeos_x86_64_isr_frame_t *frame) {
    hw_cpu_t *cpu = hw_this_cpu();
    int cur, next;

    if (!g_sched_running) {
        return;
    }

    /* A stack left behind by a task that exited on this core. Safe here for
     * the reason the whole mechanism exists: this core is running on the
     * current task's stack, which is never the parked one. */
    hw_drain_dead_kstack();

    /* The quantum, finally consulted.
     *
     * Preemption used to happen on every timer tick, so the time slice was
     * "whatever the timer period is" - a number nobody chose, and the thing
     * step 1 of S-P5 exists to remove. The policy states a slice per class;
     * this is where it is spent.
     *
     * Two ways out of a slice before it is used up, and both are required
     * rather than convenient:
     *
     *   the task stopped being runnable - it blocked, exited, or was killed,
     *   and holding a core for the rest of a slice nobody is using is just a
     *   stall;
     *
     *   something in a higher class became runnable - a class is only absolute
     *   if it preempts, otherwise a kernel task waits behind a normal one for
     *   the rest of its slice and the guarantee is only true between slices.
     *
     * Checked before taking the scheduler lock, because the common case is
     * "keep running" and taking a lock to decide not to switch is the cost
     * this whole path is trying to avoid. */
    if (cpu->slice_left > 0u) {
        int c = cpu->current_task;

        if (c >= 0 && c < VIBEOS_HW_MAX_TASKS && !g_tasks[c].id.is_idle &&
            vibeos_task_state((uint32_t)c) == VIBEOS_TASK_RUNNING &&
            !hw_higher_class_runnable(cpu, c)) {
            cpu->slice_left--;
            return;
        }
        cpu->slice_left = 0;   /* the slice is over, whatever is left of it */
    }

    hw_spin_lock_named(&g_sched_lock, __func__);
    cur = cpu->current_task;
    next = hw_pick_next(cpu);
    if (next < 0 || next == cur) {
        hw_spin_unlock(&g_sched_lock);
        return; /* nothing else runnable: keep running the current task */
    }
    if (cur >= 0) {
        g_tasks[cur].ctx = *frame;
        /* Beside the integer context, and for the same reason it is here: after
         * on_cpu is cleared another core may take this task, and it must find
         * every register it left behind. */
        hw_fpu_save(g_tasks[cur].fpu);
        if (hw_slot_state(cur) == HW_TASK_RUNNING) {
            (void)hw_task_set_state(cur, HW_TASK_READY, __func__);
            HW_TASK_MARK(cur, ready_by, "preempted");
        }
        /* Only now is it safe for another core to take it: its context is
         * saved and this core is about to stop touching it. */
        g_tasks[cur].on_cpu = 0;
    }
    cpu->current_task = next;
    (void)hw_task_set_state(next, HW_TASK_RUNNING, __func__);
    g_tasks[next].on_cpu = 1;
    vibeos_account_switch(hw_this_cpu()->index, next, vibeos_task_ready_at((uint32_t)next));
    hw_this_cpu()->slice_left = hw_slice_for(next);
    *frame = g_tasks[next].ctx;
    hw_fpu_restore(g_tasks[next].fpu);
    hw_spin_unlock(&g_sched_lock);

    /* The ordinary switch, and the one that matters most: this frame is about
     * to be consumed by the iretq at the end of the ISR, several returns away
     * from anything that could say which task it belonged to.
     *
     * Checked outside the scheduler lock deliberately - the report takes the
     * console lock, and taking that one inside this one is a lock order this
     * kernel does not otherwise use. `next` is marked on_cpu here, so no other
     * core can be writing the context being read. */
    hw_ctx_check(next, "preempt");

    hw_task_load_cpu_state(next);
}

/* Does anyone else still hold this address space?
 *
 * Threads share page tables, so the space may only be destroyed by whoever
 * leaves last. The answer is derived from the task table rather than from a
 * reference count kept alongside it: the table is what the scheduler already
 * believes, and a second count of the same thing is a second thing that can
 * disagree with the first. Yesterday's silent wedge was exactly a teardown
 * acting on a belief the scheduler no longer shared.
 *
 * Must be called with g_sched_lock held: without it the answer can be stale by
 * the time it is acted on, which is the whole failure mode.
 */
/* Does any live task other than `except` run on these tables?
 *
 * Asked by address space rather than by task, because exec needs the same
 * question about tables it has *already* stopped using - and asking it through
 * the task would look at the new address space, which is not the one about to
 * be freed. Both callers share this so the two can never answer differently.
 *
 * The caller holds g_sched_lock. */
int hw_aspace_shared_by_other(const uint64_t *pml4, int except) {
    int i;

    if (pml4 == 0) {
        return 0;
    }
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (i == except || hw_slot_state(i) == HW_TASK_FREE) {
            continue;
        }
        /* A zombie is not running, but it has not been reaped either, and its
         * cr3 still names these tables - the guard in hw_task_load_cpu_state
         * would fire if they were freed under it. */
        if (g_tasks[i].proc.as.pml4 == pml4) {
            return 1;
        }
    }
    return 0;
}

int hw_aspace_still_shared(int me) {
    const uint64_t *pml4 = g_tasks[me].proc.as.pml4;
    int i;

    if (pml4 == 0) {
        return 0;
    }
    (void)i;
    return hw_aspace_shared_by_other(pml4, me);
}
/* ---- shared with the Linux ABI layer (kernel/abi/linux) ---------------------
 *
 * The syscall handlers moved to kernel/abi/linux (C4 stage 3). What is left in
 * this stretch is what they and the trap handlers both stand on: the
 * architecture half of signals, task lookup and exit, the crash dumper. User-
 * memory validation, the page-table primitives, the TLB shootdown and the
 * copy-on-write fault moved to mm_bridge.c (2026-09-28). Each is declared in
 * arch_hw_internal.h, which is the whole size of the seam. */

#define PROT_READ  0x1
#define MAP_PRIVATE   0x02


static void hw_panic_cpu_summary(void) {
    /* Who else was running what.
     *
     * A ring-3 fault gets a full crash record; a panic got a backtrace of the
     * one core that noticed and nothing about the other three. Every wedge
     * investigated in this project needed exactly this - which core was on
     * which task, and on whose address space - and the answer had to be dug
     * out of QEMU's monitor from the host afterwards, when it could have been
     * printed here for the cost of a loop.
     *
     * Read from the per-CPU blocks rather than stopping the cores: this is a
     * machine that is about to halt, and an IPI round trip is exactly the kind
     * of thing that hangs instead of reporting. A slightly stale line is worth
     * more than no line. */
    uint32_t i;
    if (g_relrace_curcpu >= 0) {
        vibeos_x86_64_serial_puts("[PANIC] release-race: caller_rip=0x");
        vibeos_x86_64_serial_print_hex(g_relrace_rip);
        vibeos_x86_64_serial_puts(" cur_cpu=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)g_relrace_curcpu);
        vibeos_x86_64_serial_puts(" releasing_cpu=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)g_relrace_relcpu);
        vibeos_x86_64_serial_puts(" slot=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)g_relrace_slot);
        vibeos_x86_64_serial_puts("\n");
    }
    {
        for (i = 0; i < VIBEOS_HW_MAX_CPUS; i++) {
            int t;
            if (!g_cpus[i].online) {
                continue;
            }
            t = g_cpus[i].current_task;
            vibeos_x86_64_serial_puts("[PANIC] cpu=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)i);
            vibeos_x86_64_serial_puts(" lapic=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_cpus[i].lapic_id);
            vibeos_x86_64_serial_puts(" task=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)(int64_t)t);
            if (t >= 0 && t < (int)VIBEOS_HW_MAX_TASKS) {
                vibeos_x86_64_serial_puts(" pid=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)hw_task_pid_of(&g_tasks[t]));
                vibeos_x86_64_serial_puts(" state=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)hw_slot_state(t));
                vibeos_x86_64_serial_puts(" cr3=0x");
                vibeos_x86_64_serial_print_hex(g_tasks[t].cr3);
                vibeos_x86_64_serial_puts(" exe=");
                vibeos_x86_64_serial_puts(g_tasks[t].proc.exe_path[0]
                                          ? g_tasks[t].proc.exe_path
                                          : "(none)");
            }
            vibeos_x86_64_serial_puts("\n");
        }
    }
}

/* Everything known about a page that vanished under a running process.
 *
 * Armed on one signature only: a ring-3 *instruction fetch* on a page that is
 * not present. That is narrow enough to cost nothing on a healthy boot - an
 * earlier, broader version of this dump perturbed the timing enough to change
 * the failure rate, which is its own kind of lying - and it is the exact shape
 * of the defect in boot_repeatability.md: cr2 equal to rip, inside the page the
 * program was already executing from.
 *
 * The walk answers which level is missing. The frame questions answer the one
 * the walk cannot: whether the page table the CPU is using has been handed back
 * to the allocator while a task still runs on it. A leaf that is gone is a lost
 * mapping; a PML4 that is *free* is an address space destroyed under its owner,
 * and those are different defects with different fixes. */
void hw_dump_vanished(uint64_t va) {
    static const uint32_t shifts[4] = {39u, 30u, 21u, 12u};
    uint64_t e[4] = {0, 0, 0, 0};
    uint64_t cr3 = hw_read_cr3() & 0x000FFFFFFFFFF000ull;
    uint64_t *tbl;
    uint32_t level;
    int depth = 0;

    tbl = (uint64_t *)(uintptr_t)cr3;
    for (level = 0; level < 4u; level++) {
        uint64_t next;
        e[level] = tbl[(va >> shifts[level]) & 0x1FFu];
        depth = (int)level + 1;
        if ((e[level] & PTE_PRESENT) == 0u || (e[level] & PTE_PS) != 0u) {
            break;
        }
        if (level == 3u) {
            break;
        }
        next = e[level] & 0x000FFFFFFFFFF000ull;
        if (next + 4096ull > VIBEOS_HW_IDENTITY_LIMIT) {
            break;
        }
        tbl = (uint64_t *)(uintptr_t)next;
    }

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[MM] VANISHED va=0x");
    vibeos_x86_64_serial_print_hex(va);
    vibeos_x86_64_serial_puts(" cr3=0x");
    vibeos_x86_64_serial_print_hex(cr3);
    vibeos_x86_64_serial_puts(" stopped_at=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)depth);
    vibeos_x86_64_serial_puts(" pml4e=0x");
    vibeos_x86_64_serial_print_hex(e[0]);
    vibeos_x86_64_serial_puts(" pdpte=0x");
    vibeos_x86_64_serial_print_hex(e[1]);
    vibeos_x86_64_serial_puts(" pde=0x");
    vibeos_x86_64_serial_print_hex(e[2]);
    vibeos_x86_64_serial_puts(" pte=0x");
    vibeos_x86_64_serial_print_hex(e[3]);
    /* The decisive pair: if the frame holding this CR3 is free, or owned by
     * nobody, the address space was destroyed while its task kept running. */
    vibeos_x86_64_serial_puts(" cr3_state=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_frame_state(cr3));
    vibeos_x86_64_serial_puts(" cr3_owners=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_frame_owners(cr3));
    vibeos_x86_64_serial_puts(" slot=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_current_task);
    vibeos_x86_64_serial_puts(" gen=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)(g_current_task >= 0 ?
        vibeos_task_generation((uint32_t)g_current_task) : 0u));
    vibeos_x86_64_serial_puts(" cr3_by=");
    vibeos_x86_64_serial_puts((g_current_task >= 0 &&
                               g_tasks[g_current_task].cr3_set_by) ?
                              g_tasks[g_current_task].cr3_set_by : "-");
    vibeos_x86_64_serial_puts(" aspace_by=");
    vibeos_x86_64_serial_puts((g_current_task >= 0 &&
                               g_tasks[g_current_task].aspace_killed_by) ?
                              g_tasks[g_current_task].aspace_killed_by : "-");
    vibeos_x86_64_serial_puts(" last_destroy=");
    vibeos_x86_64_serial_puts(g_last_destroy_why ? g_last_destroy_why : "-");
    vibeos_x86_64_serial_puts(" of=0x");
    vibeos_x86_64_serial_print_hex(g_last_destroy_pml4);
    vibeos_x86_64_serial_puts(" by_cpu=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)g_last_destroy_cpu);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* Print the most recent crash in full. Reached from the kernel console's
 * `crash` command; safe to call at any time, including when nothing has
 * crashed - saying so plainly is more useful than printing an empty record. */
void vibeos_x86_64_crash_dump(void) {
    vibeos_x86_64_serial_lock();
    vibeos_crash_dump(hw_serial_sink_write, 0);
    vibeos_x86_64_serial_unlock();
}

/* Entry point for the `syscall` trampoline (isr.S): pull the Linux ABI
 * arguments out of the trapframe and store the result back into rax. */
void vibeos_x86_64_syscall_dispatch(vibeos_x86_64_isr_frame_t *frame) {
    /* Timed here rather than inside vibeos_x86_64_linux_syscall, because what
     * the refactor will change is the *dispatch* - the classify/marshal step an
     * ABI translator adds - and measuring only the handler would miss it. */
    uint64_t t0 = hw_tsc();
    long result = vibeos_x86_64_linux_syscall(frame, frame->rax, frame->rdi,
                                              frame->rsi, frame->rdx);
    hw_perf_add(&g_perf_syscall, t0);
    /* rt_sigreturn has already rewritten the whole frame, including rax, to
     * the state the handler interrupted. Overwriting it with a return value
     * would discard exactly what the call exists to restore. */
    if (frame->rip != 0u && (uint64_t)result != frame->rax) {
        frame->rax = (uint64_t)result;
    }
    /* A signal raised while this process was in the kernel is delivered here,
     * on the way out, where its own stack is available and the register state
     * to save is the one sitting in the trapframe. */
    (void)hw_signal_deliver(frame);
}

/* Bring the scheduler up: spawn the initial user tasks, adopt the kernel as a
 * task, and let the timer preempt from here on. */
/* ---- networking ----------------------------------------------------------
 *
 * The protocol stack (kernel/net/inet.c) is portable and hardware-free: this
 * layer gives it a transmit path, feeds it received frames, and drives its
 * timers from the tick. Everything is serialized on one lock - the stack is
 * entered both from syscalls and from the timer interrupt. */

static uint64_t hw_net_now_ms(void) {
    return g_timer_ticks * (1000ull / VIBEOS_HW_TIMER_HZ);
}

/* The interface the stack drives: the first network device the registry found
 * present. arch_hw.c used to name virtio-net's functions directly, so a second
 * network driver meant editing this file (C7). Set once, before g_net_up. */
static const vibeos_net_ops_t *g_netdev;

static int hw_net_tx(void *ctx, const void *frame, uint32_t len) {
    (void)ctx;
    return g_netdev->send(frame, len);
}

/* Drain the receive queue into the stack and advance its timers. Called from
 * the timer IRQ, so the whole system keeps making network progress even while
 * a task is blocked in a socket call. */
/* Staging buffer for one received frame. Static rather than a local: this runs
 * on the interrupt stack, and 1.5 KiB of frame there is enough to overflow it.
 * Covered by the network lock, like everything else that touches the stack. */
static uint8_t g_net_rxframe[VIBEOS_INET_MTU];

static void hw_net_pump(void) {
    int n;
    int budget = 16;

    if (!g_net_up) {
        return;
    }
    hw_spin_lock_named(&g_net_lock, __func__);
    while (budget-- > 0) {
        n = g_netdev->recv(g_net_rxframe, (uint32_t)sizeof(g_net_rxframe));
        if (n <= 0) {
            break;
        }
        (void)vibeos_inet_input(&g_net, g_net_rxframe, (uint32_t)n);
    }
    vibeos_inet_poll(&g_net, hw_net_now_ms());
    hw_spin_unlock(&g_net_lock);
}

static void hw_net_print_ip(uint32_t ip) {
    int i;
    for (i = 3; i >= 0; i--) {
        uint32_t b = (ip >> (i * 8)) & 0xFFu;
        char buf[4];
        int k = 0;
        if (b >= 100u) { buf[k++] = (char)('0' + b / 100u); }
        if (b >= 10u)  { buf[k++] = (char)('0' + (b / 10u) % 10u); }
        buf[k++] = (char)('0' + b % 10u);
        buf[k] = 0;
        vibeos_x86_64_serial_puts(buf);
        if (i > 0) {
            vibeos_x86_64_serial_puts(".");
        }
    }
}

/* Bring the interface up and take a DHCP lease. Runs before the scheduler is
 * armed, so it pumps the device itself while it waits. */
static void hw_net_bringup(void) {
    uint32_t spins;

    /* The driver was brought up by the device registry's probe; this asks
     * for the result rather than naming the driver. */
    g_netdev = vibeos_net_device();
    if (!g_netdev) {
        vibeos_x86_64_serial_puts("[NET] no network interface; networking disabled\n");
        return;
    }
    if (vibeos_inet_init(&g_net, g_netdev->mac(), hw_net_tx, 0) != 0) {
        vibeos_x86_64_serial_puts("[NET] stack init failed\n");
        return;
    }
    /* The secret the stack derives its unguessable identifiers from (H-008).
     *
     * RDRAND when the processor has it. Otherwise the timestamp mix that
     * AT_RANDOM already uses - which that function's own comment calls what it
     * is: it differs between boots and is not an entropy source. The line
     * below says which one this boot got, because a secret whose quality is
     * not stated is one somebody will later assume is good. QEMU's default
     * TCG CPU does not advertise RDRAND, so under the gate this is the weak one. */
    {
        uint32_t eax, ebx, ecx, edx;
        uint64_t k[2] = {0, 0};
        int from_rdrand = 0;

        __asm__ __volatile__("cpuid"
                             : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(1u), "c"(0u));
        if (ecx & (1u << 30)) {
            int i, tries;
            from_rdrand = 1;
            for (i = 0; i < 2; i++) {
                uint64_t v = 0, ok = 0;
                for (tries = 0; tries < 10 && !ok; tries++) {
                    __asm__ __volatile__("xorl %%edx, %%edx; rdrand %%rax; setc %%dl"
                                         : "=a"(v), "=d"(ok));
                }
                if (!ok) {
                    from_rdrand = 0;   /* a failing RDRAND is not a source */
                }
                k[i] = v;
            }
        }
        if (!from_rdrand) {
            uint8_t seed[16];
            int i;
            hw_seed_at_random(seed);
            k[0] = 0;
            k[1] = 0;
            for (i = 0; i < 8; i++) {
                k[0] |= (uint64_t)seed[i] << (8 * i);
                k[1] |= (uint64_t)seed[8 + i] << (8 * i);
            }
        }
        vibeos_inet_set_secret(&g_net, k[0], k[1]);
        vibeos_x86_64_serial_puts(from_rdrand
            ? "[NET] stack secret from rdrand\n"
            : "[NET] stack secret from tsc mix (weak: no entropy source on this cpu)\n");
    }
    g_net_up = 1;

    vibeos_x86_64_serial_puts("[NET] requesting a DHCP lease\n");
    /* Under the lock: this transmits, and the timer's pump is already live. */
    hw_spin_lock_named(&g_net_lock, __func__);
    (void)vibeos_inet_dhcp_start(&g_net);
    hw_spin_unlock(&g_net_lock);

    /* The timer is already live but the scheduler is not, so pump inline.
     * Bounded: a network that does not answer must not hold up the boot. */
    for (spins = 0; spins < 400u; spins++) {
        /* Go through the same locked path the timer uses: the interrupt is
         * already live and would otherwise reuse the staging buffer under us. */
        hw_net_pump();
        {
            int bound_now;
            hw_spin_lock_named(&g_net_lock, __func__);
            bound_now = vibeos_inet_dhcp_bound(&g_net);
            hw_spin_unlock(&g_net_lock);
            if (bound_now) {
                break;
            }
        }
        {
            uint32_t d;
            for (d = 0; d < 200000u; d++) {
                __asm__ __volatile__("pause" ::: "memory");
            }
        }
    }

    /* Snapshot under the lock: the timer is pumping the stack concurrently, and
     * a DHCP retry blanks the address for the duration of the send. Reading the
     * live fields here could catch that window and report 0.0.0.0. */
    {
        int bound;
        uint32_t ip, gw, dns;

        hw_spin_lock_named(&g_net_lock, __func__);
        bound = vibeos_inet_dhcp_bound(&g_net);
        ip = g_net.ip;
        gw = g_net.gateway;
        dns = g_net.dns;
        hw_spin_unlock(&g_net_lock);

        vibeos_x86_64_serial_lock();
        if (bound) {
            vibeos_x86_64_serial_puts("[NET] NET_OK dhcp lease ip=");
            hw_net_print_ip(ip);
            vibeos_x86_64_serial_puts(" gw=");
            hw_net_print_ip(gw);
            vibeos_x86_64_serial_puts(" dns=");
            hw_net_print_ip(dns);
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
            return;
        }
        vibeos_x86_64_serial_unlock();
    }
    /* No DHCP server answered: fall back to QEMU's user-mode defaults so the
     * stack is still usable, and say so plainly. */
    hw_spin_lock_named(&g_net_lock, __func__);
    vibeos_inet_set_addr(&g_net, 0x0A000210u, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    hw_spin_unlock(&g_net_lock);

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[NET] no DHCP answer; using a static address ip=");
    hw_net_print_ip(0x0A000210u);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

/* ---- APIC + SMP bring-up -------------------------------------------------- */

/* Index of the CPU currently being started; read by that CPU's entry point. */
static volatile uint32_t g_ap_starting;

/* Entry point of an application processor, reached from the real-mode
 * trampoline once it is in long mode on the kernel's page tables. Called with
 * interrupts disabled on a temporary boot stack. Never returns: the core takes
 * up its idle task and from then on is scheduled like any other. */
void vibeos_x86_64_ap_main(void) {
    uint32_t idx = g_ap_starting;
    hw_cpu_t *cpu = &g_cpus[idx];
    int idle;

    hw_load_gdt(idx);
    hw_load_idt_only();
    hw_enable_syscall();
    vibeos_x86_64_lapic_enable(0xFFu);
    cpu->lapic_id = vibeos_x86_64_lapic_id();
    cpu->online = 1;

    idle = hw_task_create_idle(cpu);
    if (idle < 0) {
        /* No slot for this core's idle task: park it rather than let it run
         * with no context to fall back to. Report it - a silent park here is
         * indistinguishable from a core that never started. */
        vibeos_x86_64_serial_puts("[SMP] no idle-task slot; parking cpu\n");
        cpu->online = 0;
        vibeos_x86_64_ap_alive = 1;
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }
    cpu->current_task = idle;
    (void)hw_task_set_state(idle, HW_TASK_RUNNING, __func__);
    g_tasks[idle].on_cpu = 1;       /* this core is about to enter it */
    hw_set_kernel_stack(g_tasks[idle].kstack_top);

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[SMP] cpu online: console_id=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_x86_64_cpu_id());
    vibeos_x86_64_serial_puts(" lapic_id=0x");
    vibeos_x86_64_serial_print_hex(cpu->lapic_id);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    /* Tell the BSP we made it, then start this core's own preemption clock and
     * fall into the idle task; the timer will hand us real work. */
    __asm__ __volatile__("sfence" ::: "memory");
    vibeos_x86_64_ap_alive = 1;
    vibeos_x86_64_lapic_timer_start(VIBEOS_HW_TIMER_HZ, VIBEOS_HW_IRQ_TIMER);
    hw_ctx_check(idle, "ap_idle");
    vibeos_x86_64_task_enter(&g_tasks[idle].ctx); /* does not return */
}

/* Switch the machine from the legacy 8259/PIT pair to the APIC pair: discover
 * the topology through ACPI, enable the BSP's local APIC, move the keyboard IRQ
 * to the IO-APIC, and run preemption off the local-APIC timer. Falls back to
 * the PIC silently if the firmware gives us no usable MADT. */
static void hw_apic_bringup(const vibeos_boot_info_t *boot_info) {
    uint32_t bsp_id;

    if (!boot_info || vibeos_x86_64_acpi_init(boot_info->acpi_rsdp) != 0) {
        vibeos_x86_64_serial_puts("[APIC] no ACPI topology; staying on the 8259 PIC\n");
        return;
    }
    if (!vibeos_x86_64_apic_available()) {
        vibeos_x86_64_serial_puts("[APIC] MADT lists no IO-APIC; staying on the 8259 PIC\n");
        return;
    }

    __asm__ __volatile__("cli");
    vibeos_x86_64_lapic_enable(0xFFu);
    bsp_id = vibeos_x86_64_lapic_id();
    g_cpus[0].lapic_id = bsp_id;
    g_cpus[0].online = 1;

    vibeos_x86_64_pic_disable();   /* no double delivery from the 8259s */
    /* Every legacy line a registered device declared, to vector 32 + line -
     * before anything is probed, because a probe that talks to its device
     * raises the line (the mouse's ACKs do) and an unrouted interrupt is lost. */
    {
        int lines[16];
        uint32_t i, n = vibeos_device_isa_lines(lines, 16u);
        for (i = 0; i < n; i++) {
            if (vibeos_x86_64_ioapic_route((uint8_t)lines[i],
                                           (uint8_t)(32 + lines[i]), bsp_id) != 0) {
                vibeos_x86_64_serial_lock();
                vibeos_x86_64_serial_puts("[APIC] failed to route legacy IRQ 0x");
                vibeos_x86_64_serial_print_hex((uint64_t)lines[i]);
                vibeos_x86_64_serial_puts("\n");
                vibeos_x86_64_serial_unlock();
            }
        }
    }
    vibeos_x86_64_lapic_timer_start(VIBEOS_HW_TIMER_HZ, VIBEOS_HW_IRQ_TIMER);
    g_apic_mode = 1;
    __asm__ __volatile__("sti");

    vibeos_x86_64_serial_puts("[APIC] APIC_OK: bsp lapic_id=0x");
    vibeos_x86_64_serial_print_hex(bsp_id);
    vibeos_x86_64_serial_puts(" timer=LAPIC keyboard=IOAPIC\n");
}

/* Wake every other CPU the MADT listed. Done after the scheduler is live so an
 * AP has a run queue to pull from the moment its timer fires. */
static void hw_smp_bringup(void) {
    uint32_t count = vibeos_x86_64_acpi_cpu_count();
    uint32_t bsp_id = g_cpus[0].lapic_id;
    uint32_t i;

    if (!g_apic_mode || count <= 1u) {
        vibeos_x86_64_serial_puts("[SMP] single processor (cpus=0x1)\n");
        return;
    }
    if (count > VIBEOS_HW_MAX_CPUS) {
        count = VIBEOS_HW_MAX_CPUS;
    }

    for (i = 0; i < count; i++) {
        uint32_t id = vibeos_x86_64_acpi_lapic_id(i);
        uint32_t slot = g_cpu_online_count;
        if (id == bsp_id || slot >= VIBEOS_HW_MAX_CPUS) {
            continue;
        }
        g_ap_starting = slot;
        __asm__ __volatile__("sfence" ::: "memory");
        if (vibeos_x86_64_smp_start_cpu(id, (uint64_t)(uintptr_t)&g_pml4[0],
                                        (uint64_t)(uintptr_t)&g_ap_boot_stack[slot][sizeof(g_ap_boot_stack[0])],
                                        (uint64_t)(uintptr_t)vibeos_x86_64_ap_main) == 0 &&
            g_cpus[slot].online) {
            g_cpu_online_count++;
        } else {
            vibeos_x86_64_serial_puts("[SMP] cpu did not come up: lapic_id=0x");
            vibeos_x86_64_serial_print_hex(id);
            vibeos_x86_64_serial_puts("\n");
        }
    }

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[SMP] SMP_OK: cpus online=0x");
    vibeos_x86_64_serial_print_hex(g_cpu_online_count);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

static void hw_runtime_copy_string(char *dst, uint32_t capacity, const char *src) {
    uint32_t i;
    if (!dst || !src || capacity == 0) {
        return;
    }
    for (i = 0; i + 1u < capacity && src[i] != 0; i++) {
        dst[i] = src[i];
    }
    dst[i] = 0;
}

static void hw_runtime_supervisor_init(void) {
    vibeos_service_manifest_t manifests[3] = {0};
    uint32_t i;
    if (vibeos_service_supervisor_init(&g_runtime_supervisor) != 0) {
        return;
    }
    for (i = 0; i < 3; i++) {
        manifests[i].abi_major = VIBEOS_NATIVE_ABI_MAJOR;
        manifests[i].struct_size = sizeof(manifests[i]);
        manifests[i].service_id = i + 1u;
        manifests[i].restart_policy = VIBEOS_NATIVE_RESTART_ON_FAILURE;
        manifests[i].restart_limit = 3u;
        manifests[i].startup_timeout_ms = 5000u;
        manifests[i].health_timeout_ms = 1000u;
    }
    manifests[0].dependency_mask = 0;
    manifests[1].dependency_mask = 1u;
    manifests[2].dependency_mask = 1u;
    hw_runtime_copy_string(manifests[0].name, sizeof(manifests[0].name), "init");
    hw_runtime_copy_string(manifests[1].name, sizeof(manifests[1].name), "shell");
    hw_runtime_copy_string(manifests[2].name, sizeof(manifests[2].name), "logd");
    hw_runtime_copy_string(manifests[0].image_path, sizeof(manifests[0].image_path), "/sbin/init");
    hw_runtime_copy_string(manifests[1].image_path, sizeof(manifests[1].image_path), "/bin/sh");
    hw_runtime_copy_string(manifests[2].image_path, sizeof(manifests[2].image_path), "/sbin/logd");
    if (vibeos_service_supervisor_load(&g_runtime_supervisor, manifests, 3) == 0 &&
        vibeos_service_supervisor_start_ready(&g_runtime_supervisor) == 0) {
        g_runtime_supervisor_ready = 1;
        vibeos_x86_64_serial_puts("[INIT] native supervisor manifest ready\n");
    }
}

/* Held between hardware bring-up and the moment userland is started, which
 * are now two separate steps with the portable kernel in between. */
static const vibeos_boot_info_t *g_saved_boot_info;

static void hw_sched_bringup(const vibeos_boot_info_t *boot_info) {
    const unsigned char *init_elf = vibeos_user_hello_elf;
    uint64_t init_len = vibeos_user_hello_elf_len;
    int hello_id, a_id, b_id, kern_id;
    /* Argument vectors for the first processes. The two scheduler-demo tasks
     * differ only by argv[0], which is how they pick the letter they print. */
    static const char *const init_argv[] = {"init", 0};
    static const char *const task_a_argv[] = {"0", 0};
    static const char *const task_b_argv[] = {"1", 0};

    /* Init program source, most real first: the on-disk filesystem (virtio-blk +
     * FAT), then the bootloader's EFI module, then the built-in copy. */
    if (g_disk_init_len > 0) {
        init_elf = g_disk_init_elf;
        init_len = (uint64_t)g_disk_init_len;
        vibeos_x86_64_serial_puts("[SCHED] init program from on-disk filesystem (INIT.ELF)\n");
    } else if (boot_info && boot_info->initrd_base != 0 && boot_info->initrd_size > 0 &&
               boot_info->initrd_base + boot_info->initrd_size <= 0x100000000ull) {
        init_elf = (const unsigned char *)(uintptr_t)boot_info->initrd_base;
        init_len = boot_info->initrd_size;
        vibeos_x86_64_serial_puts("[SCHED] init program from bootloader EFI module\n");
    } else {
        vibeos_x86_64_serial_puts("[SCHED] init program from built-in image\n");
    }

    /* Bring up the Linux personality so the portable translation model sees
     * every syscall the on-metal front end serves. */

    /* Keyboard is live (IRQ1 unmasked). Seed a test line so the blocking read()
     * path is exercised on the non-interactive CI console; real keystrokes fill
     * the same ring on hardware. */
    vibeos_x86_64_serial_puts("[KBD] keyboard armed (IRQ1); seeding read() self-test input\n");
    (void)vibeos_input_inject("vibeos\n"
                                  "mkdir DOCS\n"
                                  "write DOCS/NOTES.TXT persistent hello\n"
                                  "cat DOCS/NOTES.TXT\n"
                                  "ls DOCS\n"
                                  "write TMP.TXT scratch\b\b\bch\n"  /* backspace editing */
                                  "rm TMP.TXT\n"
                                  "EFI/BOOT/TASK.ELF\n"
                                  "net\n"
                                  "ping 10.0.2.2\n"
                                  "EFI/BOOT/NET.ELF\n"
                                  "EFI/BOOT/MUSL.ELF\n"
                                  "EFI/BOOT/PIE.ELF\n"
                                  "EFI/BOOT/DYN.ELF\n"
                                  "EFI/BOOT/THREADS.ELF\n"
                                  "EFI/BOOT/TFORK.ELF\n"
                                  "EFI/BOOT/SIGNAL.ELF\n"
                                  /* The reclaim load, alone: the commands here
                                   * run one at a time. It holds the machine at
                                   * its low watermark, where every other
                                   * program's allocations may be refused -
                                   * correctly - so started from init beside the
                                   * thread tests it failed them one boot in
                                   * two: pthread_create, a thread's mmap, an
                                   * exec from a thread, all refused. A load
                                   * that starves its neighbours tests them
                                   * rather than reclaim. Not last, either: it
                                   * empties the page cache, and the BusyBox
                                   * commands after it are what give the exec
                                   * cache audit something to compare. */
                                  "EFI/BOOT/SVC_RECL.ELF\n"
                                  "EFI/BOOT/BUSYBOX.ELF echo BUSYBOX_ECHO_OK\n"
                                  "EFI/BOOT/BUSYBOX.ELF cat DOCS/NOTES.TXT\n"
                                  "EFI/BOOT/BUSYBOX.ELF ls EFI/BOOT\n"
                                  "EFI/BOOT/BUSYBOX.ELF sh -c \"echo BUSYBOX_SH_OK; cat DOCS/NOTES.TXT\"\n"
                                  /* Everything from here is typed at BusyBox's
                                   * shell, not ours: it replaces this process
                                   * and reads the rest of the console itself. */
                                  "sh\n"
                                  "echo ASH_INTERACTIVE_OK\n"
                                  "cat DOCS/NOTES.TXT\n"
                                  "ls /EFI/BOOT\n"
                                  /* The last thing the self-test says. The boot
                                   * harness waits for this before driving the
                                   * kernel CLI, so a slower build cannot have
                                   * its script cut short by a halt that arrived
                                   * while it was still working. */
                                  "ls /EFI/BOOT | wc -l\n"
                                  "echo PIPE_OK\n"
                                  "echo VIBEOS_SELFTEST_DONE\n"
                                  "exit\n");

    hello_id = hw_task_spawn_user(init_elf, init_len, init_argv);
    a_id = hw_task_spawn_user(vibeos_user_task_elf, vibeos_user_task_elf_len,
                              task_a_argv);
    b_id = hw_task_spawn_user(vibeos_user_task_elf, vibeos_user_task_elf_len,
                              task_b_argv);
    if (hello_id < 0 || a_id < 0 || b_id < 0) {
        vibeos_x86_64_serial_puts("[SCHED] failed to spawn initial tasks\n");
        return;
    }
    hw_runtime_supervisor_init();
    if (g_runtime_supervisor_ready) {
        hw_task_set_service(hello_id, 1u);
        (void)vibeos_service_supervisor_bind_pid(&g_runtime_supervisor, 1u,
                                                 hw_task_pid_of(&g_tasks[hello_id]));
    }
    g_console_foreground_pgid = hw_task_pgid_of(&g_tasks[hello_id]);

    /* Printed before the scheduler is armed, so this line cannot be split by a
     * preemption. */
    vibeos_x86_64_serial_puts("[SCHED] scheduler live: kernel task + 3 user tasks, own address spaces\n");

    __asm__ __volatile__("cli");
    kern_id = hw_task_adopt_kernel();
    if (kern_id < 0) {
        __asm__ __volatile__("sti");
        vibeos_x86_64_serial_puts("[SCHED] failed to adopt kernel task\n");
        return;
    }
    (void)hw_task_create_idle(&g_cpus[0]);
    /* Before the first switch: a scheduler that starts accounting after it
     * starts scheduling reports a machine that idled through its own boot. */
    /* Sized for every core this kernel can have, not the ones online right
     * now: the application processors come up after this point, and sizing it
     * to the current count silently refused three quarters of the ticks while
     * balancing perfectly. */
    (void)vibeos_account_init((uint32_t)VIBEOS_HW_MAX_TASKS, VIBEOS_HW_MAX_CPUS);
    /* Four slots held back, and eight children to any one task.
     *
     * Four is one per core, which is the smallest reserve that lets every core
     * still start something. Eight is comfortably more than anything in this
     * boot forks - the shell's deepest pipeline is three - so the limit binds
     * on a bomb and on nothing else. Both are numbers chosen rather than
     * discovered, and if a workload ever needs more the place to argue about it
     * is here. */
    (void)vibeos_forkguard_init((uint32_t)VIBEOS_HW_MAX_TASKS, 4u, 8u);

    /* The user-access recovery, exercised on every boot (H-003, H-010).
     *
     * A copy from a user address that nothing maps must come back as an error,
     * not stop the machine - that is the whole contract, and the last attempt
     * at it measured green for three boots while never once being exercised,
     * because its only caller always had the page. So it is forced here: the
     * top of the high user window, from the kernel's own address space, before
     * any task runs. */
    {
        uint8_t probe[8];
        int r = vibeos_uaccess_copy(probe,
                                    (const void *)(uintptr_t)(VIBEOS_HW_USER_BASE + 0x7F00000000ull),
                                    sizeof(probe));
        vibeos_x86_64_serial_puts(r != 0
            ? "[HW] uaccess recovery ok: an unmapped user read returned an error\n"
            : "[HW] uaccess recovery WRONG: an unmapped user read succeeded\n");
    }
    g_sched_running = 1;
    __asm__ __volatile__("sti");

    /* With a run queue in place, wake the other cores. */
    hw_smp_bringup();

    /* Wait for the spawned tasks to finish before the kernel task goes on to
     * the console (init-style child reaping). The kernel task is preempted
     * while it waits, so the user tasks make progress; hlt idles until the next
     * timer tick instead of spinning.
     *
     * BLOCKED counts as alive, and leaving it out was a defect with a long
     * reach. This asked only for READY or RUNNING, so a moment when every user
     * task happened to be waiting - in waitpid after a fork, on a pipe, on a
     * futex - read as "nothing is running, we are done". The machine then
     * printed "all user tasks retired" with tasks very much not retired, went
     * on to the console, and kmain sampled the frame accounting whose comment
     * states outright that every user process has exited by this point.
     *
     * It hid because the ordinary boot's programs are short: the window has to
     * open while something is still blocked, and at 120 stress rounds the run
     * is usually over first. Raising that to 12000 for the plan's soak made it
     * fire on nearly every boot - the log shows fork after fork retiring after
     * the CLI is already up - and the frame numbers reported from mid-flight
     * looked like a leak that grew with the workload. It was not a leak; it
     * was a measurement taken while the thing being measured was still running.
     *
     * ZOMBIE is deliberately not alive: it holds a slot, not an address space,
     * and something has to reap it - which is what this loop's caller goes on
     * to do. */
    for (;;) {
        int i;
        int alive = 0;
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (hw_task_is_user_of(&g_tasks[i]) &&
                (hw_slot_state(i) == HW_TASK_READY ||
                 hw_slot_state(i) == HW_TASK_RUNNING ||
                 hw_slot_state(i) == HW_TASK_BLOCKED)) {
                alive = 1;
            }
        }
        if (!alive) {
            break;
        }
        /* The memory clobber forces the task states to be re-read after each
         * idle period: they are updated by interrupt/syscall context. */
        __asm__ __volatile__("hlt" ::: "memory");
    }
    vibeos_x86_64_serial_puts("[SCHED] all user tasks retired; kernel task continues\n");
    /* Every user slot that is not free, and its state, in one line (M-068). A
     * boot announced this while svc-reclaim was still running; the wait counts
     * READY, RUNNING and BLOCKED as alive, so whatever that task was, it was
     * something else for the instant the wait looked. */
    {
        int i;

        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[SCHED] RETIRED_SLOTS");
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (hw_task_is_user_of(&g_tasks[i]) && hw_slot_state(i) != HW_TASK_FREE) {
                vibeos_x86_64_serial_puts(" slot=0x");
                vibeos_x86_64_serial_print_hex((uint64_t)i);
                vibeos_x86_64_serial_puts(":0x");
                vibeos_x86_64_serial_print_hex((uint64_t)hw_slot_state(i));
            }
        }
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }

    {
        vibeos_x86_64_serial_puts("[MM] COW_STATS exclusive_lost=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mm_stats()->cow_exclusive_lost);
        vibeos_x86_64_serial_puts(" shared=0x");
        vibeos_x86_64_serial_print_hex(g_cow_shared);
        vibeos_x86_64_serial_puts(" copied=0x");
        vibeos_x86_64_serial_print_hex(g_cow_copied);
        vibeos_x86_64_serial_puts(" tlb_shootdowns=0x");
        vibeos_x86_64_serial_print_hex(g_tlb_shootdowns);
        vibeos_x86_64_serial_puts(" tlb_acks=0x");
        vibeos_x86_64_serial_print_hex(g_tlb_acks);
        vibeos_x86_64_serial_puts(" tlb_targets=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mm_stats()->tlb_targets);
        vibeos_x86_64_serial_puts(" tlb_flushed=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mm_stats()->tlb_flushed);
        vibeos_x86_64_serial_puts(" bad_unlocks=0x");
        vibeos_x86_64_serial_print_hex(vibeos_x86_64_serial_bad_unlocks());
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_puts("[ABI] abi=");
        vibeos_x86_64_serial_puts(vibeos_abi_linux()->name);
        vibeos_x86_64_serial_puts(" vocabulary=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)VIBEOS_OP_COUNT - 1u);
        /* The hot paths. One critical section for the whole line: it is built
         * from ten calls that each take the console lock on their own, and a
         * line assembled from ten of those is ten critical sections. */
        /* Does anything else own a frame the desktop is rendering into?
         *
         * This is the hypothesis the case file actually states - that the
         * frames are handed out twice - and it is the one neither the canary
         * nor the poison can see, because a double hand-out frees nothing and
         * corrupts no bookkeeping. It is asked directly: for every frame in the
         * back buffer, how many page-table entries point at it? The desktop
         * does not map its buffer into any address space, so the honest answer
         * is zero for all of them, and anything else names a process sharing
         * memory with the screen.
         *
         * Outside the frame layer's lock, deliberately: vibeos_frame_owners
         * takes it, and a diagnostic that calls a public accessor from inside
         * the lock it needs is how a previous investigation deadlocked the
         * machine it was explaining, mid-line, at "owners=0x". */
        if (g_gui_back_base != 0ull) {
            uint64_t p;
            for (p = g_gui_back_base; p < g_gui_back_end; p += 4096ull) {
                /* Greater than one, not non-zero.
                 *
                 * The first version of this asked for owners != 0 and reported
                 * 1000 of 1000 frames shared on every boot, which is the same
                 * shape as the 3019 use-after-frees that cost a phase: a
                 * detector that fires on everything is reporting its own
                 * baseline. frame_take sets owners to 1 when it hands a frame
                 * out, so one owner *is* the allocated state. A second owner is
                 * a page-table entry somebody else installed. */
                if (vibeos_frame_owners(p) > 1u) {
                    g_gui_back_shared++;
                }
                /* And the other half of the same question. Owners counts page
                 * tables pointing at the frame, which is the process case; a
                 * frame the layer believes is FREE is the kernel case, and it
                 * is the one that lets the *next* allocation - a block buffer,
                 * an argv page - be handed the screen. Neither the poison nor
                 * the canary can see it: nothing was freed early and nothing
                 * overran, the layer simply lost track. */
                if (vibeos_frame_state(p) != VIBEOS_FRAME_ALLOCATED) {
                    g_gui_back_lost++;
                }
            }
        }
        /* guard_broken used to lead this line; it is the GUI's own report now.
         * Which left backbuf_shared first after the word, printed as a
         * must-be-zero for a phase and asserted by nobody. */
        vibeos_x86_64_serial_puts("\n[GUI] MUSTBEZERO backbuf_shared=0x");
        vibeos_x86_64_serial_print_hex(g_gui_back_shared);
        vibeos_x86_64_serial_puts(" MUSTBEZERO backbuf_lost=0x");
        vibeos_x86_64_serial_print_hex(g_gui_back_lost);
        vibeos_x86_64_serial_puts(" ring3_write_nul=0x");
        vibeos_x86_64_serial_print_hex(g_ring3_write_nul);
        vibeos_x86_64_serial_puts(" cow_copy_changed=0x");
        vibeos_x86_64_serial_print_hex(g_cow_copy_changed);
        vibeos_x86_64_serial_puts(" cow_resolved=0x");
        vibeos_x86_64_serial_print_hex(g_cow_resolved);
        vibeos_x86_64_serial_puts("\n[NET] MUSTBEZERO sock_stale_parent=0x");
        vibeos_x86_64_serial_print_hex(g_net.sock_stale_parent);
        vibeos_x86_64_serial_puts(" sock_fd_aba=0x");
        vibeos_x86_64_serial_print_hex(g_net.sock_fd_aba);
        /* The ABI surface, which had no counter. (The mouse's line, which sat
         * here, is the driver's own report now - see kernel/io/device.c.) */
        vibeos_x86_64_serial_puts("\n[ABI] MUSTBEZERO unexpected_unimplemented=0x");
        vibeos_x86_64_serial_print_hex(g_abi_unimplemented - g_abi_probes);
        vibeos_x86_64_serial_puts(" probes=0x");
        vibeos_x86_64_serial_print_hex(g_abi_probes);
        vibeos_x86_64_serial_puts(" last_nr=0x");
        vibeos_x86_64_serial_print_hex(g_abi_last_nr);
        /* The device registry (C7): how many drivers the linker collected, and
         * whether an input interrupt ever woke a reader. Both asserted: an empty
         * table used to show up only as a wedge, and the wake as nothing. */
        vibeos_x86_64_serial_puts("\n[DEV] registered=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)vibeos_device_count());
        vibeos_x86_64_serial_puts(" input_irq_wakes=0x");
        vibeos_x86_64_serial_print_hex(g_input_irq_wakes);
        vibeos_x86_64_serial_puts(" MUSTBEZERO stray_vectors=0x");
        vibeos_x86_64_serial_print_hex(g_device_stray_irqs);
        /* The registry the parsers, the journal, the log sink and the scheduler
         * report through (kernel/core/mbz.c): total, and which one and what it
         * saw first. One line so the witness cannot be separated from the count. */
        vibeos_x86_64_serial_puts("\n[MBZ] MUSTBEZERO total=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mbz_total());
        vibeos_x86_64_serial_puts(" first=");
        vibeos_x86_64_serial_puts(vibeos_mbz_first() == VIBEOS_MBZ_COUNT
                                      ? "none" : vibeos_mbz_name(vibeos_mbz_first()));
        vibeos_x86_64_serial_puts(" witness=0x");
        vibeos_x86_64_serial_print_hex(vibeos_mbz_first() == VIBEOS_MBZ_COUNT
                                           ? 0u : vibeos_mbz_witness(vibeos_mbz_first()));
        /* The log's sinks: how many lines each was given, how many it refused
         * (klog_line_lost in the registry above) and how many were raised from
         * inside its own write. The gate reads serial_lines and checks every
         * ln= from one to it is in the log - which is how a device that dropped
         * a line and said it had not is seen from outside it. */
        vibeos_x86_64_serial_puts("\n[KLOG] sinks=0x");
        vibeos_x86_64_serial_print_hex(vibeos_klog_sink_count());
        {
            uint32_t k;
            for (k = 0; k < vibeos_klog_sink_count(); k++) {
                vibeos_klog_sink_stats_t ks;
                if (vibeos_klog_sink_stats(k, &ks) != 0) {
                    continue;
                }
                vibeos_x86_64_serial_puts(" ");
                vibeos_x86_64_serial_puts(ks.name);
                vibeos_x86_64_serial_puts("_lines=0x");
                vibeos_x86_64_serial_print_hex(ks.offered);
                vibeos_x86_64_serial_puts(" ");
                vibeos_x86_64_serial_puts(ks.name);
                vibeos_x86_64_serial_puts("_lost=0x");
                vibeos_x86_64_serial_print_hex(ks.lost);
                vibeos_x86_64_serial_puts(" ");
                vibeos_x86_64_serial_puts(ks.name);
                vibeos_x86_64_serial_puts("_reentered=0x");
                vibeos_x86_64_serial_print_hex(ks.reentered);
            }
        }
        vibeos_x86_64_serial_puts("\n[PERF] syscalls=0x");
        vibeos_x86_64_serial_print_hex(g_perf_syscall.count);
        vibeos_x86_64_serial_puts(" syscall_cycles=0x");
        vibeos_x86_64_serial_print_hex(g_perf_syscall.cycles);
        vibeos_x86_64_serial_puts(" syscall_min=0x");
        vibeos_x86_64_serial_print_hex(g_perf_syscall.min);
        vibeos_x86_64_serial_puts(" faults=0x");
        vibeos_x86_64_serial_print_hex(g_perf_fault.count);
        vibeos_x86_64_serial_puts(" fault_cycles=0x");
        vibeos_x86_64_serial_print_hex(g_perf_fault.cycles);
        vibeos_x86_64_serial_puts(" fault_min=0x");
        vibeos_x86_64_serial_print_hex(g_perf_fault.min);
        vibeos_x86_64_serial_puts(" switches=0x");
        vibeos_x86_64_serial_print_hex(g_perf_switch.count);
        vibeos_x86_64_serial_puts("\n");
    }
}

/* What the portable inspection layer (kernel/mm/usage.c) asks the architecture
 * for. Weak stubs there return zero; these are the real answers, and they are
 * everything today's allocator can honestly report. The rest of the picture -
 * the state histogram, the per-process split - needs the frame table and the
 * address-space layer, and reports zero until those exist rather than being
 * guessed at. */
uint64_t vibeos_mm_bytes_total(void) {
    return vibeos_frame_total() * 4096ull;
}

uint64_t vibeos_mm_bytes_free(void) {
    /* One number, kept by the layer that hands frames out, instead of a bump
     * remainder plus a walk of a free list. The walk was honest and it was also
     * the reason "how much memory is free" could disagree with "how many frames
     * are free" - two answers to one question, which is how this subsystem got
     * its reputation. */
    return vibeos_frame_free_count() * 4096ull;
}

uint64_t vibeos_mm_bytes_reserved(void) {
    /* Two different reservations, and both are real memory a person cannot use.
     *
     * The low user window is taken out before the allocator starts, so nothing
     * of the kernel's lives where a Linux process shadows it. The rest is what
     * the bootstrap bump allocator had already handed out when the frame layer
     * took over - early page tables, the staging buffers, the descriptor table
     * itself. That second part used to be invisible: it was simply missing from
     * every total, which is exactly the kind of gap this command exists to
     * close. */
    uint64_t prefix = 0;

    if (g_frame_layer_ready) {
        prefix = (uint64_t)g_hw_pmm.offset_bytes;
    }
    return (VIBEOS_HW_LOW_USER_LIMIT - VIBEOS_HW_LOW_USER_BASE) + prefix;
}

/* Entry point invoked from entry.s before vibeos_kmain. */
/* The storage and I/O bring-ups used to be here - about twelve hundred
 * lines of them, all added in the last few days while this file was
 * supposed to be shrinking. They are in io_bringup.c now.
 *
 * The seam turned out to be eight names in and seven out, which is a
 * remarkably clean cut for a monolith; it is clean because the code is
 * new, and new code has not yet grown the incidental couplings that make
 * the older sections hard to separate. That is the argument for cutting
 * recent additions early rather than letting them settle.
 */

/* Boot stages, in one format, on the serial line and in the kernel log.
 *
 * Every stage of the boot says its own name as it completes. This is not
 * decoration: the boundary between "the kernel is up" and "userland is
 * running" was nowhere in the output, and its absence cost a full session -
 * every userland hang was reported against the last bootloader marker anyone
 * had seen, so a firmware bug was hunted that did not exist.
 *
 * The names are asserted, in order, by the boot gate. A stage that disappears
 * or moves fails the boot rather than quietly changing what the log means. */
static void hw_boot_stage(const char *name) {
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[BOOT] STAGE ");
    vibeos_x86_64_serial_puts(name);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
    /* The ring only exists from the "log" stage onwards; before that the
     * serial line is the only record there is, which is why both are used. */
    if (vibeos_klog_ready()) {
        hw_log(VIBEOS_LOG_INFO, 200u, 0, 0, name);
    }
}

/* ---- devices (C7) ----------------------------------------------------------
 *
 * The drivers' descriptors, collected by kernel.ld from every object that used
 * VIBEOS_DEVICE(). Nothing here names a driver: a new one is a file and its
 * line in the build. */
extern const vibeos_device_t *const __start_vibeos_devices[];
extern const vibeos_device_t *const __stop_vibeos_devices[];

static hw_lock_t g_device_lock;

static void hw_device_lock(void) {
    hw_spin_lock_named(&g_device_lock, "vibeos_device");
}

static void hw_device_unlock(void) {
    hw_spin_unlock(&g_device_lock);
}

/* The filesystem drivers, the same way (VIBEOS_FS_DRIVER). Registered once, at
 * boot, before any volume is mounted or scanned - single-threaded, so the
 * storage table needs no lock for as long as this is its only writer. */
extern const vibeos_fs_driver_t *const __start_vibeos_fs_drivers[];
extern const vibeos_fs_driver_t *const __stop_vibeos_fs_drivers[];

static void hw_fs_drivers(void) {
    const vibeos_fs_driver_t *const *d;

    for (d = __start_vibeos_fs_drivers; d < __stop_vibeos_fs_drivers; d++) {
        if (vibeos_storage_register(*d) != 0) {
            hw_panic("filesystem driver refused: incomplete, or too many");
        }
    }
}

/* Locks for drivers (C7): the storage is the driver's, the operations are
 * these, registered once for all of them. hw spinlocks mask interrupts, which
 * a driver's lock has to - a display's repaint runs from the timer - and they
 * name their holder when a wait is too long, which is why the lock carries a
 * name. Before this, each driver that wanted a lock had it
 * registered here by name, which put the driver back in this file. */
_Static_assert(sizeof(hw_lock_t) <= sizeof(((vibeos_dev_lock_t *)0)->opaque),
               "a driver's lock storage must hold an hw_lock_t");

static int hw_dev_lock(vibeos_dev_lock_t *l) {
    hw_lock_t *h = (hw_lock_t *)(void *)l->opaque;

    /* The one wait that cannot end: this CPU holds it already. owner_cpu is
     * written only by the holder, after taking the lock, and cleared before it
     * lets go, so only the holder can find its own number there - except in a
     * lock nobody has taken yet, which is zeroed storage and so says "cpu 0".
     * owner_fn is written after owner_cpu and cleared before it, and starts
     * null, so a non-null one means the owner_cpu beside it is a holder's. */
    if (h->locked && h->owner_fn && h->owner_cpu == (int)vibeos_x86_64_cpu_id()) {
        return -1;
    }
    hw_spin_lock_named(h, l->name ? l->name : "driver");
    return 0;
}

static void hw_dev_unlock(vibeos_dev_lock_t *l) {
    hw_spin_unlock((hw_lock_t *)(void *)l->opaque);
}

static void hw_device_table(void) {
    uint32_t n = (uint32_t)(__stop_vibeos_devices - __start_vibeos_devices);

    vibeos_device_set_lock(hw_device_lock, hw_device_unlock);
    vibeos_device_set_lock_ops(hw_dev_lock, hw_dev_unlock);
    if (vibeos_device_set_table(__start_vibeos_devices, n) != 0) {
        hw_panic("device table refused: too many drivers, or an empty entry");
    }
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[DEV] registered=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)n);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}

void vibeos_x86_64_hw_early_init(const vibeos_boot_info_t *boot_info) {
    /* The Linux syscall registry, before anything that could make a syscall. It
     * refuses a number claimed twice and an operation with no handler, so a
     * malformed table stops the boot with the reason instead of answering some
     * call wrongly for the life of the machine. */
    hw_pipe_init();
    vibeos_linux_abi_init();

    /* SSE on, explicitly, on this core too.
     *
     * ap_boot.S sets OSFXSR and OSXMMEXCPT for every application processor and
     * nothing ever set them for the boot processor - which has worked only
     * because UEFI leaves them on, EDK2 using SSE itself. This kernel compiles
     * thousands of XMM instructions and now runs fxsave on every context
     * switch, and fxsave without OSFXSR is #UD. Inheriting that from firmware
     * is not a decision, it is a coincidence that has held. */
    {
        uint64_t cr4;
        __asm__ __volatile__("movq %%cr4, %0" : "=r"(cr4));
        cr4 |= 0x600ull;                 /* OSFXSR | OSXMMEXCPT */
        __asm__ __volatile__("movq %0, %%cr4" :: "r"(cr4));
    }
    vibeos_x86_64_serial_puts("[HW] early init: loading GDT\n");
    hw_load_gdt(0);
    vibeos_x86_64_serial_puts("[HW] GDT loaded (CS=0x08 DS=0x10)\n");
    hw_boot_stage("gdt");

    hw_load_idt();
    vibeos_x86_64_serial_puts("[HW] IDT loaded (256 gates, 48 vectors wired: 32 exceptions + 16 IRQs)\n");
    hw_boot_stage("idt");

    (void)vibeos_trap_state_init(&g_arch_trap_state);
    g_arch_trap_ready = 1;
    vibeos_x86_64_serial_puts("[HW] trap model armed (routing faults via vibeos_trap_dispatch_ex)\n");
    hw_boot_stage("traps");

    hw_enable_paging();

    vibeos_x86_64_serial_puts("[HW] self-test: raising int3\n");
    __asm__ __volatile__("int3");
    vibeos_x86_64_serial_puts("[HW] resumed after int3 (trap routed through model)\n");
    hw_boot_stage("trap_selftest");

    hw_enable_syscall();
    hw_boot_stage("syscall");

    hw_enable_timer_irq();
    hw_boot_stage("timer");

    /* Move off the legacy PIC/PIT onto the local + IO APIC pair (per-CPU timer,
     * IO-APIC interrupt routing) now that basic IRQ delivery is proven. */
    hw_device_table();
    hw_fs_drivers();
    hw_apic_bringup(boot_info);
    hw_boot_stage("apic_smp");

    /* From here the system is scheduled: the kernel itself becomes a task and
     * user tasks are preempted alongside it. */
    /* The task table's state machine, before anything can claim a slot. */
    (void)vibeos_task_table_init((uint32_t)VIBEOS_HW_MAX_TASKS);
    vibeos_task_view_set_source(hw_task_slots, hw_task_describe);
    (void)vibeos_runq_init(&g_runq, (uint32_t)VIBEOS_HW_MAX_TASKS,
                           (uint32_t)VIBEOS_HW_MAX_CPUS, hw_task_runnable, 0);
    hw_pmm_bringup(boot_info);
    hw_boot_stage("physical_memory");

    /* Display console: render text into the firmware framebuffer, if any. */
    if (boot_info && boot_info->framebuffer_base != 0u) {
        /* The graphical shell needs a screen-sized back buffer. Taken from the
         * page allocator, since a static one would put several megabytes of
         * .bss into every kernel image including the ones that never see a
         * framebuffer. */
        uint64_t px = (uint64_t)boot_info->framebuffer_width *
                      boot_info->framebuffer_height;
        uint64_t pages = (px * 4ull + 4095ull) / 4096ull;
        /* From the frame layer, not from the bump allocator underneath it.
         *
         * This line is why the first attempt at wiring the layer in broke the
         * dynamic loader. hw_pmm_bringup hands the whole region over to the
         * frame layer and reserves the prefix already consumed; this call ran
         * *afterwards* and took several megabytes straight from the bump
         * allocator, which the frame table still described as free. The layer
         * then handed the same frames to a process, and the desktop rendered
         * over its memory.
         *
         * There was no poison hit and no free-while-mapped report, because
         * nothing was freed early - the frames were simply given out twice.
         * That is the failure mode of having two allocators, and it is why the
         * bump allocator is closed below rather than left available. */
        /* One page more than the desktop needs, filled with a pattern nothing
         * else writes, and checked at the end of the boot.
         *
         * The open defect is that a process's argv reads as two white pixels -
         * COL_TITLETXT, twice - with every memory counter at zero. Zero is
         * correct for all of them: nothing was freed early and nothing was
         * double-allocated, so no detector in the memory manager can see this.
         * The remaining explanations are that the desktop writes somewhere it
         * was not given, or that something else writes into what it was.
         *
         * A canary distinguishes those two, which four sessions of reading did
         * not. If it is damaged, the desktop overran its buffer; if it is
         * intact while argv still reads white, the desktop is writing to an
         * address it computed rather than past the end of one it owns - and
         * those need completely different fixes. */
        void *back = hw_alloc_pages_contig((uint32_t)pages + 1u);

        /* Handed to the display's probe (C7). The extra page holds the GUI's
         * canary, which it writes itself directly after the pixels. The
         * identity-map limit is this file's to check: the GUI writes through
         * the pointer it is given and cannot know what is mapped. */
        if (back && ((uint64_t)(uintptr_t)back + (pages + 1ull) * 4096ull) <=
                        VIBEOS_HW_IDENTITY_LIMIT) {
            g_gui_back = back;
            g_gui_back_bytes = (pages + 1ull) * 4096ull;
            g_gui_back_base = (uint64_t)(uintptr_t)back;
            g_gui_back_end = g_gui_back_base + pages * 4096ull;
        }

        /* Where the back buffer actually landed, against the window a Linux
         * program is identity-mapped into.
         *
         * The comment above says the frames were once given out twice and
         * blames the bump allocator, which was closed. This line exists to
         * check the successor of that claim: the frame layer is initialised
         * over the whole PMM region and reserves exactly two things - the
         * prefix the bump allocator already consumed, and everything above the
         * identity limit. The low user window is reserved in the *PMM* and the
         * frame layer is never told, so a first-fit run can land in it.
         *
         * One call, because a line assembled from a dozen is a dozen critical
         * sections and this one has to be read as one fact. */
        {
            uint64_t lo = (uint64_t)(uintptr_t)back;
            uint64_t hi = lo + pages * 4096ull;
            int overlaps = back && lo < VIBEOS_HW_LOW_USER_LIMIT &&
                           hi > VIBEOS_HW_LOW_USER_BASE;
            vibeos_x86_64_serial_lock();
            vibeos_x86_64_serial_puts("[GUI] backbuf=0x");
            vibeos_x86_64_serial_print_hex(lo);
            vibeos_x86_64_serial_puts("..0x");
            vibeos_x86_64_serial_print_hex(hi);
            vibeos_x86_64_serial_puts(" pages=0x");
            vibeos_x86_64_serial_print_hex(pages);
            vibeos_x86_64_serial_puts(" lowuser=0x");
            vibeos_x86_64_serial_print_hex(VIBEOS_HW_LOW_USER_BASE);
            vibeos_x86_64_serial_puts("..0x");
            vibeos_x86_64_serial_print_hex(VIBEOS_HW_LOW_USER_LIMIT);
            vibeos_x86_64_serial_puts(" pmm_base=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_hw_pmm.base);
            vibeos_x86_64_serial_puts(" pmm_prefix=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_hw_pmm.offset_bytes);
            vibeos_x86_64_serial_puts(" SHADOWS_USER=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)overlaps);
            /* And the front buffer, which is firmware-provided and was never
             * checked against the region the allocator hands out of. If a GOP
             * framebuffer sits inside RAM the frame layer manages, every blit
             * writes over somebody's frames - which would produce exactly white
             * pixels in a process's memory with every allocator counter at
             * zero, because nothing was allocated twice or freed early. */
            vibeos_x86_64_serial_puts(" fb=0x");
            vibeos_x86_64_serial_print_hex(boot_info->framebuffer_base);
            vibeos_x86_64_serial_puts("..0x");
            vibeos_x86_64_serial_print_hex(boot_info->framebuffer_base +
                                           px * 4ull);
            vibeos_x86_64_serial_puts(" pmm_end=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_hw_pmm.base +
                                           (uint64_t)g_hw_pmm.size_bytes);
            vibeos_x86_64_serial_puts(" FB_IN_RAM=0x");
            vibeos_x86_64_serial_print_hex(
                (uint64_t)(boot_info->framebuffer_base <
                               (uint64_t)g_hw_pmm.base +
                               (uint64_t)g_hw_pmm.size_bytes &&
                           boot_info->framebuffer_base + px * 4ull >
                               (uint64_t)g_hw_pmm.base));
            vibeos_x86_64_serial_puts("\n");
            vibeos_x86_64_serial_unlock();
        }

    }

    hw_klog_init();
    hw_crash_init();
    hw_log(VIBEOS_LOG_INFO, 0, 0, 0, "kernel log ready");
    hw_boot_stage("log");

    /* Every registered device, once the screen's size is known (a pointer
     * clamps to it) and the kernel log exists (the disk drivers log). The mouse
     * used to be initialised by name inside the framebuffer setup above and the
     * disks just below; their probes keep the same conditions. */
    {
        vibeos_dev_env_t env = { 0 };
        if (boot_info && boot_info->framebuffer_base != 0u) {
            env.fb_base = boot_info->framebuffer_base;
            env.fb_width = boot_info->framebuffer_width;
            env.fb_height = boot_info->framebuffer_height;
            env.fb_back = g_gui_back;
            env.fb_back_bytes = g_gui_back_bytes;
        }
        (void)vibeos_device_probe_all(&env);
    }

    /* The text console, only on a framebuffer no display took. It used to be
     * decided before the display existed, and its "else" said "no
     * framebuffer; console is serial-only" on every boot that had a desktop -
     * the one line about the screen, wrong whenever there was one. Three
     * cases, three lines. */
    if (vibeos_display_present()) {
        vibeos_x86_64_serial_puts("[GUI] desktop up: 0x");
        vibeos_x86_64_serial_print_hex(boot_info->framebuffer_width);
        vibeos_x86_64_serial_puts("x0x");
        vibeos_x86_64_serial_print_hex(boot_info->framebuffer_height);
        vibeos_x86_64_serial_puts("\n");
    } else if (boot_info && boot_info->framebuffer_base != 0u &&
               vibeos_x86_64_fb_init(boot_info->framebuffer_base,
                                     boot_info->framebuffer_width,
                                     boot_info->framebuffer_height) == 0) {
        vibeos_x86_64_serial_puts("[FB] framebuffer console ready: 0x");
        vibeos_x86_64_serial_print_hex(boot_info->framebuffer_width);
        vibeos_x86_64_serial_puts(" x 0x");
        vibeos_x86_64_serial_print_hex(boot_info->framebuffer_height);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_fb_puts("VibeOS console\n");
    } else {
        vibeos_x86_64_serial_puts("[FB] no framebuffer; console is serial-only\n");
    }

    /* Real storage: find a disk, mount the FAT filesystem, and load the init
     * program straight from it (EFI/BOOT/INIT.ELF -> INIT.ELF at root).
     *
     * Two drivers, tried in this order, and the order is the whole point:
     * virtio-blk is what QEMU offers and is faster, AHCI is what VirtualBox,
     * VMware and real machines offer. Only virtio existed until now, so the
     * appliances booted and then could not read their own disk - the
     * bootloader hid it, because UEFI does the reading up to ExitBootServices
     * and after that there was simply no device. */
    /* Every disk the device registry found, bound in table order (C7). The
     * drivers used to be initialised and bound here by name - virtio-blk, then
     * AHCI, each with seven of its functions spelled out - so a third disk
     * driver meant editing this function. Which disk is the boot disk is
     * decided by mounting, just below, not by this order; see there for why
     * bind order stopped deciding it. */
    {
        uint32_t i;
        for (i = 0; i < vibeos_device_count(); i++) {
            const vibeos_device_t *d = vibeos_device_at(i);
            const vibeos_block_ops_t *op;
            if (d->cls != VIBEOS_DEV_BLOCK || !vibeos_device_present(i) || !d->ops) {
                continue;
            }
            op = (const vibeos_block_ops_t *)d->ops;
            vibeos_x86_64_blk_bind(d->name, op->read, op->read_many, op->write,
                                   op->write_many, op->barrier, op->sectors(),
                                   op->timeouts);
        }
    }
    {
        uint32_t d;
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[BLK] disks=0x");
        vibeos_x86_64_serial_print_hex(vibeos_x86_64_blk_adapter_count());
        for (d = 0; d < vibeos_x86_64_blk_adapter_count(); d++) {
            vibeos_x86_64_serial_puts(" disk=");
            vibeos_x86_64_serial_puts(vibeos_x86_64_blk_adapter_name(d));
            vibeos_x86_64_serial_puts(":0x");
            vibeos_x86_64_serial_print_hex(
                (uint64_t)vibeos_x86_64_blk_adapter_device(d));
        }
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
    hw_boot_stage("block_device");

    /* Which of the disks is the boot disk, decided by mounting rather than by
     * bind order.
     *
     * The order used to decide, and the comment above says so as a deliberate
     * choice. It was right with one disk. With the ESP on AHCI and I5b's log
     * disk on virtio-blk - which is exactly the AHCI CI configuration, and what
     * every desktop hypervisor offers - adapter 0 is a blank 4 MiB file: the
     * mount read its sector 0, found no signature, and every exec for the rest
     * of the boot returned not-found. The whole boot did one read. Nothing in
     * the block layer had anything to report, correctly - reading the wrong
     * disk succeeds.
     *
     * So each adapter is tried until one carries a mountable volume. The cost
     * is one sector read per rejected disk, once. */
    {
        uint32_t d, n = vibeos_x86_64_blk_adapter_count();
        for (d = 0; d < n; d++) {
            if (vibeos_x86_64_blk_set_boot(d) != 0) {
                continue;
            }
            /* By name, and not by scanning: the machine boots from the EFI
             * system partition, which UEFI requires to be FAT. A first_lba of
             * zero is that driver's "the boot volume" (C7). */
            const vibeos_fs_driver_t *fat = vibeos_storage_driver("fat");
            if (fat && fat->mount(&g_rootfs, 0, 0ull, 0ull, 0) == 0) {
                g_boot_disk_mounted = 1;
                break;
            }
        }
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[BLK] boot volume on ");
        vibeos_x86_64_serial_puts(g_boot_disk_mounted
                                  ? vibeos_x86_64_blk_name() : "none");
        vibeos_x86_64_serial_puts(" rejected=0x");
        vibeos_x86_64_serial_print_hex(vibeos_x86_64_blk_boot_rejected());
        vibeos_x86_64_serial_puts(" disks=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)n);
        /* Kept, and moved: the boot disk by name is what several existing
         * checks read. It used to be printed before anything was mounted, so
         * it named whichever driver bound first - which is the very thing that
         * was wrong, announced in the line people read to check it. Bracketed
         * into this critical section rather than three of its own, for the
         * reason the console lock exists. */
        vibeos_x86_64_serial_puts("\n[BLK] disk driver: ");
        vibeos_x86_64_serial_puts(g_boot_disk_mounted
                                  ? vibeos_x86_64_blk_name() : "none");
        vibeos_x86_64_serial_puts("\n");
        /* A boot with no volume carries on - the built-in init and the kernel
         * console still work, and that is worth having on a machine whose disk
         * this kernel cannot read. But it runs none of the machine's programs,
         * and "boot volume on none" alone was read by nobody: the boot gate
         * waited for a self-test that lived on the missing disk and called the
         * idle prompt a wedge. Said as a stage failure, so it is a failure by
         * name. `tried`, not `rejected`: rejected counts the disks passed over
         * on the way to one that mounted, so with none it is one short. */
        if (!g_boot_disk_mounted) {
            vibeos_x86_64_serial_puts("[BLK] BOOT_VOLUME_FAIL: no disk carries a "
                                      "mountable volume tried=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)n);
            vibeos_x86_64_serial_puts("; userland runs from the built-in image only\n");
        }
        vibeos_x86_64_serial_unlock();
    }
    if (g_boot_disk_mounted) {
        hw_volumes_bringup();
        /* After the real volume is mounted, so the scratch device can never be
         * confused with it: it is registered second and named separately. */
        hw_scratch_bringup();
        hw_logsink_bringup();
        hw_fsimages_bringup();
        hw_mount_report();
        hw_swap_bringup();
        /* After the volume is mounted and before anything else uses it. The
         * file it leaves behind is small and is overwritten every boot. */
        hw_write_proof();
        long n = vibeos_fs_read_file(&g_rootfs, "EFI/BOOT/INIT.ELF",
                                      g_disk_init_elf, sizeof(g_disk_init_elf));
        if (n > 0) {
            g_disk_init_len = n;
            hw_log(VIBEOS_LOG_INFO, 1u, (uint64_t)n, 0,
                   "init program read from disk");
            vibeos_x86_64_serial_puts("[FAT] read INIT.ELF from disk, size=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)n);
            vibeos_x86_64_serial_puts("\n");
        } else {
            hw_log(VIBEOS_LOG_ERROR, 2u, 0, 0,
                   "init program missing from the boot volume");
            vibeos_x86_64_serial_puts("[FAT] INIT.ELF not found on disk\n");
        }
    }

    /* Network interface: virtio-net + the TCP/IP stack, addressed by DHCP. */
    hw_net_bringup();
    hw_boot_stage("network");

    /* Userland does *not* start here any more.
     *
     * It used to, and the name of this function is what hid it: everything
     * below the hardware - init, every service, the self-test, BusyBox, the
     * shell - ran to completion inside something called "early init", and
     * vibeos_kmain was only entered afterwards, to bring up portable
     * subsystems nothing would ever use and print BOOT_OK.
     *
     * That made BOOT_OK mean "the machine has already finished" rather than
     * "the kernel is up", so the boot gate attributed every userland hang to
     * the last bootloader marker it had seen. A full session went into looking
     * for a firmware bug that did not exist.
     *
     * The kernel now announces itself before it runs anything: entry.s calls
     * this, then vibeos_kmain, and vibeos_kmain calls
     * vibeos_x86_64_hw_start_userland once it has said BOOT_OK. */
    g_saved_boot_info = boot_info;

    hw_boot_stage("hardware_ready");
    vibeos_x86_64_serial_puts("[HW] HW_INIT_OK\n");
}

/* Start init and everything under it. Called from vibeos_kmain, after the
 * portable subsystems are up and the kernel has said so. Does not return until
 * every user task has retired. */
void vibeos_x86_64_hw_start_userland(void) {
    hw_sched_bringup(g_saved_boot_info);
    hw_tlbq_selftest();
}
