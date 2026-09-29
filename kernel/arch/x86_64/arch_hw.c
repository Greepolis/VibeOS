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
volatile uint64_t g_input_irq_wakes;

/* An interrupt on a registry vector that no device owns. Must be zero: a
 * device raising one would be a driver routing its line to a vector it was not
 * given, and it would otherwise be acknowledged and forgotten. */
volatile uint64_t g_device_stray_irqs;

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
uint8_t g_ap_boot_stack[VIBEOS_HW_MAX_CPUS][16384] __attribute__((aligned(16)));

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
volatile int g_sched_running;

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


/* vibeos_x86_64_isr_frame_t now lives in arch_hw_internal.h. */

/* The two disk interrupt handlers, declared where the dispatcher can see
 * them. They were being called implicitly: gcc accepts that with a warning,
 * clang refuses it, and code scanning reported both call sites. */


int hw_user_range_ok(uint64_t va, uint64_t len, int need_write);
/* The fault-tolerant copy's faulting range and recovery point (uaccess.S). */
extern const char vibeos_uaccess_copy_begin[];
extern const char vibeos_uaccess_copy_end[];
extern void vibeos_uaccess_copy_fixup(void);
static uint64_t g_uaccess_recovered;

/* Load the shared GDT on this CPU, install its private TSS, and point GS.base
 * at its per-CPU block. Every core runs this; the shared descriptors are
 * rewritten identically, which is harmless. */
void hw_load_gdt(uint32_t cpu_index) {
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

/* The local APIC's end-of-interrupt (apic.c); the rest of the APIC and SMP
 * interface is smp.c's. */
extern void vibeos_x86_64_lapic_eoi(void);

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
uint64_t g_gui_back_base;
uint64_t g_gui_back_end;
uint64_t g_gui_back_shared;
uint64_t g_gui_back_lost;
/* g_ring3_write_nul and the g_abi_* counters are the Linux layer's now
 * (vibeos/linux_exports.h); this file prints them. */
extern int vibeos_x86_64_fb_init(uint64_t base, uint32_t width, uint32_t height);
extern int vibeos_x86_64_fb_ready(void);
extern void vibeos_x86_64_fb_puts(const char *s);

/* Init program read from the on-disk filesystem, if present. */
uint8_t g_disk_init_elf[65536] __attribute__((aligned(16)));
long g_disk_init_len = -1;

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
void hw_load_idt_only(void) {
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
/* Defined with the task code, because it needs the signal numbers that are
 * #defined a thousand lines below here and C only reads the file once. */
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
/* hw_perf_t is in arch_hw_internal.h: the context switch counts into one. */

hw_perf_t g_perf_syscall;
hw_perf_t g_perf_fault;
hw_perf_t g_perf_switch;

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
                (void)linux_signal_deliver((struct ks_regs *)frame);
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
void hw_enable_syscall(void) {
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
/* ---- pipes and the console, as the task code sees them ---------------------
 * The task table, the scheduler and the context switch moved to task_switch.c
 * (2026-09-28). */


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

/* The mount table's own lock (vibeos_fs_set_lock). Its own, not the pipes' or the
 * scheduler's: resolving a path must not wait on either, and nothing is called
 * while it is held. Registered before the first attach, which is the storage
 * bring-up; a call before this counts as mount_unlocked. */
static hw_lock_t g_mount_lock;

static void hw_mount_lock(void) {
    hw_spin_lock_named(&g_mount_lock, "vibeos_mounts");
}

static void hw_mount_unlock(void) {
    hw_spin_unlock(&g_mount_lock);
}

void hw_mount_table_init(void) {
    vibeos_fs_set_lock(hw_mount_lock, hw_mount_unlock);
}

/* hw_task_t now lives in arch_hw_internal.h, so the files lifted out
 * of here can see it. */

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

/* ---- the syscall entry, into the Linux ABI layer (kernel/abi/linux) ---------
 *
 * The syscall handlers moved to kernel/abi/linux (C4 stage 3); the page-table
 * primitives and the copy-on-write fault to mm_bridge.c and the crash dumper to
 * diagnostics.c (2026-09-28/29). What is left here is the one call the
 * trampoline makes. */

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
    (void)linux_signal_deliver((struct ks_regs *)frame);
}

/* Bring the scheduler up: spawn the initial user tasks, adopt the kernel as a
 * task, and let the timer preempt from here on. */
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
    hw_mount_table_init();
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
