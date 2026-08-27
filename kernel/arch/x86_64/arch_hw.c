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

#define VIBEOS_HW_KERNEL_CS 0x08u
#define VIBEOS_HW_KERNEL_DS 0x10u
#define VIBEOS_HW_IDT_GATES 256u
#define VIBEOS_HW_GATE_INTERRUPT 0x8Eu /* present, DPL=0, 64-bit interrupt gate */
#define VIBEOS_HW_WIRED_VECTORS 48u    /* 0-31 CPU exceptions + 32-47 PIC IRQs */

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
#define VIBEOS_HW_TIMER_HZ 100u

/* ---- GDT + TSS ---------------------------------------------------------- */

/* SYSRET requires user data (SS) to precede user code (CS) in the GDT, so the
 * user segments are ordered data-then-code: index 3 = data, index 4 = code. */
#define VIBEOS_HW_USER_DATA_SEL 0x1Bu   /* GDT index 3, RPL 3 */
#define VIBEOS_HW_USER_CODE_SEL 0x23u   /* GDT index 4, RPL 3 */
#define VIBEOS_HW_TSS_SEL 0x28u         /* GDT index 5 */

struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

#define VIBEOS_HW_MAX_CPUS 8u

/* null, kcode, kdata, ucode, udata, then one 16-byte (two-slot) TSS descriptor
 * per CPU: every core needs its own task state segment for RSP0. */
static uint64_t g_gdt[5u + 2u * VIBEOS_HW_MAX_CPUS];

#define VIBEOS_HW_TSS_SEL_FOR(cpu) ((uint16_t)((5u + 2u * (cpu)) * 8u))

/* ---- per-CPU state ------------------------------------------------------- */

/* One of these per core, reached through GS.base. The first two fields are read
 * and written by the `syscall` trampoline in isr.S at fixed offsets 0 and 8 -
 * do not reorder them.
 *
 * GS.base is programmed once per CPU and never swapped: user code cannot change
 * it (CR4.FSGSBASE stays clear and ring-3 programs never load %gs), so the
 * kernel entry paths can rely on it without a swapgs dance. */
typedef struct hw_cpu {
    uint64_t syscall_kstack_top;  /* offset 0  - isr.S loads rsp from here */
    uint64_t user_saved_rsp;      /* offset 8  - isr.S stashes the user rsp */
    struct hw_cpu *self;          /* offset 16 - so C can find its own block */
    uint32_t lapic_id;
    uint32_t index;
    int current_task;             /* index into g_tasks, -1 before bring-up */
    int idle_task;                /* this core's idle task, -1 on the BSP */
    volatile int online;
    struct tss64 tss;
} hw_cpu_t;

static hw_cpu_t g_cpus[VIBEOS_HW_MAX_CPUS];
static uint32_t g_cpu_online_count = 1u;

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

static hw_cpu_t *hw_this_cpu(void) {
    hw_cpu_t *p;
    __asm__ __volatile__("movq %%gs:16, %0" : "=r"(p));
    return p;
}

/* The scheduler state below is shared by every core; `g_current_task` is not.
 * Making it a macro over the per-CPU block keeps every existing use site
 * (syscalls, exit, fork) correct on SMP without threading a CPU argument
 * through the whole syscall layer. */
#define g_current_task (hw_this_cpu()->current_task)

/* Console-lock ownership (overrides the weak default in serial.c). Reads the
 * GS base MSR directly so it is safe to call before the per-CPU block is
 * installed, which happens after the first boot messages. */
uint32_t vibeos_x86_64_cpu_id(void) {
    uint32_t lo, hi;
    uint64_t base;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101u));
    base = ((uint64_t)hi << 32) | lo;
    return (base == 0u) ? 0u : ((const hw_cpu_t *)(uintptr_t)base)->index;
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

typedef struct {
    volatile int locked;
    uint64_t flags;   /* caller's RFLAGS, restored on release */
} hw_lock_t;

static hw_lock_t g_sched_lock;
static hw_lock_t g_mm_lock;
static hw_lock_t g_exec_lock;

/* The TCP/IP stack is entered both from syscalls and from the timer interrupt
 * that pumps the device, so it lives behind its own lock. Declared here because
 * the socket syscalls appear before the network bring-up code below. */
static vibeos_inet_t g_net;
static hw_lock_t g_net_lock;
static int g_net_up;

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
static void hw_spin_lock_preemptible(hw_lock_t *lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
    lock->flags = 0;
}

static void hw_spin_unlock_preemptible(hw_lock_t *lock) {
    __sync_lock_release(&lock->locked);
}

static void hw_spin_lock(hw_lock_t *lock) {
    uint64_t flags;
    __asm__ __volatile__("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
    lock->flags = flags;
}

static void hw_spin_unlock(hw_lock_t *lock) {
    uint64_t flags = lock->flags;
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

/* ELF loader (elf_load.c) and the embedded user program (generated blob).
 * The loader hands each PT_LOAD segment to a callback so we can place it in a
 * process's private address space. */
typedef int (*vibeos_elf_load_cb)(void *ctx, uint64_t vaddr, const unsigned char *data,
                                  uint64_t filesz, uint64_t memsz, uint32_t flags);
extern int vibeos_x86_64_elf_load(const unsigned char *elf, uint64_t len,
                                  void *ctx, vibeos_elf_load_cb cb, uint64_t *out_entry);
extern const unsigned char vibeos_user_hello_elf[];
extern const unsigned long vibeos_user_hello_elf_len;

/* Frame pushed by the ISR stubs, in ascending memory order. */
typedef struct vibeos_x86_64_isr_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} vibeos_x86_64_isr_frame_t;

static void hw_schedule(vibeos_x86_64_isr_frame_t *frame); /* defined below */
static void hw_task_exit(uint64_t code);                   /* defined below */
static void hw_keyboard_wake(void);                        /* defined below */
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

/* Network interface (virtio_net.c). */
extern int vibeos_x86_64_virtio_net_init(void);
extern const uint8_t *vibeos_x86_64_virtio_net_mac(void);
extern int vibeos_x86_64_virtio_net_ready(void);
extern int vibeos_x86_64_virtio_net_send(const void *frame, uint32_t len);
extern int vibeos_x86_64_virtio_net_recv(void *out, uint32_t cap);
extern void vibeos_x86_64_virtio_net_stats(uint64_t *out_tx, uint64_t *out_rx);

extern int vibeos_x86_64_virtio_blk_init(void);
extern int vibeos_x86_64_virtio_blk_read(uint64_t sector, void *buf);
#include "vibeos/log.h"
#include "vibeos/vfs.h"

/* The one mounted volume. Everything below reaches the filesystem through
 * this, so no syscall in this file knows which driver is underneath it. */
static vibeos_fsmount_t g_rootfs;
extern int vibeos_x86_64_fat_vfs_mount(vibeos_fsmount_t *mnt);

extern void vibeos_x86_64_keyboard_irq(void);
extern void vibeos_x86_64_mouse_irq(void);
extern int vibeos_x86_64_mouse_init(uint32_t width, uint32_t height);
extern int vibeos_x86_64_mouse_ready(void);
extern uint32_t vibeos_x86_64_mouse_packets(void);
extern int vibeos_x86_64_gui_init(uint64_t fb_base, uint32_t width, uint32_t height,
                                  void *back_buffer);
extern void vibeos_x86_64_gui_tick(void);
extern int vibeos_x86_64_gui_active(void);
extern uint32_t vibeos_x86_64_gui_frames(void);
extern uint32_t vibeos_x86_64_gui_term_chars(void);
extern int vibeos_x86_64_keyboard_getc(void);
extern void vibeos_x86_64_keyboard_inject(const char *s);
extern int vibeos_x86_64_fb_init(uint64_t base, uint32_t width, uint32_t height);
extern int vibeos_x86_64_fb_ready(void);
extern void vibeos_x86_64_fb_putc(char c);
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
static volatile uint64_t g_timer_ticks;

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
static int g_apic_mode;

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

static int hw_handle_cow_fault(uint64_t fault_va, uint64_t error_code);
/* Defined with the rest of the signal code, far below; the timer path needs it
 * here so a signal raised while a task was running is delivered on the way
 * back to ring 3 rather than at the next syscall. */
static int hw_signal_deliver(vibeos_x86_64_isr_frame_t *frame);
static int hw_signal_raise(int task_index, uint32_t sig);

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

/* ---- the kernel log ------------------------------------------------------
 *
 * There has been a structured log with levels in kernel/core/log.c since early
 * on, and the running kernel never used it: the trap dispatcher was handed a
 * null pointer for it, and everything that mattered was printed with bare
 * serial_puts calls carrying no level at all. So there was no way to ask for
 * more detail, no way to ask for less, and nothing at all to show after a
 * machine went quiet.
 *
 * Two things are wanted from a log here and they pull in opposite directions.
 * A failure in CI should print the last thing that happened, which argues for
 * writing everything to the serial line. But the serial line is slow enough
 * under emulation to change the timing of the bug being hunted, and this
 * kernel has an intermittent wedge that is plainly timing-sensitive. So events
 * are always recorded into a ring in memory, which costs a memcpy, and only
 * those at or above a threshold are also written out. When the machine dies,
 * the ring is dumped - including everything that was too quiet to print at the
 * time, which is usually the part worth reading. */

static vibeos_log_t g_kernel_log;
static uint32_t g_log_serial_level = VIBEOS_LOG_INFO;

static void hw_log_emit(const vibeos_log_event_t *ev) {
    vibeos_x86_64_serial_puts("[LOG][");
    vibeos_x86_64_serial_puts(vibeos_log_level_name((vibeos_log_level_t)ev->level));
    vibeos_x86_64_serial_puts("] ");
    vibeos_x86_64_serial_puts(ev->message);
    if (ev->code != 0u || ev->arg0 != 0u || ev->arg1 != 0u) {
        vibeos_x86_64_serial_puts(" code=0x");
        vibeos_x86_64_serial_print_hex(ev->code);
        vibeos_x86_64_serial_puts(" a0=0x");
        vibeos_x86_64_serial_print_hex(ev->arg0);
        vibeos_x86_64_serial_puts(" a1=0x");
        vibeos_x86_64_serial_print_hex(ev->arg1);
    }
    vibeos_x86_64_serial_puts("\n");
}

static void hw_log(vibeos_log_level_t level, uint32_t code, uint64_t a0,
                   uint64_t a1, const char *message) {
    vibeos_log_event_t ev;

    (void)vibeos_log_record(&g_kernel_log, level, code, a0, a1, message);
    if ((uint32_t)level < g_log_serial_level) {
        return;   /* recorded, not printed: it is still there after a panic */
    }
    if (vibeos_log_latest(&g_kernel_log, &ev) == 0) {
        hw_log_emit(&ev);
    }
}

/* Everything the ring still holds, oldest first. Called when the machine is
 * about to stop, which is the only moment the quiet events are worth their
 * transmission time. */
static void hw_log_dump(void) {
    uint32_t count = 0;
    uint32_t dropped = 0;
    uint32_t i;

    if (vibeos_log_count(&g_kernel_log, &count) != 0) {
        return;
    }
    (void)vibeos_log_dropped(&g_kernel_log, &dropped);

    vibeos_x86_64_serial_puts("[LOG] dump count=0x");
    vibeos_x86_64_serial_print_hex(count);
    vibeos_x86_64_serial_puts(" dropped=0x");
    vibeos_x86_64_serial_print_hex(dropped);
    vibeos_x86_64_serial_puts("\n");

    for (i = 0; i < count; i++) {
        vibeos_log_event_t ev;

        if (vibeos_log_get(&g_kernel_log, i, &ev) != 0) {
            continue;
        }
        vibeos_x86_64_serial_puts("[LOG] #");
        vibeos_x86_64_serial_print_hex(ev.seq);
        vibeos_x86_64_serial_puts(" ");
        hw_log_emit(&ev);
    }
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
}

static void hw_panic(const char *why) {
    uint64_t rbp;
    uint64_t rip;

    __asm__ __volatile__("movq %%rbp, %0" : "=r"(rbp));
    /* The panic site itself, so the first line of the trace is this call and
     * not whatever the compiler left in a register. */
    __asm__ __volatile__("leaq (%%rip), %0" : "=r"(rip));

    vibeos_x86_64_serial_puts(" FATAL: ");
    vibeos_x86_64_serial_puts(why);
    vibeos_x86_64_serial_puts(", halting\n");
    hw_backtrace(rbp, rip);
    hw_log_dump();
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/* Linux x86-64 syscall front end; defined after the task/address-space code
 * because it validates user pointers against the calling task's page tables. */
long vibeos_x86_64_linux_syscall(vibeos_x86_64_isr_frame_t *frame, uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3);
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

    if (frame->vector >= VIBEOS_HW_IRQ_BASE && frame->vector < VIBEOS_HW_WIRED_VECTORS) {
        if (frame->vector == VIBEOS_HW_IRQ_TIMER) {
            /* Every core's LAPIC timer lands here; only one may own the clock. */
            if (!g_apic_mode || hw_this_cpu()->index == 0u) {
                g_timer_ticks++;
            }
            hw_pic_send_eoi((uint32_t)frame->vector);
            /* One core owns the network clock: draining the device from every
             * core would just contend on the same lock. */
            if (!g_apic_mode || hw_this_cpu()->index == 0u) {
                hw_net_pump();
                /* Repaint the pointer. Cheap by construction: it touches only
                 * the two small rectangles that can have changed. */
                vibeos_x86_64_gui_tick();
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
        if (frame->vector == 44u) { /* IRQ12: PS/2 mouse */
            vibeos_x86_64_mouse_irq();
        }
        if (frame->vector == 33u) { /* IRQ1: keyboard */
            vibeos_x86_64_keyboard_irq();
            hw_keyboard_wake();
            hw_pic_send_eoi((uint32_t)frame->vector);
            return;
        }
        hw_pic_send_eoi((uint32_t)frame->vector);
        return;
    }

    fault_address = (frame->vector == 14u) ? hw_read_cr2() : 0u;

    /* A write to a shared page is not an error, it is the mechanism: fork
     * leaves both processes pointing at the same read-only frame, and this is
     * where the copy actually happens. Resolved faults must be handled before
     * anything is reported, or every fork would look like a crash. */
    if (frame->vector == 14u && hw_handle_cow_fault(fault_address, frame->error_code)) {
        return;
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

    if (vibeos_trap_dispatch_ex(&g_arch_trap_state, &tf, &g_kernel_log,
                                0, &decision) != 0) {
        hw_panic("trap dispatch failed");
    }

    vibeos_x86_64_serial_puts(" -> action=");
    vibeos_x86_64_serial_puts(hw_action_name(decision.action));
    vibeos_x86_64_serial_puts(" count=0x");
    vibeos_x86_64_serial_print_hex(g_arch_trap_state.trap_count);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    if (decision.action == VIBEOS_TRAP_ACTION_CONTINUE) {
        /* Resumable trap (e.g. #BP): iretq returns to the saved RIP. */
        return;
    }

    /* The trace comes from the interrupted frame's own base pointer, not from
     * this handler's: what is wanted is the path that reached the fault, and
     * the handler's own frames are noise on top of it. */
    hw_backtrace(frame->rbp, frame->rip);

    /* KILL_CURRENT has no meaning yet (no user processes on metal); both
     * remaining actions are fatal during bring-up. */
    hw_panic("unrecoverable CPU exception");
}

/* ---- Paging ------------------------------------------------------------- */

#define PTE_PRESENT 0x001ull
#define PTE_WRITE   0x002ull
#define PTE_USER    0x004ull            /* ring-3 accessible */
#define PTE_PS      0x080ull            /* 2 MiB page at PD level */
/* Bits 9 through 11 are ignored by the hardware and belong to the OS. This one
 * marks a page that is shared after fork and must be duplicated before it is
 * written. Without it a read-only page is indistinguishable from a page the
 * program was never allowed to write, and a genuine protection fault would be
 * silently turned into a successful write. */
#define PTE_COW     0x200ull
#define VIBEOS_HW_IDENTITY_GIB 4u       /* identity-map the first 4 GiB */
#define VIBEOS_HW_IDENTITY_LIMIT 0x100000000ull

/* setjmp-style kernel context saved by ring3_enter (see isr.S). Global so the
 * assembly can reference it by name. Layout: rbx,rbp,r12,r13,r14,r15,rsp. */
uint64_t g_ring3_kctx[8];

/* The `syscall` trampoline (isr.S) stashes the user rsp and loads a kernel
 * stack itself; both live in the per-CPU block reached through GS.base
 * (hw_cpu_t fields at offsets 8 and 0).
 */

/* Kernel-owned page tables (static BSS, no PMM dependency during bring-up).
 * The identity map is supervisor-only; user memory lives in its own PML4 slot
 * with per-process tables, so ring 3 cannot reach kernel pages. */
static uint64_t g_pml4[512] __attribute__((aligned(4096)));
static uint64_t g_pdpt[512] __attribute__((aligned(4096)));
static uint64_t g_pd[VIBEOS_HW_IDENTITY_GIB][512] __attribute__((aligned(4096)));

/* ---- Page pool + per-process address spaces ------------------------------ */

/* Page frames come from the real physical memory manager, initialized from the
 * firmware memory map. The small static pool is only a fallback for boot paths
 * that hand us no boot_info (e.g. a direct -kernel load). */
#define VIBEOS_HW_POOL_PAGES 32u
static uint8_t g_page_pool[VIBEOS_HW_POOL_PAGES][4096] __attribute__((aligned(4096)));
static uint32_t g_pool_next;

/* Staging buffer for an image being exec'd.
 *
 * A real program is not small - BusyBox is about two megabytes - and putting
 * that in .bss would add it to every kernel image whether or not anything ever
 * execs. It is taken from the page allocator instead, once, at boot, and a
 * small static buffer remains as the fallback for the early paths that run
 * before the allocator exists. */
#define VIBEOS_HW_EXEC_STAGE_BYTES (4u * 1024u * 1024u)
static uint8_t g_exec_elf_static[65536] __attribute__((aligned(16)));
static uint8_t *g_exec_elf = g_exec_elf_static;
static uint32_t g_exec_elf_cap = (uint32_t)sizeof(g_exec_elf_static);

/* Which image the staging buffer currently holds.
 *
 * A shell runs the same binary over and over - every external command in a
 * BusyBox system is the same two megabytes - and re-reading it from the
 * filesystem each time is the single most expensive thing an exec does. The
 * buffer is already there and already holds exactly those bytes, so the read
 * can be skipped when the path has not changed.
 *
 * Correctness rests on the whole thing living under g_exec_lock, and on any
 * write to the volume dropping the cache: a program that has been rewritten
 * must not keep running as its old self. */
static char g_exec_cached[128];
static long g_exec_cached_len;

static void hw_exec_cache_drop(void) {
    g_exec_cached[0] = 0;
    g_exec_cached_len = 0;
}

static int hw_exec_cache_hit(const char *path) {
    uint32_t i;
    if (g_exec_cached_len <= 0) {
        return 0;
    }
    for (i = 0; i < sizeof(g_exec_cached); i++) {
        if (g_exec_cached[i] != path[i]) {
            return 0;
        }
        if (path[i] == 0) {
            return 1;
        }
    }
    return 0;
}

/* How many address spaces map each physical frame.
 *
 * fork() no longer copies pages; it maps the parent's frames into the child
 * read-only and marks both copies as copy-on-write. A frame may therefore be
 * live in several address spaces at once, and freeing it when the first of
 * them exits would hand a running process's memory to the allocator.
 *
 * One byte per frame over the allocator's region, taken from that region at
 * boot. A count of zero means "one owner" so that the ordinary case needs no
 * bookkeeping at all: only sharing writes here. The count saturates rather
 * than wrapping - at 255 owners the frame is simply never reclaimed, which
 * leaks a page instead of freeing memory somebody is still using. */
static uint8_t *g_frame_refs;
/* How much work copy-on-write actually did. Reported at the end of the boot,
 * because a mechanism that is never exercised and a mechanism that does not
 * work look identical from outside: shared says how many pages fork handed
 * over instead of copying, copied says how many of those a write later forced
 * it to duplicate after all. */
static volatile uint64_t g_cow_shared;
static volatile uint64_t g_cow_copied;
static uint64_t g_frame_refs_base;
static uint64_t g_frame_refs_count;

static uint8_t *frame_ref_slot(uint64_t phys) {
    uint64_t idx;
    if (!g_frame_refs) {
        return 0;
    }
    if (phys < g_frame_refs_base) {
        return 0;
    }
    idx = (phys - g_frame_refs_base) / 4096ull;
    return (idx < g_frame_refs_count) ? &g_frame_refs[idx] : 0;
}

static void frame_ref_inc(uint64_t phys) {
    uint8_t *slot = frame_ref_slot(phys);
    if (slot && *slot < 255u) {
        (*slot)++;
    }
}

/* Returns non-zero when the caller was the last owner and may free it. */
static int frame_ref_dec(uint64_t phys) {
    uint8_t *slot = frame_ref_slot(phys);
    if (!slot) {
        return 1;   /* not tracked: it was never shared */
    }
    if (*slot == 0u) {
        return 1;   /* sole owner */
    }
    if (*slot < 255u) {
        (*slot)--;
    }
    return 0;
}

static vibeos_pmm_t g_hw_pmm;
static int g_hw_pmm_ready;
static uint64_t g_hw_pmm_pages_used;

/* User virtual layout: PML4 slot 1 (512 GiB). VibeOS programs are linked at
 * VIBEOS_HW_USER_BASE (user/prog/user.ld); their stack sits above the image. */
#define VIBEOS_HW_USER_BASE 0x8000000000ull

/* A second, much smaller user window down in the first GiB.
 *
 * This exists for one reason: a Linux executable is linked at 0x400000 and
 * cannot be asked to move. That address is inside the region the kernel
 * identity-maps for itself, so mapping user pages there means shadowing the
 * kernel's own view of those physical addresses while that process is current.
 * The window is therefore kept small and, crucially, the same physical range
 * is reserved out of the page allocator at boot (vibeos_pmm_reserve), so no
 * kernel object can ever live at an address a process is able to shadow.
 *
 * Only the program image goes here. Its stack, heap and mmap arena stay in the
 * high window, where there is no such interaction - nothing in the Linux ABI
 * requires those to be at any particular address. */
#define VIBEOS_HW_LOW_USER_BASE  0x00400000ull
#define VIBEOS_HW_LOW_USER_LIMIT 0x00C00000ull   /* 8 MiB */
#define VIBEOS_HW_USER_STACK_TOP (VIBEOS_HW_USER_BASE + 0x00400000ull) /* +4 MiB */

/* Programs no longer start on a bare stack: hw_proc_create builds the System V
 * startup block (argc, argv, envp, auxv) in the topmost stack page and reports
 * the stack pointer to enter on, which is 16-byte aligned as the ABI requires.
 * `_start` is written in assembly (user/prog/crt0.S) precisely so that state is
 * consumed correctly instead of being reinterpreted as a function frame. */
#define VIBEOS_HW_USER_STACK_PAGES 4u

typedef struct vibeos_hw_aspace {
    uint64_t *pml4;
} vibeos_hw_aspace_t;

static uint64_t hw_read_cr3(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static void hw_write_cr3(uint64_t pml4_phys) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pml4_phys) : "memory");
}

/* Allocate one zeroed page frame. Frames must live inside the identity-mapped
 * window, since the kernel reaches page tables and process images through it. */
/* Freelist of reclaimed 4 KiB pages; the link is stored in the page itself.
 * This gives real reclamation (address spaces freed on exit/exec) without
 * needing a free path in the portable PMM. */
static void *g_free_pages;

/* Both sides of the page allocator are shared by every core (a task can fork or
 * exit on any of them), so they take the memory lock. */
static void hw_free_page(void *p) {
    if (!p) {
        return;
    }
    hw_spin_lock(&g_mm_lock);
    *(void **)p = g_free_pages;
    g_free_pages = p;
    hw_spin_unlock(&g_mm_lock);
}

static void *hw_alloc_page(void) {
    uint8_t *p = 0;
    uint32_t i;

    hw_spin_lock(&g_mm_lock);
    if (g_free_pages) {
        p = (uint8_t *)g_free_pages;
        g_free_pages = *(void **)g_free_pages;
    }
    if (!p && g_hw_pmm_ready) {
        p = (uint8_t *)vibeos_pmm_alloc_page(&g_hw_pmm);
        if (p && ((uint64_t)(uintptr_t)p + 4096ull) > VIBEOS_HW_IDENTITY_LIMIT) {
            p = 0; /* outside the identity map: unusable this early */
        }
        if (p) {
            g_hw_pmm_pages_used++;
        }
    }
    if (!p) {
        if (g_pool_next >= VIBEOS_HW_POOL_PAGES) {
            hw_spin_unlock(&g_mm_lock);
            return 0;
        }
        p = g_page_pool[g_pool_next++];
    }
    hw_spin_unlock(&g_mm_lock);
    for (i = 0; i < 4096u; i++) {
        p[i] = 0;
    }
    return p;
}

/* Bring the physical memory manager online from the firmware memory map. */
static void hw_pmm_bringup(const vibeos_boot_info_t *boot_info) {
    if (!boot_info) {
        vibeos_x86_64_serial_puts("[HW] no boot_info: using the static page pool\n");
        return;
    }
    if (vibeos_pmm_init_from_boot_info(&g_hw_pmm, boot_info, 4096) != 0) {
        vibeos_x86_64_serial_puts("[HW] PMM init failed: falling back to the static page pool\n");
        return;
    }
    /* Nothing of the kernel's may live where a process can shadow it. See
     * VIBEOS_HW_LOW_USER_BASE and vibeos_pmm_reserve for why this is a
     * correctness requirement and not a tidiness one. */
    if (vibeos_pmm_reserve(&g_hw_pmm, (uintptr_t)VIBEOS_HW_LOW_USER_BASE,
                           (size_t)(VIBEOS_HW_LOW_USER_LIMIT - VIBEOS_HW_LOW_USER_BASE)) != 0) {
        g_hw_pmm_ready = 0;
        vibeos_x86_64_serial_puts("[HW] PMM cannot reserve the low user window; "
                                 "falling back to the static page pool\n");
        return;
    }
    g_hw_pmm_ready = 1;

    /* One byte per frame of the allocator's region, taken from that region.
     * Without it fork cannot share pages safely, so failing to allocate it is
     * not fatal - it only means fork keeps copying eagerly, which is slow
     * rather than wrong. */
    {
        uint64_t bytes = (uint64_t)vibeos_pmm_remaining(&g_hw_pmm) / 4096ull;
        uint64_t pages = (bytes + 4095ull) / 4096ull;
        void *table = pages ? vibeos_pmm_alloc_pages(&g_hw_pmm, (size_t)pages) : 0;
        if (table && ((uint64_t)(uintptr_t)table + pages * 4096ull) <= VIBEOS_HW_IDENTITY_LIMIT) {
            uint64_t i;
            g_frame_refs = (uint8_t *)table;
            /* The base is the start of the allocator's whole region, not the
             * end of this table. Indexing from the end leaves every frame
             * handed out earlier untracked - and untracked means "sole owner",
             * so the first process to exit frees pages another one is still
             * running from. That is not a leak, it is corruption, and it
             * presented as a shell that started and then silently stopped. */
            g_frame_refs_base = (uint64_t)g_hw_pmm.base;
            g_frame_refs_count = bytes;
            for (i = 0; i < bytes; i++) {
                g_frame_refs[i] = 0;
            }
            vibeos_x86_64_serial_puts("[MM] copy-on-write fork enabled\n");
        } else {
            vibeos_x86_64_serial_puts("[MM] no frame reference table; fork copies eagerly\n");
        }
    }

    /* Now that pages exist, take a contiguous staging area large enough for a
     * real program. Failing is not fatal: the small static buffer still works,
     * and small programs still run - they simply cannot be large ones. */
    {
        void *stage = vibeos_pmm_alloc_pages(&g_hw_pmm,
                                             VIBEOS_HW_EXEC_STAGE_BYTES / 4096u);
        if (stage && ((uint64_t)(uintptr_t)stage + VIBEOS_HW_EXEC_STAGE_BYTES)
                <= VIBEOS_HW_IDENTITY_LIMIT) {
            g_exec_elf = (uint8_t *)stage;
            g_exec_elf_cap = VIBEOS_HW_EXEC_STAGE_BYTES;
            vibeos_x86_64_serial_puts("[HW] exec staging buffer: 4 MiB\n");
        } else {
            vibeos_x86_64_serial_puts("[HW] exec staging buffer stays at 64 KiB; "
                                     "large programs will not load\n");
        }
    }

    vibeos_x86_64_serial_puts("[HW] PMM online, free bytes=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)vibeos_pmm_remaining(&g_hw_pmm));
    vibeos_x86_64_serial_puts("\n");
}

/* Walk the four levels (creating missing tables from the pool) and install a
 * 4 KiB mapping. Intermediate entries carry US so the leaf's US decides access.
 * Identity-mapped physical addresses double as the kernel's view of the tables. */
static int hw_map_page(vibeos_hw_aspace_t *as, uint64_t va, uint64_t pa, uint64_t leaf_flags) {
    static const uint32_t shifts[3] = {39u, 30u, 21u}; /* PML4, PDPT, PD */
    uint64_t *tbl = as->pml4;
    uint32_t level;

    for (level = 0; level < 3u; level++) {
        uint32_t idx = (uint32_t)((va >> shifts[level]) & 0x1FFu);
        if ((tbl[idx] & PTE_PRESENT) == 0) {
            void *page = hw_alloc_page();
            if (!page) {
                return -1;
            }
            tbl[idx] = (uint64_t)(uintptr_t)page | PTE_PRESENT | PTE_WRITE | PTE_USER;
        }
        tbl = (uint64_t *)(uintptr_t)(tbl[idx] & 0x000FFFFFFFFFF000ull);
    }
    tbl[(va >> 12) & 0x1FFu] = (pa & 0x000FFFFFFFFFF000ull) | leaf_flags;
    return 0;
}

/* Map one user page inside the kernel's identity region.
 *
 * The tables covering the first GiB are global and shared by every address
 * space, and the identity map uses 2 MiB pages, so there is nowhere to put a
 * 4 KiB user entry without first making private copies. This walks down and
 * un-shares exactly as much as it has to:
 *
 *   - PML4 slot 0 still points at the global PDPT: copy it.
 *   - The PDPT entry still points at a global PD: copy it.
 *   - The PD entry is a 2 MiB leaf: split it into a page table whose 512
 *     entries reproduce the same identity mapping at 4 KiB granularity, so the
 *     kernel's view of that region is unchanged, and only then overwrite the
 *     one entry the program wants.
 *
 * The upper levels get PTE_USER because on x86-64 access is the AND of the US
 * bits along the path; the leaf decides. Every identity entry left behind has
 * US clear, so ring 3 still cannot reach any of it. */
static int hw_map_low_user_page(vibeos_hw_aspace_t *as, uint64_t va, uint64_t pa,
                                uint64_t leaf_flags) {
    uint64_t *pdpt, *pd, *pt;
    uint32_t gi = (uint32_t)((va >> 30) & 0x1FFu);
    uint32_t pdi = (uint32_t)((va >> 21) & 0x1FFu);
    uint32_t pti = (uint32_t)((va >> 12) & 0x1FFu);

    if (va >= VIBEOS_HW_IDENTITY_LIMIT) {
        return -1;   /* not this function's business */
    }

    /* Level 1: the PDPT for the low 512 GiB. */
    pdpt = (uint64_t *)(uintptr_t)(as->pml4[0] & 0x000FFFFFFFFFF000ull);
    if (pdpt == &g_pdpt[0]) {
        uint64_t *priv = (uint64_t *)hw_alloc_page();
        uint32_t i;
        if (!priv) {
            return -1;
        }
        for (i = 0; i < 512u; i++) {
            priv[i] = g_pdpt[i];
        }
        as->pml4[0] = (uint64_t)(uintptr_t)priv | PTE_PRESENT | PTE_WRITE | PTE_USER;
        pdpt = priv;
    }

    /* Level 2: the page directory for this GiB. */
    pd = (uint64_t *)(uintptr_t)(pdpt[gi] & 0x000FFFFFFFFFF000ull);
    if (gi < VIBEOS_HW_IDENTITY_GIB && pd == &g_pd[gi][0]) {
        uint64_t *priv = (uint64_t *)hw_alloc_page();
        uint32_t i;
        if (!priv) {
            return -1;
        }
        for (i = 0; i < 512u; i++) {
            priv[i] = g_pd[gi][i];
        }
        pdpt[gi] = (uint64_t)(uintptr_t)priv | PTE_PRESENT | PTE_WRITE | PTE_USER;
        pd = priv;
    }

    /* Level 3: split the 2 MiB leaf into a real page table, preserving the
     * identity mapping it stood for. */
    if ((pd[pdi] & PTE_PRESENT) == 0 || (pd[pdi] & PTE_PS) != 0) {
        uint64_t region = (uint64_t)gi << 30 | (uint64_t)pdi << 21;
        uint64_t *priv = (uint64_t *)hw_alloc_page();
        uint32_t i;
        if (!priv) {
            return -1;
        }
        for (i = 0; i < 512u; i++) {
            /* Same physical address, same supervisor-only access, finer
             * granularity. No PTE_USER: this is still the kernel's memory. */
            priv[i] = (region + (uint64_t)i * 4096ull) | PTE_PRESENT | PTE_WRITE;
        }
        pd[pdi] = (uint64_t)(uintptr_t)priv | PTE_PRESENT | PTE_WRITE | PTE_USER;
        pt = priv;
    } else {
        pt = (uint64_t *)(uintptr_t)(pd[pdi] & 0x000FFFFFFFFFF000ull);
    }

    pt[pti] = (pa & 0x000FFFFFFFFFF000ull) | leaf_flags;
    return 0;
}

/* A fresh address space: a private PML4 that shares the supervisor-only kernel
 * identity mapping, so ring 0 (syscalls, interrupts) keeps working while running
 * on a process's CR3, but ring 3 cannot touch kernel memory. */
static int hw_aspace_create(vibeos_hw_aspace_t *as) {
    as->pml4 = (uint64_t *)hw_alloc_page();
    if (!as->pml4) {
        return -1;
    }
    as->pml4[0] = (uint64_t)(uintptr_t)&g_pdpt[0] | PTE_PRESENT | PTE_WRITE; /* no PTE_USER */
    return 0;
}

/* Free an address space: the user subtree (PML4 slot 1 - pages and the tables
 * that map them) and the PML4 itself. The shared kernel identity map (slot 0)
 * is static and never freed. Must not be called while this CR3 is active. */
/* Who last tore an address space down, and which one.
 *
 * proc.as.pml4 going to zero is the one unexplained step left in the wedge,
 * and this function is its only writer. Three readings of the call sites have
 * been wrong, so the function records what it did rather than being reasoned
 * about. */
static const char *g_last_destroy_why;
static uint64_t g_last_destroy_pml4;
static uint32_t g_last_destroy_cpu;

static void hw_aspace_destroy_why(vibeos_hw_aspace_t *as, const char *why) {
    uint64_t *pdpt;
    uint32_t i, j, k;

    if (!as->pml4) {
        return;
    }
    if (as->pml4[1] & PTE_PRESENT) {
        pdpt = (uint64_t *)(uintptr_t)(as->pml4[1] & 0x000FFFFFFFFFF000ull);
        for (i = 0; i < 512u; i++) {
            uint64_t *pd;
            if ((pdpt[i] & PTE_PRESENT) == 0) {
                continue;
            }
            pd = (uint64_t *)(uintptr_t)(pdpt[i] & 0x000FFFFFFFFFF000ull);
            for (j = 0; j < 512u; j++) {
                uint64_t *pt;
                if ((pd[j] & PTE_PRESENT) == 0) {
                    continue;
                }
                pt = (uint64_t *)(uintptr_t)(pd[j] & 0x000FFFFFFFFFF000ull);
                for (k = 0; k < 512u; k++) {
                    if (pt[k] & PTE_PRESENT) {
                        uint64_t phys = pt[k] & 0x000FFFFFFFFFF000ull;
                        /* Only the last owner frees it: after fork, several
                         * address spaces map the same frame. */
                        if (frame_ref_dec(phys)) {
                            hw_free_page((void *)(uintptr_t)phys);
                        }
                    }
                }
                hw_free_page(pt);
            }
            hw_free_page(pd);
        }
        hw_free_page(pdpt);
    }

    /* Slot 0 is the kernel's identity map and is shared by every address
     * space - unless this process had pages down there, in which case parts of
     * it were copied. Free exactly the copies: a table that is still one of
     * the globals belongs to everyone and must be left alone, and inside a
     * split page table only the entries carrying PTE_USER own a frame; the
     * rest are identity entries that were never allocated. */
    if (as->pml4[0] & PTE_PRESENT) {
        uint64_t *lowpdpt = (uint64_t *)(uintptr_t)(as->pml4[0] & 0x000FFFFFFFFFF000ull);
        if (lowpdpt != &g_pdpt[0]) {
            for (i = 0; i < 512u; i++) {
                uint64_t *pd;
                if ((lowpdpt[i] & PTE_PRESENT) == 0) {
                    continue;
                }
                pd = (uint64_t *)(uintptr_t)(lowpdpt[i] & 0x000FFFFFFFFFF000ull);
                if (i < VIBEOS_HW_IDENTITY_GIB && pd == &g_pd[i][0]) {
                    continue;   /* shared */
                }
                for (j = 0; j < 512u; j++) {
                    uint64_t *pt;
                    if ((pd[j] & PTE_PRESENT) == 0 || (pd[j] & PTE_PS) != 0) {
                        continue;   /* absent, or an untouched 2 MiB identity leaf */
                    }
                    pt = (uint64_t *)(uintptr_t)(pd[j] & 0x000FFFFFFFFFF000ull);
                    for (k = 0; k < 512u; k++) {
                        if ((pt[k] & PTE_PRESENT) && (pt[k] & PTE_USER)) {
                            uint64_t phys = pt[k] & 0x000FFFFFFFFFF000ull;
                            if (frame_ref_dec(phys)) {
                                hw_free_page((void *)(uintptr_t)phys);
                            }
                        }
                    }
                    hw_free_page(pt);
                }
                hw_free_page(pd);
            }
            hw_free_page(lowpdpt);
        }
    }

    g_last_destroy_why = why;
    g_last_destroy_pml4 = (uint64_t)(uintptr_t)as->pml4;
    g_last_destroy_cpu = hw_this_cpu()->index;
    hw_free_page(as->pml4);
    as->pml4 = 0;
}

#define hw_aspace_destroy(as) hw_aspace_destroy_why((as), __func__)

/* Build the kernel's page tables: identity-map the first N GiB with 2 MiB
 * supervisor pages (US=0) and switch CR3 to them. */
static void hw_enable_paging(void) {
    uint32_t g, e;

    vibeos_x86_64_serial_puts("[HW] building kernel page tables (identity 4GiB, supervisor-only)\n");
    for (g = 0; g < VIBEOS_HW_IDENTITY_GIB; g++) {
        for (e = 0; e < 512u; e++) {
            uint64_t phys = ((uint64_t)g * 0x40000000ull) + ((uint64_t)e * 0x200000ull);
            g_pd[g][e] = phys | PTE_PRESENT | PTE_WRITE | PTE_PS;
        }
        g_pdpt[g] = (uint64_t)(uintptr_t)&g_pd[g][0] | PTE_PRESENT | PTE_WRITE;
    }
    g_pml4[0] = (uint64_t)(uintptr_t)&g_pdpt[0] | PTE_PRESENT | PTE_WRITE;

    hw_write_cr3((uint64_t)(uintptr_t)&g_pml4[0]);

    vibeos_x86_64_serial_puts("[HW] CR3 loaded with kernel-owned tables: 0x");
    vibeos_x86_64_serial_print_hex(hw_read_cr3());
    vibeos_x86_64_serial_puts("\n[HW] PAGING_OK (kernel tables, user pages isolated)\n");
}

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
/* Thread-local storage base. A C runtime reaches its own thread state through
 * %fs on x86-64 - errno, the stack guard, locale - so this MSR is per task,
 * not per CPU, and has to be reloaded on every context switch. */
#define MSR_FS_BASE 0xC0000100u

extern void vibeos_x86_64_syscall_entry(void); /* trampoline in isr.S */

static uint64_t hw_rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void hw_wrmsr(uint32_t msr, uint64_t value) {
    __asm__ __volatile__("wrmsr"
                         : : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

/* Enable the `syscall`/`sysret` fast path. STAR selects the CS/SS pairs:
 * SYSCALL loads kernel CS=0x08 (SS=0x10); SYSRET loads user CS=0x20|3 and
 * SS=0x18|3 from base 0x10 - which matches the data-then-code user GDT order. */
static void hw_enable_syscall(void) {
    hw_wrmsr(MSR_EFER, hw_rdmsr(MSR_EFER) | 1ull);                 /* SCE */
    hw_wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    hw_wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)vibeos_x86_64_syscall_entry);
    hw_wrmsr(MSR_SFMASK, 0x200ull);                                /* clear IF on entry */
    if (hw_this_cpu()->index == 0u) {
        vibeos_x86_64_serial_puts("[HW] syscall/sysret enabled (LSTAR set)\n");
    }
}

/* ---- Process creation (ELF -> private address space) --------------------- */

/* User address-space layout (all inside the process's own PML4 slot):
 *   [USER_BASE ..]            program image (linked address)
 *   [.. USER_STACK_TOP]       stack (grows down)
 *   [USER_HEAP_BASE ..]       brk heap (grows up)
 *   [USER_MMAP_BASE ..]       anonymous mmap arena (grows up)
 */
#define VIBEOS_HW_USER_HEAP_BASE (VIBEOS_HW_USER_BASE + 0x00800000ull) /* +8 MiB  */
#define VIBEOS_HW_USER_MMAP_BASE (VIBEOS_HW_USER_BASE + 0x04000000ull) /* +64 MiB */

typedef struct {
    vibeos_hw_aspace_t as;
    uint64_t entry;
    uint64_t brk_cur;   /* current program break            */
    uint64_t mmap_cur;  /* next free anonymous mmap address  */
    uint64_t user_sp;   /* entry rsp, atop the startup block */
    /* What execve was given. A program that wants to find itself reads
     * /proc/self/exe, and answering from the real path is the difference
     * between a correct answer and a plausible one. */
    char exe_path[64];
} hw_proc_t;

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
static void hw_seed_at_random(uint8_t out[16]) {
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
static int hw_proc_create(hw_proc_t *p, const unsigned char *elf, uint64_t len,
                          const char *const *argv, const char *const *envp) {
    vibeos_elf_image_t img;
    vibeos_elf_stack_desc_t sd;
    uint8_t at_random[16];
    uint8_t *top_page = 0;
    uint64_t va;
    uint32_t i;

    if (hw_aspace_create(&p->as) != 0) {
        return -1;
    }
    /* The portable parser validates the file and describes it; this loop just
     * places it. Working a page at a time is what makes a page shared between
     * two segments come out right - allocated once, carrying the permissions
     * of both, holding the bytes of both. */
    /* Two windows are allowed: the one VibeOS programs are linked into, and
     * the low one a Linux executable is linked into. Parsing with the widest
     * bounds and then checking which window the image landed in is what keeps
     * a crafted file from asking to be placed between them - on top of the
     * kernel, for instance, which is linked at 64 MiB. */
    if (vibeos_elf_parse(elf, len, VIBEOS_HW_LOW_USER_BASE,
                         VIBEOS_HW_USER_STACK_TOP, &img) != VIBEOS_ELF_OK) {
        return -1;
    }
    if (!(img.min_vaddr >= VIBEOS_HW_USER_BASE) &&
        !(img.min_vaddr >= VIBEOS_HW_LOW_USER_BASE &&
          img.end_vaddr <= VIBEOS_HW_LOW_USER_LIMIT)) {
        return -1;
    }
    for (va = img.min_vaddr; va < img.end_vaddr; va += 4096ull) {
        uint32_t flags = vibeos_elf_page_flags(&img, va);
        uint64_t leaf = PTE_PRESENT | PTE_USER;
        uint8_t *page;

        if (flags == 0u) {
            continue;   /* a hole between segments stays unmapped */
        }
        if (flags & VIBEOS_ELF_W) {
            leaf |= PTE_WRITE;
        }
        page = (uint8_t *)hw_alloc_page();
        if (!page) {
            return -1;
        }
        vibeos_elf_fill_page(&img, elf, va, page);
        if (va < VIBEOS_HW_IDENTITY_LIMIT) {
            if (hw_map_low_user_page(&p->as, va, (uint64_t)(uintptr_t)page, leaf) != 0) {
                return -1;
            }
        } else if (hw_map_page(&p->as, va, (uint64_t)(uintptr_t)page, leaf) != 0) {
            return -1;
        }
    }
    p->entry = img.entry;
    for (i = 0; i < VIBEOS_HW_USER_STACK_PAGES; i++) {
        void *page = hw_alloc_page();
        /* Named apart from the image-loading loop's `va` above: two different
         * addresses in one function should not share a name. */
        uint64_t stack_va = VIBEOS_HW_USER_STACK_TOP - ((uint64_t)(i + 1u) * 4096ull);
        if (!page || hw_map_page(&p->as, stack_va, (uint64_t)(uintptr_t)page,
                                 PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
            return -1;
        }
        if (i == 0u) {
            top_page = (uint8_t *)page;
        }
    }

    /* Fill the topmost stack page with the startup block. The page is still
     * identity-mapped for the kernel, so it is written here through its
     * physical address while the builder computes every pointer it stores in
     * terms of the user virtual address the program will see. */
    for (i = 0; i < sizeof(sd); i++) {
        ((uint8_t *)(void *)&sd)[i] = 0;
    }
    sd.argv = argv;
    sd.envp = envp;
    sd.entry = img.entry;
    sd.phdr_vaddr = img.phdr_vaddr;
    sd.phnum = img.phnum;
    sd.phentsize = img.phentsize;
    hw_seed_at_random(at_random);
    sd.random16 = at_random;
    p->user_sp = vibeos_elf_build_stack(top_page, 4096ull,
                                        VIBEOS_HW_USER_STACK_TOP, &sd);
    if (p->user_sp == 0) {
        return -1;   /* arguments too large for the stack we mapped */
    }
    p->brk_cur = VIBEOS_HW_USER_HEAP_BASE;
    p->mmap_cur = VIBEOS_HW_USER_MMAP_BASE;
    return 0;
}

/* Map `pages` fresh zeroed user pages at `va` in an address space. */
static int hw_map_user_pages(vibeos_hw_aspace_t *as, uint64_t va, uint64_t pages) {
    uint64_t i;
    for (i = 0; i < pages; i++) {
        void *page = hw_alloc_page();
        if (!page || hw_map_page(as, va + i * 4096ull, (uint64_t)(uintptr_t)page,
                                 PTE_PRESENT | PTE_WRITE | PTE_USER) != 0) {
            return -1;
        }
    }
    return 0;
}

static uint64_t hw_proc_cr3(const hw_proc_t *p) {
    return (uint64_t)(uintptr_t)p->as.pml4;
}

/* ---- Task table + preemptive scheduler ---------------------------------- */

#define VIBEOS_HW_MAX_TASKS 24  /* kernel + user processes + one idle task per CPU */

/* Open-file table entry. Reads stream straight off the filesystem; writes are
 * buffered and committed to disk on close (the FAT writer stores whole files). */
#define VIBEOS_HW_MAX_FDS 4
#define VIBEOS_HW_MAX_DIR_ENTRIES 4096u
#define VIBEOS_HW_WBUF 512

/* Pipes.
 *
 * A pipe is a ring buffer with two ends, and what makes it a pipe rather than
 * a buffer is what happens at the edges: a reader with nothing to read waits
 * for a writer, a writer with no room waits for a reader, and a reader whose
 * writers have all closed gets end of file rather than waiting forever. Those
 * three rules are the entire difference between `ls | wc -l` printing a number
 * and hanging.
 *
 * The ends are counted, not flagged, because a descriptor can be duplicated
 * and inherited: `ls | wc` gives the write end to a child, and the parent must
 * close its own copy or the reader never sees end of file. That is the classic
 * way a shell pipeline hangs, and it is a refcount bug, not a pipe bug. */
#define VIBEOS_HW_MAX_PIPES 8
#define VIBEOS_HW_PIPE_BYTES 4096u

typedef struct {
    int used;
    uint32_t readers;
    uint32_t writers;
    uint32_t head;      /* next byte to read  */
    uint32_t tail;      /* next byte to write */
    uint32_t count;     /* bytes currently held */
    uint8_t buf[VIBEOS_HW_PIPE_BYTES];
} hw_pipe_t;

static hw_pipe_t g_pipes[VIBEOS_HW_MAX_PIPES];
static hw_lock_t g_pipe_lock;

typedef struct {
    int used;
    int writable;
    int dirty;
    /* Index into g_pipes, or -1. A descriptor is a pipe end when this is set;
     * `writable` then says which end. */
    int pipe;
    uint32_t cluster;
    uint32_t size;
    uint32_t pos;
    int net_sock;         /* index into the TCP/IP stack, or -1 for a file */
    uint32_t dir_index;   /* for getdents64 on a directory fd */
    /* Whether this descriptor names a directory. Determined when it is opened
     * rather than guessed later: opendir() opens the path and then fstats the
     * descriptor, and a descriptor that claims to be a regular file is refused
     * with ENOTDIR no matter what stat said about the path a moment earlier. */
    int isdir;
    char name[24];
    uint8_t wbuf[VIBEOS_HW_WBUF];
    uint32_t wlen;
} hw_fd_t;

/* Defined with the pipe code below; the task-exit path above needs it. */
static void hw_pipe_release(hw_fd_t *f);

/* Signals 1..64; index 0 is unused so the numbering matches Linux. */
#define VIBEOS_HW_NSIG 65

#define SIG_DFL_ADDR 0ull
#define SIG_IGN_ADDR 1ull

#define VIBEOS_SIGHUP   1u
#define VIBEOS_SIGINT   2u
#define VIBEOS_SIGQUIT  3u
#define VIBEOS_SIGILL   4u
#define VIBEOS_SIGABRT  6u
#define VIBEOS_SIGFPE   8u
#define VIBEOS_SIGKILL  9u
#define VIBEOS_SIGSEGV 11u
#define VIBEOS_SIGPIPE 13u
#define VIBEOS_SIGALRM 14u
#define VIBEOS_SIGTERM 15u
#define VIBEOS_SIGCHLD 17u
#define VIBEOS_SIGCONT 18u
#define VIBEOS_SIGSTOP 19u
#define VIBEOS_SIGWINCH 28u

/* SA_RESTORER: the handler entry carries the address the handler returns to. */
#define VIBEOS_SA_RESTORER 0x04000000u

/* Where a task was last handled, for the guard in hw_task_load_cpu_state.
 *
 * Three readings of this code have already been wrong about how an exited task
 * gets scheduled again, so the code stops being the source: each task records
 * the last place its cr3 was written, the last place it was made runnable, and
 * the last place its address space was destroyed. Static strings, one store
 * each - the cost is a pointer write on paths that already do far more, and
 * what it buys is the difference between a theory and a name. */
#define HW_TASK_MARK(idx, field, where) (g_tasks[idx].field = (where))

enum {
    HW_TASK_FREE = 0,
    HW_TASK_READY = 1,
    HW_TASK_RUNNING = 2,
    HW_TASK_ZOMBIE = 3,
    HW_TASK_BLOCKED = 4,  /* waiting for an event; not schedulable until woken */
    HW_TASK_RESERVED = 5  /* slot claimed by a creator that is still filling it */
};

extern void vibeos_x86_64_task_enter(vibeos_x86_64_isr_frame_t *task);
extern const unsigned char vibeos_user_task_elf[];
extern const unsigned long vibeos_user_task_elf_len;

typedef struct {
    vibeos_x86_64_isr_frame_t ctx;
    hw_proc_t proc;
    uint64_t cr3;
    const char *cr3_set_by;      /* diagnostics only; see HW_TASK_MARK */
    const char *ready_by;
    const char *aspace_killed_by;
    uint32_t alloc_seq;          /* which tenancy of this slot this is */
    uint64_t exit_code;
    /* Non-zero when this task was killed by a signal rather than exiting.
     * wait() encodes the two cases differently, and a parent that cannot tell
     * them apart reads a signal death as an ordinary exit with a large status
     * - which is how a crashed child looks like a successful one. */
    uint32_t exit_signal;
    uint64_t kstack_top;  /* private ring-0 stack: lets a task block in a syscall */
    uint32_t pid;
    uint32_t ppid;
    /* Written from interrupt/syscall context (preemption, task exit) and read
     * by the kernel task, so it must not be cached across a wait loop. */
    volatile int state;
    /* Set while some CPU is executing this task, cleared only once its
     * context has been saved. A waker on another core can flip state to
     * READY while the task is still running here; without this flag a
     * third core would pick it up and two CPUs would run one task,
     * sharing its kernel stack. */
    volatile int on_cpu;
    int is_user;
    int is_idle;      /* per-CPU idle task: only run when nothing else is ready */
    int wait_input;   /* blocked in read() on stdin */
    /* Set by prctl(PR_SET_NAME); reported back by PR_GET_NAME. */
    char comm[16];
    /* Signals.
     *
     * pending is a bitmask of signals raised but not yet delivered; blocked is
     * the mask the process asked to defer. Delivery happens on the way back to
     * user space, never at the point the signal is raised - raising can happen
     * from an interrupt or from another CPU, and building a signal frame on a
     * stack that is not currently in use would corrupt it.
     *
     * handler[] holds one user address per signal, plus the flags and the
     * restorer trampoline the C library supplied. SIG_DFL and SIG_IGN are
     * stored as they arrive so the default action is a property of the entry
     * rather than of a separate table that could disagree with it. */
    uint64_t sig_pending;
    uint64_t sig_blocked;
    uint64_t sig_handler[VIBEOS_HW_NSIG];
    uint64_t sig_restorer[VIBEOS_HW_NSIG];
    uint64_t sig_flags[VIBEOS_HW_NSIG];
    uint64_t sig_mask[VIBEOS_HW_NSIG];
    /* %fs base for this task, set by arch_prctl(ARCH_SET_FS). Restored on
     * every switch: leaving the previous task's value loaded would let one
     * program read and write another's thread-local state. */
    uint64_t fs_base;
    uint64_t kstack_base;  /* for reclamation on exit */
    uint32_t kstack_pages;
    hw_fd_t fds[VIBEOS_HW_MAX_FDS];
    /* What descriptors 0, 1 and 2 currently mean. Unused entries mean the
     * console, which is where they point when nothing has redirected them.
     * Kept apart from fds[] because the console is not a table entry and a
     * shell redirects the standard three far more often than anything else. */
    hw_fd_t std_redirect[3];
} hw_task_t;

/* Point the CPU at a task's ring-0 stack: the TSS one is used when ring 3 is
 * interrupted, the syscall one when it issues `syscall`. */
static void hw_set_kernel_stack(uint64_t top) {
    hw_cpu_t *cpu = hw_this_cpu();
    cpu->tss.rsp0 = top;
    cpu->syscall_kstack_top = top;
}


/* Two contiguous pages when the PMM can give them, one otherwise. Reports the
 * base and page count so the stack can be reclaimed when the task exits. */
static uint64_t hw_alloc_kstack(uint64_t *out_base, uint32_t *out_pages) {
    uint8_t *p = 0;
    uint64_t size = 8192ull;
    uint32_t pages = 2u;

    if (g_hw_pmm_ready) {
        hw_spin_lock(&g_mm_lock);
        p = (uint8_t *)vibeos_pmm_alloc_pages(&g_hw_pmm, 2);
        hw_spin_unlock(&g_mm_lock);
        if (p && ((uint64_t)(uintptr_t)p + size) > VIBEOS_HW_IDENTITY_LIMIT) {
            p = 0;
        }
    }
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

/* Free a kernel stack from its address and length, not from the task.
 *
 * Deliberately not taking a task pointer: the only caller reaps a slot, and a
 * helper that writes back into the task invites doing so after the slot has
 * been published as reusable - which is exactly the bug this shape prevents.
 * The caller takes what it needs under the lock and frees afterwards. */
static void hw_free_kstack_pages(uint64_t base, uint32_t pages) {
    uint32_t i;
    for (i = 0; i < pages; i++) {
        hw_free_page((void *)(uintptr_t)(base + (uint64_t)i * 4096ull));
    }
}

static hw_task_t g_tasks[VIBEOS_HW_MAX_TASKS];
static uint32_t g_alloc_seq;
static int g_sched_running;
static uint32_t g_next_pid = 1;

/* Claim a free slot atomically: two cores can fork at the same time, so the
 * slot is marked RESERVED (never schedulable, never reapable) until the caller
 * has finished filling it in. */
static int hw_task_alloc(void) {
    int i;
    hw_spin_lock(&g_sched_lock);
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (g_tasks[i].state == HW_TASK_FREE) {
            int fi;
            /* Slots are recycled, so the descriptor tables hold whatever the
             * previous occupant left. Clearing them here covers every way a
             * task comes into existence - spawn, fork, the idle tasks - which
             * is the only way to be sure none of them starts out believing a
             * stale entry. An uninitialised redirection sends a write into a
             * pipe that does not exist, and the task waits there forever. */
            for (fi = 0; fi < VIBEOS_HW_MAX_FDS; fi++) {
                g_tasks[i].fds[fi].used = 0;
                g_tasks[i].fds[fi].pipe = -1;
                g_tasks[i].fds[fi].net_sock = -1;
            }
            for (fi = 0; fi < 3; fi++) {
                g_tasks[i].std_redirect[fi].used = 0;
                g_tasks[i].std_redirect[fi].pipe = -1;
                g_tasks[i].std_redirect[fi].net_sock = -1;
            }
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
            g_tasks[i].alloc_seq = (uint32_t)__sync_add_and_fetch(&g_alloc_seq, 1u);
            g_tasks[i].proc.as.pml4 = 0;
            g_tasks[i].state = HW_TASK_RESERVED;
            hw_spin_unlock(&g_sched_lock);
            return i;
        }
    }
    hw_spin_unlock(&g_sched_lock);
    return -1;
}

/* Give a reserved slot back after a failed creation. */
static void hw_task_release(int i) {
    if (i >= 0) {
        g_tasks[i].state = HW_TASK_FREE;
    }
}

/* Next task to run on `cpu`, round robin. Only READY tasks are candidates: a
 * RUNNING one is owned by some core (possibly another), so on SMP it must never
 * be picked twice. Idle tasks are skipped unless nothing else is available.
 * Returns -1 when the caller should simply keep running what it has. */
static int hw_pick_next(hw_cpu_t *cpu) {
    int n;
    int cur = cpu->current_task;
    int start = (cur < 0) ? 0 : cur;

    for (n = 1; n <= VIBEOS_HW_MAX_TASKS; n++) {
        int i = (start + n) % VIBEOS_HW_MAX_TASKS;
        if (g_tasks[i].state == HW_TASK_READY && !g_tasks[i].is_idle &&
            !g_tasks[i].on_cpu) {
            return i;
        }
    }
    if (cur >= 0 && g_tasks[cur].state == HW_TASK_RUNNING) {
        return -1;  /* still runnable and nothing better: no switch */
    }
    return cpu->idle_task;  /* current task blocked/died: fall back to idle */
}

/* Control-C from the console.
 *
 * A real system sends this to the foreground process group of the controlling
 * terminal. There are no sessions or process groups here, so the target is the
 * most recently created live user task - which is the one the console is
 * talking to in every arrangement this system can currently produce. That is a
 * limitation of the model, not an approximation of the signal: the signal
 * itself is delivered exactly as any other. */
void vibeos_x86_64_console_interrupt(void) {
    int i, newest = -1;
    uint32_t best = 0;

    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (!g_tasks[i].is_user || g_tasks[i].state == HW_TASK_FREE ||
            g_tasks[i].state == HW_TASK_ZOMBIE) {
            continue;
        }
        if (newest < 0 || g_tasks[i].pid > best) {
            best = g_tasks[i].pid;
            newest = i;
        }
    }
    if (newest >= 0) {
        (void)hw_signal_raise(newest, VIBEOS_SIGINT);
    }
}

/* Wake every task blocked in read() on stdin (called from the keyboard IRQ). */
static void hw_keyboard_wake(void) {
    int i;
    hw_spin_lock(&g_sched_lock);
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (g_tasks[i].state == HW_TASK_BLOCKED && g_tasks[i].wait_input) {
            g_tasks[i].wait_input = 0;
            g_tasks[i].state = HW_TASK_READY;
            HW_TASK_MARK(i, ready_by, "keyboard_wake");
        }
    }
    hw_spin_unlock(&g_sched_lock);
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
static void hw_task_load_cpu_state(int idx) {
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
            vibeos_x86_64_serial_print_hex(g_tasks[idx].pid);
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
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].state);
            vibeos_x86_64_serial_puts(" user=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].is_user);
            vibeos_x86_64_serial_puts(" idle=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].is_idle);
            vibeos_x86_64_serial_puts(" on_cpu=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].on_cpu);
            vibeos_x86_64_serial_puts(" ppid=0x");
            vibeos_x86_64_serial_print_hex((uint64_t)g_tasks[idx].ppid);
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
                   (uint64_t)g_tasks[idx].pid,
                   "scheduler asked to install a freed address space");
            hw_panic("address space freed while still schedulable");
        }
    }
    hw_write_cr3(g_tasks[idx].cr3);
    hw_set_kernel_stack(g_tasks[idx].kstack_top);
    if (g_tasks[idx].is_user) {
        hw_wrmsr(MSR_FS_BASE, g_tasks[idx].fs_base);
    }
}

/* Timer-driven preemption: save the interrupted task, pick the next runnable
 * one, and resume it by rewriting the live IRQ frame and switching CR3. Runs
 * for the life of the system - there is no "demo over" exit.
 *
 * On SMP every core's local-APIC timer calls this independently; the run queue
 * is shared, so the pick-and-claim step is done under the scheduler lock. */
static void hw_schedule(vibeos_x86_64_isr_frame_t *frame) {
    hw_cpu_t *cpu = hw_this_cpu();
    int cur, next;

    if (!g_sched_running) {
        return;
    }

    hw_spin_lock(&g_sched_lock);
    cur = cpu->current_task;
    next = hw_pick_next(cpu);
    if (next < 0 || next == cur) {
        hw_spin_unlock(&g_sched_lock);
        return; /* nothing else runnable: keep running the current task */
    }
    if (cur >= 0) {
        g_tasks[cur].ctx = *frame;
        if (g_tasks[cur].state == HW_TASK_RUNNING) {
            g_tasks[cur].state = HW_TASK_READY;
            HW_TASK_MARK(cur, ready_by, "preempted");
        }
        /* Only now is it safe for another core to take it: its context is
         * saved and this core is about to stop touching it. */
        g_tasks[cur].on_cpu = 0;
    }
    cpu->current_task = next;
    g_tasks[next].state = HW_TASK_RUNNING;
    g_tasks[next].on_cpu = 1;
    *frame = g_tasks[next].ctx;
    hw_spin_unlock(&g_sched_lock);

    hw_task_load_cpu_state(next);
}

static void hw_task_init_user_ctx(vibeos_x86_64_isr_frame_t *c, uint64_t entry, uint64_t sp) {
    uint32_t k;
    for (k = 0; k < (uint32_t)sizeof(*c); k++) {
        ((uint8_t *)(void *)c)[k] = 0;
    }
    c->rip = entry;
    c->cs = VIBEOS_HW_USER_CODE_SEL;
    c->rflags = 0x202;   /* reserved bit + IF */
    c->rsp = sp;         /* atop argc/argv/envp/auxv, 16-byte aligned */
    c->ss = VIBEOS_HW_USER_DATA_SEL;
}

/* Adopt the currently-executing kernel flow as a schedulable task, so the
 * kernel (and its serial CLI) is just another entry in the run queue rather
 * than something the scheduler has to "return to". */
static int hw_task_adopt_kernel(void) {
    int i = hw_task_alloc();
    if (i < 0) {
        return -1;
    }
    g_tasks[i].state = HW_TASK_RUNNING;
    g_tasks[i].on_cpu = 1;          /* it is this CPU, right now */
    g_tasks[i].is_user = 0;
    g_tasks[i].pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    g_tasks[i].kstack_top = hw_this_cpu()->syscall_kstack_top;
    g_tasks[i].cr3 = (uint64_t)(uintptr_t)&g_pml4[0];
    HW_TASK_MARK(i, cr3_set_by, "adopt_kernel");
    g_current_task = i;
    return i;
}

/* Idle task body: nothing to run on this core, so wait for the next interrupt
 * rather than burning the core in a spin. */
static void hw_idle_loop(void) {
    for (;;) {
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

/* Every CPU needs a task it can always fall back to, so the scheduler never has
 * to invent a context when the last runnable task blocks or exits. */
static int hw_task_create_idle(hw_cpu_t *cpu) {
    int i = hw_task_alloc();
    if (i < 0) {
        return -1;
    }
    g_tasks[i].kstack_top = hw_alloc_kstack(&g_tasks[i].kstack_base, &g_tasks[i].kstack_pages);
    if (g_tasks[i].kstack_top == 0) {
        hw_task_release(i);
        return -1;
    }
    {
        vibeos_x86_64_isr_frame_t *c = &g_tasks[i].ctx;
        uint32_t k;
        for (k = 0; k < (uint32_t)sizeof(*c); k++) {
            ((uint8_t *)(void *)c)[k] = 0;
        }
        c->rip = (uint64_t)(uintptr_t)hw_idle_loop;
        c->cs = VIBEOS_HW_KERNEL_CS;
        c->ss = VIBEOS_HW_KERNEL_DS;
        c->rflags = 0x202;
        /* Leave slack below the top, and land on rsp % 16 == 8: the idle loop is
         * entered by iretq rather than by a call, so the ABI's post-call stack
         * alignment has to be reproduced by hand. */
        c->rsp = g_tasks[i].kstack_top - 56ull;
    }
    g_tasks[i].cr3 = (uint64_t)(uintptr_t)&g_pml4[0];
    HW_TASK_MARK(i, cr3_set_by, "create_idle");
    g_tasks[i].state = HW_TASK_READY;
    HW_TASK_MARK(i, ready_by, "create_idle");
    g_tasks[i].is_user = 0;
    g_tasks[i].is_idle = 1;
    g_tasks[i].pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    cpu->idle_task = i;
    return i;
}

/* Create a ring-3 task from an ELF image: private address space, mapped stack,
 * READY to be picked by the scheduler. */
static int hw_task_spawn_user(const unsigned char *elf, uint64_t len,
                              const char *const *argv) {
    int i = hw_task_alloc();
    if (i < 0) {
        return -1;
    }
    if (hw_proc_create(&g_tasks[i].proc, elf, len, argv, 0) != 0) {
        hw_task_release(i);
        return -1;
    }
    g_tasks[i].kstack_top = hw_alloc_kstack(&g_tasks[i].kstack_base, &g_tasks[i].kstack_pages);
    if (g_tasks[i].kstack_top == 0) {
        hw_task_release(i);
        return -1;
    }
    g_tasks[i].cr3 = hw_proc_cr3(&g_tasks[i].proc);
    HW_TASK_MARK(i, cr3_set_by, "spawn_user");
    hw_task_init_user_ctx(&g_tasks[i].ctx, g_tasks[i].proc.entry,
                          g_tasks[i].proc.user_sp);
    /* Task slots are recycled, so anything the previous occupant left has to
     * be cleared explicitly. A stale TLS base would point the new program at
     * a dead process's thread state. */
    g_tasks[i].fs_base = 0;
    {
        uint32_t sg;
        g_tasks[i].exit_signal = 0;
        g_tasks[i].sig_pending = 0;
        g_tasks[i].sig_blocked = 0;
        for (sg = 0; sg < VIBEOS_HW_NSIG; sg++) {
            g_tasks[i].sig_handler[sg] = SIG_DFL_ADDR;
            g_tasks[i].sig_restorer[sg] = 0;
            g_tasks[i].sig_flags[sg] = 0;
            g_tasks[i].sig_mask[sg] = 0;
        }
    }
    g_tasks[i].state = HW_TASK_READY;
    g_tasks[i].is_user = 1;
    g_tasks[i].pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    return i;
}

/* exit(): retire the calling task and switch to another runnable one. Called
 * from the syscall path, so it enters the next task directly and never returns
 * to the caller. */
static void hw_task_exit(uint64_t code) {
    hw_cpu_t *cpu = hw_this_cpu();
    int dying = cpu->current_task;
    int next, i;

    /* A process owns its sockets: releasing them here is what stops a task that
     * exits with connections open from leaking them for the life of the system.
     * Done before the task is retired, while its descriptor table is still
     * ours to walk. */
    if (dying >= 0) {
        int fd;
        if (g_net_up) {
            hw_spin_lock(&g_net_lock);
            (void)vibeos_inet_release_owner_sockets(&g_net, g_tasks[dying].pid);
            hw_spin_unlock(&g_net_lock);
        }
        for (fd = 0; fd < VIBEOS_HW_MAX_FDS; fd++) {
            hw_fd_t *f = &g_tasks[dying].fds[fd];
            if (!f->used) {
                continue;
            }
            if (f->net_sock >= 0) {
                f->net_sock = -1;
            }
            /* Exiting closes everything, and for a pipe that is not tidiness:
             * the reader at the other end is waiting for its writers to reach
             * zero, and a program that produced its output and exited without
             * closing is the normal case. Leaving the count high is how
             * ls | wc -l prints nothing and hangs. */
            hw_pipe_release(f);
            f->used = 0;
        }
        for (fd = 0; fd < 3; fd++) {
            hw_fd_t *f = &g_tasks[dying].std_redirect[fd];
            if (f->used) {
                hw_pipe_release(f);
                f->used = 0;
            }
        }
    }

    if (dying >= 0) {
        /* Only the exit code is recorded here. Becoming a ZOMBIE - which is
         * what lets the parent reap the slot and hand it to the next fork -
         * waits until this task's address space has actually been taken down,
         * further below.
         *
         * It used to happen here, and that window is the wedge: the parent
         * reaps between the announcement and the teardown, a fork on another
         * core takes the freed slot and builds an address space in it, and
         * then the teardown below frees the *new* tenant's page tables. The
         * new process keeps running on a CR3 whose top-level page has gone
         * back to the allocator, and the next core to install it stops on the
         * instruction that loads it, with no kernel mapped to report from. */
        hw_spin_lock(&g_sched_lock);
        g_tasks[dying].exit_code = code;
        hw_spin_unlock(&g_sched_lock);
        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[SCHED] task pid=0x");
        vibeos_x86_64_serial_print_hex(g_tasks[dying].pid);
        vibeos_x86_64_serial_puts(" exited code=0x");
        vibeos_x86_64_serial_print_hex(code);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_unlock();
    }
    hw_spin_lock(&g_sched_lock);
    next = hw_pick_next(cpu);
    if (next < 0) {
        next = cpu->idle_task;
    }
    if (next < 0) {
        hw_spin_unlock(&g_sched_lock);
        vibeos_x86_64_serial_puts("[SCHED] no runnable task; halting\n");
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }
    if (dying >= 0) {
        g_tasks[dying].on_cpu = 0;
    }
    cpu->current_task = next;
    g_tasks[next].state = HW_TASK_RUNNING;
    g_tasks[next].on_cpu = 1;
    hw_spin_unlock(&g_sched_lock);
    hw_task_load_cpu_state(next);
    /* Now on the next task's CR3; the dying user address space is still
     * reachable through the shared kernel identity map, so free it. */
    if (dying >= 0 && g_tasks[dying].is_user) {
        HW_TASK_MARK(dying, aspace_killed_by, "task_exit");
        hw_aspace_destroy(&g_tasks[dying].proc.as);
        /* The kernel stack stays until the parent reaps us: we are still
         * executing on it right now. */
    }

    /* Now, and not before: the address space is gone, so a parent that reaps
     * this slot the instant it sees the zombie cannot have its child's page
     * tables pulled out from under it. cr3 goes too, so nothing can be
     * scheduled on a table that has been freed - the guard in
     * hw_task_load_cpu_state would catch that, but not being wrong beats
     * being told. */
    if (dying >= 0) {
        hw_spin_lock(&g_sched_lock);
        g_tasks[dying].cr3 = 0;
        g_tasks[dying].state = HW_TASK_ZOMBIE;
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (g_tasks[i].state == HW_TASK_BLOCKED &&
                g_tasks[i].pid == g_tasks[dying].ppid) {
                g_tasks[i].state = HW_TASK_READY;
                HW_TASK_MARK(i, ready_by, "parent_woken_by_child_exit");
            }
        }
        hw_spin_unlock(&g_sched_lock);
    }
    vibeos_x86_64_task_enter(&g_tasks[next].ctx); /* does not return */
}

/* ---- Linux syscall layer ------------------------------------------------- */

#include "vibeos/compat.h"

/* Linux errno values returned to user space (negated). */
#define VIBEOS_ENOSYS 38
#define VIBEOS_EFAULT 14
#define VIBEOS_EINVAL 22
#define VIBEOS_ENOMEM 12
#define VIBEOS_EBADF  9
#define VIBEOS_ENOENT 2
#define VIBEOS_ECHILD 10
#define VIBEOS_EAGAIN 11
#define VIBEOS_ENOTTY 25
#define VIBEOS_EPERM  1
#define VIBEOS_ESRCH  3
#define VIBEOS_EPIPE 32
#define VIBEOS_ERANGE 34
#define VIBEOS_EMFILE 24
#define VIBEOS_E2BIG  7
#define VIBEOS_EMFILE 24
#define VIBEOS_EIO    5
#define VIBEOS_ENOTDIR 20

/* Linux x86-64 syscall numbers we implement. */
#define LSYS_read   0
#define LSYS_write  1
#define LSYS_brk    12
#define LSYS_mmap   9
#define LSYS_getpid 39
#define LSYS_exit   60
#define LSYS_exit_group 231
#define LSYS_fork   57
#define LSYS_vfork  58
#define LSYS_wait4  61
#define LSYS_execve 59
#define LSYS_open   2
#define LSYS_close  3
#define LSYS_lseek  8
#define LSYS_getdents64 217
#define LSYS_unlink 87
#define LSYS_mkdir  83
#define LSYS_socket   41
#define LSYS_connect  42
#define LSYS_accept   43
#define LSYS_sendto   44
#define LSYS_recvfrom 45
#define LSYS_bind     49
#define LSYS_listen   50
/* VibeOS-specific: network control. Deliberately outside the Linux number
 * space, so it can never collide with a real syscall we implement later. */
#define LSYS_netctl   1000

/* Numbers a real C runtime reaches for before it runs any of the program.
 * Taken from arch/x86/entry/syscalls/syscall_64.tbl, not from memory. */
#define LSYS_mprotect       10
#define LSYS_munmap         11
#define LSYS_rt_sigaction   13
#define LSYS_rt_sigprocmask 14
#define LSYS_ioctl          16
#define LSYS_readv          19
#define LSYS_writev         20
#define LSYS_sched_yield    24
#define LSYS_uname          63
#define LSYS_getuid        102
#define LSYS_getgid        104
#define LSYS_geteuid       107
#define LSYS_getegid       108
#define LSYS_arch_prctl    158
#define LSYS_gettid        186
#define LSYS_futex         202
#define LSYS_set_tid_address 218
#define LSYS_clock_gettime 228
#define LSYS_set_robust_list 273
#define LSYS_prlimit64     302
#define LSYS_getrandom     318
#define LSYS_rseq          334

/* What a real program needs once it is past startup and doing work. Taken from
 * a strace of BusyBox running echo, cat, ls, pwd and wc - see
 * scripts/dev/trace-linux-binary.sh. */
#define LSYS_fstat           5
#define LSYS_sendfile       40
#define LSYS_getcwd         79
#define LSYS_setuid        105
#define LSYS_setgid        106
#define LSYS_prctl         157
#define LSYS_openat        257
#define LSYS_newfstatat    262
#define LSYS_readlinkat    267
#define LSYS_kill           62
#define LSYS_tgkill        234
#define LSYS_tkill         200
#define LSYS_dup            32
#define LSYS_dup2           33
#define LSYS_pipe           22
#define LSYS_pipe2         293
#define LSYS_time          201
#define LSYS_clone          56
#define LSYS_getppid       110
#define LSYS_rt_sigreturn   15

/* clone() flags that decide whether this is a fork or a thread. */
#define CLONE_VM     0x00000100u
#define CLONE_THREAD 0x00010000u

/* openat/newfstatat interpret a relative path against this directory fd. There
 * is no per-process working directory here, so it is the only value accepted.
 *
 * Reading it needs care. Arguments the Linux ABI types as `int` arrive in the
 * low half of a register, and writing a 32-bit register zeroes the upper half:
 * a caller doing `mov $-100, %edi` delivers 0x00000000ffffff9c, not
 * 0xffffffffffffff9c. Comparing the full 64 bits against -100 therefore never
 * matches, and every relative open fails with ENOSYS - which is exactly what
 * BusyBox reported as "can't open: Function not implemented". Read the low 32
 * bits and sign-extend, as the kernel this ABI belongs to does. */
#define AT_FDCWD           (-100)
#define VIBEOS_ARG_INT(v)  ((int)(uint32_t)(v))
#define AT_EMPTY_PATH      0x1000

/* struct stat, x86-64 layout. Byte offsets rather than a struct definition
 * because the layout is the ABI: it is fixed by Linux, not by this compiler. */
#define STAT_SIZE          144u
#define STAT_OFF_MODE       24u
#define STAT_OFF_NLINK      16u
#define STAT_OFF_UID        28u
#define STAT_OFF_GID        32u
#define STAT_OFF_SIZE       48u
#define STAT_OFF_BLKSIZE    56u
#define STAT_OFF_BLOCKS     64u
#define STAT_OFF_INO         8u

#define S_IFREG 0100000u
#define S_IFDIR 0040000u
#define S_IFCHR 0020000u

/* prctl operations. PR_SET_NAME is the one a real program actually uses. */
#define PR_SET_NAME 15
#define PR_GET_NAME 16

/* arch_prctl subfunctions. */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

/* mmap flags and protection bits (asm-generic/mman-common.h). */
#define PROT_NONE  0x0
#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define PROT_EXEC  0x4
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

/* futex operations we can answer honestly. */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_CMD_MASK 0x7F

static vibeos_compat_runtime_t g_compat_rt;

/* Validate that [va, va+len) is mapped in the *calling task's* address space
 * and reachable from ring 3. Without this the kernel would happily dereference
 * any pointer a user task passes - including kernel addresses. */
static int hw_user_range_ok(uint64_t va, uint64_t len, int need_write) {
    static const uint32_t shifts[3] = {39u, 30u, 21u};
    const hw_task_t *t;
    uint64_t page;

    if (len == 0) {
        return 1;
    }
    if (va + len < va) {
        return 0; /* wrap-around */
    }
    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return 0;
    }
    t = &g_tasks[g_current_task];

    for (page = va & ~0xFFFull; page < va + len; page += 4096ull) {
        const uint64_t *tbl = t->proc.as.pml4;
        uint64_t e = 0;
        uint32_t level;

        for (level = 0; level < 3u; level++) {
            e = tbl[(page >> shifts[level]) & 0x1FFu];
            if ((e & PTE_PRESENT) == 0 || (e & PTE_USER) == 0) {
                return 0;
            }
            if (level == 2u && (e & PTE_PS) != 0) {
                break; /* 2 MiB leaf */
            }
            tbl = (const uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
        }
        if ((e & PTE_PS) == 0) {
            e = tbl[(page >> 12) & 0x1FFu];
            if ((e & PTE_PRESENT) == 0 || (e & PTE_USER) == 0) {
                return 0;
            }
        }
        /* A copy-on-write page has its write bit cleared on purpose: the
         * process may write it, and doing so faults so the page can be
         * duplicated first. The hardware bit is the mechanism, not the
         * permission, so refusing the buffer here rejects writes that are
         * perfectly legal - which made every read() into freshly forked
         * memory return EFAULT, and a shell report end of input. */
        if (need_write && (e & PTE_WRITE) == 0 && (e & PTE_COW) == 0) {
            return 0;
        }
    }
    return 1;
}

static int hw_copy_user_string(uint64_t uptr, char *dst, int max); /* defined below */

/* Per-process open-file table helpers. fds 0-2 are the console; 3+ are files. */
static hw_fd_t *hw_fd_get(uint64_t fd) {
    if (g_current_task < 0 || fd < 3u || fd >= 3u + VIBEOS_HW_MAX_FDS) {
        return 0;
    }
    {
        hw_fd_t *f = &g_tasks[g_current_task].fds[fd - 3u];
        return f->used ? f : 0;
    }
}

/* Socket-backed descriptors are served by these (defined with the socket
 * syscalls below), so read/write work on a connection like any other stream. */
static long hw_net_recv(hw_fd_t *f, uint64_t buf, uint64_t len);
static long hw_net_send(hw_fd_t *f, uint64_t buf, uint64_t len);

/* Let go of one end of a pipe. The pipe itself lives until both ends are
 * gone, because a reader may still have data to drain after every writer has
 * closed. */
static void hw_pipe_release(hw_fd_t *f) {
    hw_pipe_t *pp;

    if (!f || f->pipe < 0 || f->pipe >= VIBEOS_HW_MAX_PIPES) {
        return;
    }
    pp = &g_pipes[f->pipe];
    hw_spin_lock(&g_pipe_lock);
    if (f->writable) {
        if (pp->writers > 0u) {
            pp->writers--;
        }
    } else if (pp->readers > 0u) {
        pp->readers--;
    }
    if (pp->readers == 0u && pp->writers == 0u) {
        pp->used = 0;
        pp->count = 0;
        pp->head = 0;
        pp->tail = 0;
    }
    hw_spin_unlock(&g_pipe_lock);
    f->pipe = -1;
    /* Somebody may be waiting for the data or the space that just became
     * possible - or for the end of file that just became true. */
    hw_keyboard_wake();
}

static long hw_pipe_read(hw_fd_t *f, uint64_t buf, uint64_t len) {
    hw_pipe_t *pp = &g_pipes[f->pipe];
    uint8_t *dst = (uint8_t *)(uintptr_t)buf;

    for (;;) {
        uint64_t copied = 0;

        hw_spin_lock(&g_pipe_lock);
        while (copied < len && pp->count > 0u) {
            dst[copied++] = pp->buf[pp->head];
            pp->head = (pp->head + 1u) % VIBEOS_HW_PIPE_BYTES;
            pp->count--;
        }
        hw_spin_unlock(&g_pipe_lock);
        if (copied > 0u) {
            hw_keyboard_wake();   /* a blocked writer may now have room */
            return (long)copied;
        }
        if (pp->writers == 0u) {
            return 0;   /* end of file: nobody can ever write again */
        }
        /* Nothing yet, and somebody could still write. Park instead of
         * spinning, so the writer actually gets a chance to run. */
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

static long hw_pipe_write(hw_fd_t *f, uint64_t buf, uint64_t len) {
    hw_pipe_t *pp = &g_pipes[f->pipe];
    const uint8_t *src = (const uint8_t *)(uintptr_t)buf;
    uint64_t written = 0;

    while (written < len) {
        uint64_t before = written;

        if (pp->readers == 0u) {
            /* Writing into a pipe nobody will read. Linux raises SIGPIPE and
             * returns EPIPE; with no handler the default action ends the
             * process, which is what stops a pipeline from filling memory
             * after its reader has gone. */
            if (g_current_task >= 0) {
                (void)hw_signal_raise(g_current_task, VIBEOS_SIGPIPE);
            }
            return written > 0u ? (long)written : -VIBEOS_EPIPE;
        }
        hw_spin_lock(&g_pipe_lock);
        while (written < len && pp->count < VIBEOS_HW_PIPE_BYTES) {
            pp->buf[pp->tail] = src[written++];
            pp->tail = (pp->tail + 1u) % VIBEOS_HW_PIPE_BYTES;
            pp->count++;
        }
        hw_spin_unlock(&g_pipe_lock);
        if (written > before) {
            hw_keyboard_wake();   /* a blocked reader now has data */
            continue;
        }
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
    return (long)written;
}

/* pipe2(): two descriptors onto one buffer, read end first. */
static long hw_sys_pipe2(uint64_t fds_uptr, uint64_t flags) {
    hw_task_t *t;
    int slot = -1, rfd = -1, wfd = -1;
    int i;

    (void)flags;   /* O_CLOEXEC has no meaning without an exec-close list */
    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if (!hw_user_range_ok(fds_uptr, 8, 1)) {
        return -VIBEOS_EFAULT;
    }
    t = &g_tasks[g_current_task];

    hw_spin_lock(&g_pipe_lock);
    for (i = 0; i < VIBEOS_HW_MAX_PIPES; i++) {
        if (!g_pipes[i].used) {
            g_pipes[i].used = 1;
            g_pipes[i].readers = 1;
            g_pipes[i].writers = 1;
            g_pipes[i].head = 0;
            g_pipes[i].tail = 0;
            g_pipes[i].count = 0;
            slot = i;
            break;
        }
    }
    hw_spin_unlock(&g_pipe_lock);
    if (slot < 0) {
        return -VIBEOS_EMFILE;
    }

    for (i = 0; i < VIBEOS_HW_MAX_FDS && (rfd < 0 || wfd < 0); i++) {
        if (t->fds[i].used) {
            continue;
        }
        {
            hw_fd_t *f = &t->fds[i];
            uint32_t z;
            for (z = 0; z < (uint32_t)sizeof(*f); z++) {
                ((uint8_t *)(void *)f)[z] = 0;
            }
            f->net_sock = -1;
            f->pipe = slot;
            f->writable = (rfd < 0) ? 0 : 1;
            f->used = 1;
        }
        if (rfd < 0) {
            rfd = 3 + i;
        } else {
            wfd = 3 + i;
        }
    }
    if (rfd < 0 || wfd < 0) {
        hw_spin_lock(&g_pipe_lock);
        g_pipes[slot].used = 0;
        hw_spin_unlock(&g_pipe_lock);
        if (rfd >= 0) {
            t->fds[rfd - 3].used = 0;
        }
        return -VIBEOS_EMFILE;
    }
    ((int *)(uintptr_t)fds_uptr)[0] = rfd;
    ((int *)(uintptr_t)fds_uptr)[1] = wfd;
    return 0;
}

/* dup2(): make newfd refer to whatever oldfd refers to.
 *
 * This is how a shell attaches a pipe to a program's standard input or output
 * without the program knowing. Only descriptors 0, 1 and 2 can be targets
 * here: the console is not an entry in the table, so redirecting one means
 * remembering that the entry now stands in for it. */
static long hw_sys_dup2(uint64_t oldfd, uint64_t newfd) {
    hw_task_t *t;
    hw_fd_t *src;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];
    if (oldfd == newfd) {
        return (long)newfd;
    }
    src = hw_fd_get(oldfd);
    if (!src) {
        return -VIBEOS_EBADF;
    }
    if (newfd >= 3u) {
        hw_fd_t *dst = (newfd < 3u + VIBEOS_HW_MAX_FDS)
                       ? &t->fds[newfd - 3u] : 0;
        if (!dst) {
            return -VIBEOS_EBADF;
        }
        if (dst->used) {
            hw_pipe_release(dst);
            dst->used = 0;
        }
        *dst = *src;
        if (dst->pipe >= 0) {
            hw_spin_lock(&g_pipe_lock);
            if (dst->writable) {
                g_pipes[dst->pipe].writers++;
            } else {
                g_pipes[dst->pipe].readers++;
            }
            hw_spin_unlock(&g_pipe_lock);
        }
        return (long)newfd;
    }
    /* Redirecting a standard descriptor: the branch above returned for every
     * other value, so newfd is 0, 1 or 2 here and re-checking that only looks
     * like a bound. */
    hw_pipe_release(&t->std_redirect[newfd]);
    t->std_redirect[newfd] = *src;
    if (t->std_redirect[newfd].pipe >= 0) {
        hw_spin_lock(&g_pipe_lock);
        if (t->std_redirect[newfd].writable) {
            g_pipes[t->std_redirect[newfd].pipe].writers++;
        } else {
            g_pipes[t->std_redirect[newfd].pipe].readers++;
        }
        hw_spin_unlock(&g_pipe_lock);
    }
    return (long)newfd;
}

static long hw_sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    const char *p = (const char *)(uintptr_t)buf;
    uint64_t i;

    if (!hw_user_range_ok(buf, len, 0)) {
        return -VIBEOS_EFAULT;
    }
    if (fd < 3u && g_current_task >= 0 &&
        g_tasks[g_current_task].std_redirect[fd].used) {
        hw_fd_t *r = &g_tasks[g_current_task].std_redirect[fd];
        if (r->pipe >= 0) {
            return hw_pipe_write(r, buf, len);
        }
    }
    if (fd >= 3u) { /* a file: buffer the bytes, committed on close */
        hw_fd_t *f = hw_fd_get(fd);
        uint64_t i2;
        if (!f) {
            return -VIBEOS_EBADF;
        }
        if (f->pipe >= 0) {
            return hw_pipe_write(f, buf, len);
        }
        if (f->net_sock >= 0) {
            return hw_net_send(f, buf, len);
        }
        if (!f->writable) {
            return -VIBEOS_EBADF;
        }
        for (i2 = 0; i2 < len; i2++) {
            if (f->wlen >= VIBEOS_HW_WBUF) {
                break;
            }
            f->wbuf[f->wlen++] = (uint8_t)p[i2];
        }
        f->dirty = 1;
        return (long)i2;
    }
    if (fd != 1u && fd != 2u) {
        return -VIBEOS_EBADF;
    }
    /* User output goes to both consoles: the serial line (logs, CI) and the
     * display framebuffer (what a user in front of the machine sees). */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[HW][SYS] write(ring3): ");
    for (i = 0; i < len; i++) {
        char c = p[i];
        if (c == '\n') {
            vibeos_x86_64_serial_putc('\r');
        }
        vibeos_x86_64_serial_putc(c);
        vibeos_x86_64_fb_putc(c);
    }
    vibeos_x86_64_serial_unlock();
    return (long)len;
}

/* read(0, ...): blocking keyboard read. Returns after at least one character;
 * blocks (BLOCKED + wait_input) until the keyboard IRQ enqueues input and wakes
 * us. The cli window makes the check-and-block race-free against the IRQ. */
static long hw_sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    uint8_t *dst = (uint8_t *)(uintptr_t)buf;

    if (len == 0u) {
        return 0;
    }
    if (!hw_user_range_ok(buf, len, 1)) {
        return -VIBEOS_EFAULT;
    }
    if (fd < 3u && g_current_task >= 0 &&
        g_tasks[g_current_task].std_redirect[fd].used) {
        hw_fd_t *r = &g_tasks[g_current_task].std_redirect[fd];
        if (r->pipe >= 0) {
            return hw_pipe_read(r, buf, len);
        }
    }
    if (fd >= 3u) { /* a file: stream from the filesystem */
        hw_fd_t *f = hw_fd_get(fd);
        long n;
        if (!f) {
            return -VIBEOS_EBADF;
        }
        if (f->pipe >= 0) {
            return hw_pipe_read(f, buf, len);
        }
        if (f->net_sock >= 0) {
            return hw_net_recv(f, buf, len);
        }
        {
            vibeos_fs_node_t node;
            node.id = f->cluster;
            node.size = f->size;
            node.is_dir = f->isdir;
            n = vibeos_fs_read_at(&g_rootfs, &node, f->pos, dst, (uint32_t)len);
        }
        if (n > 0) {
            f->pos += (uint32_t)n;
        }
        return n;
    }
    if (fd != 0u) {
        return -VIBEOS_EBADF;
    }
    for (;;) {
        uint64_t copied = 0;
        int c;

        __asm__ __volatile__("cli");
        c = vibeos_x86_64_keyboard_getc();
        if (c >= 0) {
            /* Line discipline: echo what was typed and let backspace erase the
             * previous character before the line is handed to the program. */
            while (copied < len && c >= 0) {
                if (c == '\b' || c == 127) {
                    if (copied > 0) {
                        copied--;
                        vibeos_x86_64_serial_puts("\b \b");
                        vibeos_x86_64_fb_putc('\b');
                    }
                    c = vibeos_x86_64_keyboard_getc();
                    continue;
                }
                dst[copied++] = (uint8_t)c;
                if (c == '\n') {
                    vibeos_x86_64_serial_putc('\r');
                }
                vibeos_x86_64_serial_putc((char)c);
                vibeos_x86_64_fb_putc((char)c);
                if ((uint8_t)c == '\n') {
                    break; /* line-oriented: stop at newline */
                }
                c = vibeos_x86_64_keyboard_getc();
            }
            __asm__ __volatile__("sti");
            return (long)copied;
        }
        if (g_current_task >= 0) {
            g_tasks[g_current_task].wait_input = 1;
            g_tasks[g_current_task].state = HW_TASK_BLOCKED;
        }
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

/* open(path, flags): resolve a file (or directory) and take an fd. With a write
 * flag the file is created/truncated on close from the buffered bytes. */
static long hw_sys_open(uint64_t path_uptr, uint64_t flags) {
    char path[64];
    hw_task_t *t;
    int i, k;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    t = &g_tasks[g_current_task];
    for (i = 0; i < VIBEOS_HW_MAX_FDS; i++) {
        if (!t->fds[i].used) {
            break;
        }
    }
    if (i == VIBEOS_HW_MAX_FDS) {
        return -VIBEOS_EMFILE;
    }
    {
        hw_fd_t *f = &t->fds[i];
        int writable = ((flags & 1u) != 0u) || ((flags & 0100u) != 0u); /* O_WRONLY|O_CREAT */
        uint32_t cluster = 0, size = 0;
        vibeos_fs_node_t node;
        int node_is_dir = 0;

        if (!writable) {
            if (vibeos_fs_lookup(&g_rootfs, path, &node) != 0) {
                return -VIBEOS_ENOENT;
            }
            cluster = (uint32_t)node.id;
            size = (uint32_t)node.size;
            node_is_dir = node.is_dir;
        }
        for (k = 0; k < (int)sizeof(f->name) - 1 && path[k]; k++) {
            f->name[k] = path[k];
        }
        f->name[k] = 0;
        f->cluster = cluster;
        f->size = size;
        f->pos = 0;
        f->dir_index = 0;
        /* Not a pipe. The field has to be set explicitly: descriptor slots are
         * recycled, so an uninitialised value here is whatever the previous
         * occupant left, and a stale pipe index sends every read and write on
         * this file into the pipe path - where it waits for a writer that does
         * not exist. */
        f->pipe = -1;
        /* The filesystem already answered this during lookup; asking twice
         * would put a FAT-specific question back in the syscall layer. */
        f->isdir = node_is_dir;
        f->wlen = 0;
        f->dirty = 0;
        f->writable = writable;
        f->net_sock = -1;
        f->used = 1;
    }
    return 3 + i;
}

/* ---- socket syscalls ------------------------------------------------------
 *
 * Sockets share the process file-descriptor table with files, so read/write/
 * close work on them unchanged and a program can treat a connection like any
 * other stream.
 *
 * The socket calls that wait - connect, accept, recv - block the calling task
 * the same way read(0) does: mark it BLOCKED and hlt. The network keeps moving
 * because the stack is pumped from the timer interrupt. */

#define VIBEOS_HW_NET_TIMEOUT_TICKS (VIBEOS_HW_TIMER_HZ * 10u)   /* 10 seconds */

/* Claim a free descriptor slot in the calling process. */
static int hw_fd_alloc(hw_task_t *t) {
    int i;
    for (i = 0; i < VIBEOS_HW_MAX_FDS; i++) {
        if (!t->fds[i].used) {
            hw_fd_t *f = &t->fds[i];
            uint32_t k;
            for (k = 0; k < (uint32_t)sizeof(*f); k++) {
                ((uint8_t *)(void *)f)[k] = 0;
            }
            f->net_sock = -1;
            f->used = 1;
            return i;
        }
    }
    return -1;
}

/* Read a struct sockaddr_in out of user memory: family (host order), port and
 * address (both network order on the wire). */
static int hw_read_sockaddr(uint64_t uptr, uint32_t *out_ip, uint16_t *out_port) {
    const uint8_t *p;
    if (!hw_user_range_ok(uptr, 8, 0)) {
        return -1;
    }
    p = (const uint8_t *)(uintptr_t)uptr;
    if (((uint16_t)p[0] | ((uint16_t)p[1] << 8)) != 2u) {   /* AF_INET */
        return -1;
    }
    *out_port = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
    *out_ip = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
              ((uint32_t)p[6] << 8) | (uint32_t)p[7];
    return 0;
}

static int hw_write_sockaddr(uint64_t uptr, uint32_t ip, uint16_t port) {
    uint8_t *p;
    if (uptr == 0u) {
        return 0;
    }
    if (!hw_user_range_ok(uptr, 16, 1)) {
        return -1;
    }
    p = (uint8_t *)(uintptr_t)uptr;
    p[0] = 2; p[1] = 0;
    p[2] = (uint8_t)(port >> 8);
    p[3] = (uint8_t)(port & 0xFFu);
    p[4] = (uint8_t)(ip >> 24);
    p[5] = (uint8_t)((ip >> 16) & 0xFFu);
    p[6] = (uint8_t)((ip >> 8) & 0xFFu);
    p[7] = (uint8_t)(ip & 0xFFu);
    {
        int k;
        for (k = 8; k < 16; k++) {
            p[k] = 0;
        }
    }
    return 0;
}

/* Give up the CPU until the next tick; the network is pumped from there. */
static void hw_net_wait_tick(void) {
    __asm__ __volatile__("sti; hlt" ::: "memory");
}

static long hw_sys_socket(uint64_t domain, uint64_t type) {
    hw_task_t *t;
    int fd, s;
    int kind;

    if (!g_net_up || g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if (domain != 2u) {                       /* AF_INET only */
        return -VIBEOS_EINVAL;
    }
    if ((type & 0xFFu) == 1u) {
        kind = VIBEOS_INET_SOCK_TCP;          /* SOCK_STREAM */
    } else if ((type & 0xFFu) == 2u) {
        kind = VIBEOS_INET_SOCK_UDP;          /* SOCK_DGRAM  */
    } else {
        return -VIBEOS_EINVAL;
    }

    t = &g_tasks[g_current_task];
    fd = hw_fd_alloc(t);
    if (fd < 0) {
        return -VIBEOS_EMFILE;
    }
    hw_spin_lock(&g_net_lock);
    s = vibeos_inet_socket(&g_net, kind);
    if (s >= 0 && vibeos_inet_socket_set_owner(&g_net, s, t->pid) != 0) {
        (void)vibeos_inet_close(&g_net, s);
        s = -1;
    }
    hw_spin_unlock(&g_net_lock);
    if (s < 0) {
        t->fds[fd].used = 0;
        return -VIBEOS_ENOMEM;
    }
    t->fds[fd].net_sock = s;
    t->fds[fd].pipe = -1;
    return 3 + fd;
}

static long hw_sys_bind(uint64_t fd, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint32_t ip;
    uint16_t port;
    int r;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (hw_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    hw_spin_lock(&g_net_lock);
    r = vibeos_inet_bind(&g_net, f->net_sock, port);
    hw_spin_unlock(&g_net_lock);
    return (r == 0) ? 0 : -VIBEOS_EINVAL;
}

static long hw_sys_listen(uint64_t fd) {
    hw_fd_t *f = hw_fd_get(fd);
    int r;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    hw_spin_lock(&g_net_lock);
    r = vibeos_inet_listen(&g_net, f->net_sock);
    hw_spin_unlock(&g_net_lock);
    return (r == 0) ? 0 : -VIBEOS_EINVAL;
}

static long hw_sys_connect(uint64_t fd, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint32_t ip;
    uint16_t port;
    uint64_t deadline;
    int r;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (hw_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    hw_spin_lock(&g_net_lock);
    r = vibeos_inet_connect(&g_net, f->net_sock, ip, port);
    hw_spin_unlock(&g_net_lock);
    if (r != 0) {
        return -VIBEOS_EINVAL;
    }

    deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;
    for (;;) {
        int st;
        hw_spin_lock(&g_net_lock);
        st = vibeos_inet_socket_state(&g_net, f->net_sock);
        hw_spin_unlock(&g_net_lock);
        if (st == VIBEOS_TCP_ESTABLISHED) {
            return 0;
        }
        if (st == VIBEOS_TCP_CLOSED || st < 0) {
            return -VIBEOS_EIO;   /* refused, reset, or gave up retransmitting */
        }
        if (g_timer_ticks > deadline) {
            return -VIBEOS_EIO;
        }
        hw_net_wait_tick();
    }
}

static long hw_sys_accept(uint64_t fd, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    hw_task_t *t;
    int child = -1;
    int nfd;

    if (!f || f->net_sock < 0 || g_current_task < 0) {
        return -VIBEOS_EBADF;
    }
    t = &g_tasks[g_current_task];
    for (;;) {
        hw_spin_lock(&g_net_lock);
        child = vibeos_inet_accept(&g_net, f->net_sock);
        hw_spin_unlock(&g_net_lock);
        if (child >= 0) {
            break;
        }
        if (child != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        hw_net_wait_tick();
    }

    nfd = hw_fd_alloc(t);
    if (nfd < 0) {
        hw_spin_lock(&g_net_lock);
        (void)vibeos_inet_close(&g_net, child);
        hw_spin_unlock(&g_net_lock);
        return -VIBEOS_EMFILE;
    }
    t->fds[nfd].net_sock = child;
    t->fds[nfd].pipe = -1;
    {
        uint32_t ip;
        uint16_t port;
        hw_spin_lock(&g_net_lock);
        ip = g_net.sockets[child].remote_ip;
        port = g_net.sockets[child].remote_port;
        hw_spin_unlock(&g_net_lock);
        (void)hw_write_sockaddr(addr_uptr, ip, port);
    }
    return 3 + nfd;
}

/* Blocking stream receive: returns 0 at end of stream, like Linux. */
static long hw_net_recv(hw_fd_t *f, uint64_t buf, uint64_t len) {
    uint64_t deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;

    if (!hw_user_range_ok(buf, len, 1)) {
        return -VIBEOS_EFAULT;
    }
    for (;;) {
        long n;
        hw_spin_lock(&g_net_lock);
        n = vibeos_inet_recv(&g_net, f->net_sock, (void *)(uintptr_t)buf, (uint32_t)len);
        hw_spin_unlock(&g_net_lock);
        if (n >= 0) {
            return n;
        }
        if (n == -VIBEOS_INET_ECONNRESET) {
            return -VIBEOS_EIO;
        }
        if (n != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        if (g_timer_ticks > deadline) {
            return -VIBEOS_EIO;
        }
        hw_net_wait_tick();
    }
}

static long hw_net_send(hw_fd_t *f, uint64_t buf, uint64_t len) {
    long n;
    if (!hw_user_range_ok(buf, len, 0)) {
        return -VIBEOS_EFAULT;
    }
    hw_spin_lock(&g_net_lock);
    n = vibeos_inet_send(&g_net, f->net_sock, (const void *)(uintptr_t)buf, (uint32_t)len);
    hw_spin_unlock(&g_net_lock);
    if (n < 0) {
        return (n == -VIBEOS_INET_EAGAIN) ? 0 : -VIBEOS_EIO;
    }
    return n;
}

static long hw_sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint32_t ip;
    uint16_t port;
    long n;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (addr_uptr == 0u) {
        return hw_net_send(f, buf, len);
    }
    if (hw_read_sockaddr(addr_uptr, &ip, &port) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (!hw_user_range_ok(buf, len, 0)) {
        return -VIBEOS_EFAULT;
    }
    hw_spin_lock(&g_net_lock);
    n = vibeos_inet_sendto(&g_net, f->net_sock, (const void *)(uintptr_t)buf,
                           (uint32_t)len, ip, port);
    hw_spin_unlock(&g_net_lock);
    return (n < 0) ? -VIBEOS_EIO : n;
}

/* netctl: the small control surface a shell needs to inspect and exercise the
 * interface. Linux would spread this across ioctl and netlink; VibeOS keeps one
 * explicit call rather than pretending to implement either.
 *
 *   op 0  write {ip, netmask, gateway, dns, up} as five u32 to `arg`
 *   op 1  ping `arg` (an IPv4 address), returns the round trip in ms
 *   op 2  resolve the name at `arg`, returns the address
 *   op 3  write {tx_frames, rx_frames, rx_dropped, tcp_retransmits} as four u64
 */
static long hw_sys_netctl(uint64_t op, uint64_t arg) {
    uint64_t deadline;

    if (!g_net_up) {
        return -VIBEOS_EIO;
    }
    switch (op) {
        case 0: {
            uint32_t *out;
            if (!hw_user_range_ok(arg, 20, 1)) {
                return -VIBEOS_EFAULT;
            }
            out = (uint32_t *)(uintptr_t)arg;
            hw_spin_lock(&g_net_lock);
            out[0] = g_net.ip;
            out[1] = g_net.netmask;
            out[2] = g_net.gateway;
            out[3] = g_net.dns;
            out[4] = (uint32_t)vibeos_inet_dhcp_bound(&g_net);
            hw_spin_unlock(&g_net_lock);
            return 0;
        }
        case 1: {
            hw_spin_lock(&g_net_lock);
            (void)vibeos_inet_ping(&g_net, (uint32_t)arg);
            hw_spin_unlock(&g_net_lock);
            deadline = g_timer_ticks + (VIBEOS_HW_TIMER_HZ * 4u);
            for (;;) {
                uint64_t rtt = 0;
                int r;
                hw_spin_lock(&g_net_lock);
                r = vibeos_inet_ping_result(&g_net, &rtt);
                hw_spin_unlock(&g_net_lock);
                if (r == 0) {
                    return (long)rtt;
                }
                if (g_timer_ticks > deadline) {
                    return -VIBEOS_EIO;
                }
                hw_net_wait_tick();
            }
        }
        case 2: {
            char name[64];
            if (hw_copy_user_string(arg, name, sizeof(name)) != 0) {
                return -VIBEOS_EFAULT;
            }
            hw_spin_lock(&g_net_lock);
            (void)vibeos_inet_resolve(&g_net, name);
            hw_spin_unlock(&g_net_lock);
            deadline = g_timer_ticks + (VIBEOS_HW_TIMER_HZ * 5u);
            for (;;) {
                uint32_t ip = 0;
                int r;
                hw_spin_lock(&g_net_lock);
                r = vibeos_inet_resolve_result(&g_net, &ip);
                hw_spin_unlock(&g_net_lock);
                if (r == 0) {
                    return (long)ip;
                }
                if (r != -VIBEOS_INET_EAGAIN || g_timer_ticks > deadline) {
                    return -VIBEOS_ENOENT;
                }
                hw_net_wait_tick();
            }
        }
        case 3: {
            uint64_t *out;
            if (!hw_user_range_ok(arg, 32, 1)) {
                return -VIBEOS_EFAULT;
            }
            out = (uint64_t *)(uintptr_t)arg;
            hw_spin_lock(&g_net_lock);
            out[0] = g_net.tx_frames;
            out[1] = g_net.rx_frames;
            out[2] = g_net.rx_dropped;
            out[3] = g_net.tcp_retransmits;
            hw_spin_unlock(&g_net_lock);
            return 0;
        }
        default:
            return -VIBEOS_EINVAL;
    }
}

static long hw_sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t addr_uptr) {
    hw_fd_t *f = hw_fd_get(fd);
    uint64_t deadline;

    if (!f || f->net_sock < 0) {
        return -VIBEOS_EBADF;
    }
    if (!hw_user_range_ok(buf, len, 1)) {
        return -VIBEOS_EFAULT;
    }
    deadline = g_timer_ticks + VIBEOS_HW_NET_TIMEOUT_TICKS;
    for (;;) {
        long n;
        uint32_t ip = 0;
        uint16_t port = 0;
        hw_spin_lock(&g_net_lock);
        n = vibeos_inet_recvfrom(&g_net, f->net_sock, (void *)(uintptr_t)buf,
                                 (uint32_t)len, &ip, &port);
        hw_spin_unlock(&g_net_lock);
        if (n >= 0) {
            (void)hw_write_sockaddr(addr_uptr, ip, port);
            return n;
        }
        if (n != -VIBEOS_INET_EAGAIN) {
            return -VIBEOS_EINVAL;
        }
        if (g_timer_ticks > deadline) {
            return -VIBEOS_EIO;
        }
        hw_net_wait_tick();
    }
}

/* close(fd): commit buffered writes to the filesystem and release the slot. */
static long hw_sys_close(uint64_t fd) {
    hw_fd_t *f = hw_fd_get(fd);
    long rc = 0;

    if (fd < 3u && g_current_task >= 0) {
        /* Closing a redirected standard descriptor drops the redirection. */
        hw_task_t *t = &g_tasks[g_current_task];
        if (t->std_redirect[fd].used) {
            hw_pipe_release(&t->std_redirect[fd]);
            t->std_redirect[fd].used = 0;
            return 0;
        }
    }
    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->pipe >= 0) {
        hw_pipe_release(f);
        f->used = 0;
        return 0;
    }
    if (f->net_sock >= 0) {
        hw_spin_lock(&g_net_lock);
        (void)vibeos_inet_close(&g_net, f->net_sock);
        hw_spin_unlock(&g_net_lock);
        f->net_sock = -1;
        f->used = 0;
        return 0;
    }
    if (f->writable && f->dirty) {
        /* The volume changed, so a staged image may no longer match the file
         * it came from. Dropping it here is the whole basis for trusting the
         * cache: a rewritten program must not keep running as its old self. */
        hw_exec_cache_drop();
        if (vibeos_fs_write_file(&g_rootfs, f->name, f->wbuf, f->wlen) < 0) {
            rc = -VIBEOS_EIO;
        }
    }
    f->used = 0;
    return rc;
}

static long hw_sys_lseek(uint64_t fd, uint64_t off, uint64_t whence) {
    hw_fd_t *f = hw_fd_get(fd);
    uint32_t base;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    base = (whence == 1u) ? f->pos : ((whence == 2u) ? f->size : 0u);
    f->pos = base + (uint32_t)off;
    return (long)f->pos;
}

/* getdents64(fd, buf, len): fill Linux dirent64 records from the directory the
 * fd was opened on, so user space can list a directory. */
static long hw_sys_getdents64(uint64_t fd, uint64_t buf, uint64_t len) {
    hw_fd_t *f = hw_fd_get(fd);
    uint8_t *out = (uint8_t *)(uintptr_t)buf;
    uint64_t used = 0;
    uint32_t records = 0;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (!f->isdir) {
        return -VIBEOS_ENOTDIR;
    }
    if (!hw_user_range_ok(buf, len, 1)) {
        return -VIBEOS_EFAULT;
    }
    /* A bounded syscall must not spin forever if a filesystem backend returns
     * a cyclic directory stream or fails to advance its cursor. */
    while (records < 256u && f->dir_index < VIBEOS_HW_MAX_DIR_ENTRIES) {
        char name[16];
        uint32_t fsize = 0;
        int is_dir = 0, n = 0;
        uint16_t reclen;

        {
            uint64_t entry_size = 0;
            if (vibeos_fs_list(&g_rootfs, f->name, f->dir_index, name,
                                sizeof(name), &entry_size, &is_dir) != 0) {
                break; /* end of directory */
            }
            fsize = (uint32_t)entry_size;
            (void)fsize;   /* getdents64 reports names and kinds, not sizes */
        }
        while (name[n]) {
            n++;
        }
        reclen = (uint16_t)((19 + n + 1 + 7) & ~7); /* 8+8+2+1 header, 8-aligned */
        if (used + reclen > len) {
            break;
        }
        {
            uint8_t *rec = out + used;
            int k;
            for (k = 0; k < reclen; k++) {
                rec[k] = 0;
            }
            rec[16] = (uint8_t)(reclen & 0xFFu);
            rec[17] = (uint8_t)(reclen >> 8);
            rec[18] = is_dir ? 4u : 8u; /* DT_DIR / DT_REG */
            for (k = 0; k < n; k++) {
                rec[19 + k] = (uint8_t)name[k];
            }
        }
        used += reclen;
        f->dir_index++;
        records++;
    }
    return (long)used;
}

/* unlink(path) / mkdir(path): filesystem mutations from user space. */
static long hw_sys_unlink(uint64_t path_uptr) {
    char path[64];
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return (vibeos_fs_unlink(&g_rootfs, path) == 0) ? 0 : -VIBEOS_ENOENT;
}

static long hw_sys_mkdir(uint64_t path_uptr) {
    char path[64];
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return (vibeos_fs_mkdir(&g_rootfs, path) == 0) ? 0 : -VIBEOS_EIO;
}

/* brk(0) reports the break; brk(addr) grows it, mapping fresh pages. */
static long hw_sys_brk(uint64_t addr) {
    hw_proc_t *proc;
    uint64_t new_brk, pages;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    proc = &g_tasks[g_current_task].proc;
    if (addr == 0u) {
        return (long)proc->brk_cur;
    }
    if (addr < VIBEOS_HW_USER_HEAP_BASE || addr >= VIBEOS_HW_USER_MMAP_BASE) {
        return (long)proc->brk_cur; /* out of the heap arena: unchanged */
    }
    new_brk = (addr + 0xFFFull) & ~0xFFFull;
    if (new_brk > proc->brk_cur) {
        pages = (new_brk - proc->brk_cur) / 4096ull;
        if (hw_map_user_pages(&proc->as, proc->brk_cur, pages) != 0) {
            return -VIBEOS_ENOMEM;
        }
    }
    proc->brk_cur = new_brk;
    return (long)proc->brk_cur;
}

/* Find the leaf page-table entry for `va`, or NULL if nothing maps it.
 * Deliberately does not create tables: callers are changing or removing an
 * existing mapping, and silently materialising one would hide a bad address. */
static uint64_t *hw_pte_lookup(vibeos_hw_aspace_t *as, uint64_t va) {
    static const uint32_t shifts[3] = {39u, 30u, 21u};
    uint64_t *tbl = as->pml4;
    uint32_t level;

    for (level = 0; level < 3u; level++) {
        uint64_t e = tbl[(va >> shifts[level]) & 0x1FFu];
        if ((e & PTE_PRESENT) == 0) {
            return 0;
        }
        if (level == 2u && (e & PTE_PS) != 0) {
            return 0;   /* a 2 MiB leaf; user mappings are 4 KiB */
        }
        tbl = (uint64_t *)(uintptr_t)(e & 0x000FFFFFFFFFF000ull);
    }
    {
        uint64_t *pte = &tbl[(va >> 12) & 0x1FFu];
        return (*pte & PTE_PRESENT) ? pte : 0;
    }
}

/* Drop one page from the TLB. Changing a PTE without this leaves the old
 * translation cached, so a revoked write permission is not actually revoked
 * until something else happens to flush it. */
static void hw_invlpg(uint64_t va) {
    __asm__ __volatile__("invlpg (%0)" : : "r"((void *)(uintptr_t)va) : "memory");
}

/* Resolve a write to a copy-on-write page.
 *
 * Returns non-zero when the fault was handled and execution may resume.
 *
 * Every condition is checked rather than assumed, because being wrong here
 * turns a memory-protection violation into a silent success. The fault must be
 * a write (bit 1), from user space (bit 2), to a page that is present (bit 0)
 * - a not-present fault is a genuine bad access, not a shared page - and the
 * entry must carry our own copy-on-write bit. A read-only page without that
 * bit is read-only because the program is not allowed to write it. */
static int hw_handle_cow_fault(uint64_t fault_va, uint64_t error_code) {
    hw_task_t *t;
    uint64_t *pte;
    uint64_t phys;
    uint8_t *fresh;

    /* Present and write. The originating privilege level is deliberately not
     * required to be user: the kernel writes into user memory on a process's
     * behalf - read() filling a buffer, a syscall storing a result - and with
     * CR0.WP set those writes fault on a read-only page exactly as ring 3
     * would. Insisting on the user bit here refuses precisely the faults that
     * happen while serving a syscall, which is how a freshly forked shell
     * dies without printing anything. */
    if ((error_code & 0x3u) != 0x3u) {
        return 0;
    }
    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return 0;
    }
    t = &g_tasks[g_current_task];
    pte = hw_pte_lookup(&t->proc.as, fault_va);
    if (!pte || (*pte & PTE_USER) == 0 || (*pte & PTE_COW) == 0) {
        /* Not a shared page: a genuine protection violation, and the fault
         * reporting below is where it belongs. */
        return 0;
    }
    phys = *pte & 0x000FFFFFFFFFF000ull;

    /* Sole owner: nothing to copy, just take the write permission back. This
     * is the common case once the other side has exec'd or exited, and
     * copying there would waste a page and a copy for nothing. */
    if (frame_ref_dec(phys)) {
        *pte = phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
        hw_invlpg(fault_va);
        return 1;
    }

    fresh = (uint8_t *)hw_alloc_page();
    if (!fresh) {
        /* Out of memory. The reference was already dropped above, so put it
         * back before giving up: leaving it low would let the frame be freed
         * while this process is still using it. */
        frame_ref_inc(phys);
        return 0;
    }
    {
        const uint8_t *src = (const uint8_t *)(uintptr_t)phys;
        uint64_t b;
        for (b = 0; b < 4096ull; b++) {
            fresh[b] = src[b];
        }
    }
    *pte = ((uint64_t)(uintptr_t)fresh) | PTE_PRESENT | PTE_WRITE | PTE_USER;
    hw_invlpg(fault_va);
    g_cow_copied++;
    return 1;
}

/* Anonymous mmap: bump the per-process arena and map zeroed pages.
 *
 * The address hint is ignored - the arena is bump-allocated, so honouring a
 * fixed address would require a real VMA tree. MAP_FIXED is therefore refused
 * rather than quietly ignored: a program that asks for a specific address and
 * silently gets another one corrupts itself later, far from here. */
static long hw_sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd) {
    hw_proc_t *proc;
    uint64_t pages, base, leaf;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user || len == 0u) {
        return -VIBEOS_EINVAL;
    }
    if (flags & MAP_FIXED) {
        return -VIBEOS_EINVAL;
    }
    /* File-backed mappings need a page cache this kernel does not have. Say so
     * instead of returning anonymous zeroes, which would look like a file full
     * of NULs. */
    if ((flags & MAP_ANONYMOUS) == 0 || VIBEOS_ARG_INT(fd) >= 0) {
        return -VIBEOS_ENOSYS;
    }
    if (prot == PROT_NONE) {
        return -VIBEOS_EINVAL;   /* nothing sensible to map */
    }
    (void)addr;

    proc = &g_tasks[g_current_task].proc;
    pages = (len + 0xFFFull) / 4096ull;
    base = proc->mmap_cur;
    if (base + pages * 4096ull < base) {
        return -VIBEOS_ENOMEM;
    }
    leaf = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) {
        leaf |= PTE_WRITE;
    }
    {
        uint64_t i;
        for (i = 0; i < pages; i++) {
            void *page = hw_alloc_page();
            if (!page || hw_map_page(&proc->as, base + i * 4096ull,
                                     (uint64_t)(uintptr_t)page, leaf) != 0) {
                return -VIBEOS_ENOMEM;
            }
        }
    }
    proc->mmap_cur = base + pages * 4096ull;
    return (long)base;
}

/* mprotect(): change permissions on pages that are already mapped.
 *
 * Applied to the real page-table entries rather than recorded and ignored. A
 * libc uses this for RELRO - it maps its relocated data writable, then takes
 * write away - and a kernel that returns success without revoking anything
 * leaves the program less protected than it believes itself to be. */
static long hw_sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    hw_proc_t *proc;
    uint64_t va, end;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if ((addr & 0xFFFull) != 0u || len == 0u || addr + len < addr) {
        return -VIBEOS_EINVAL;
    }
    proc = &g_tasks[g_current_task].proc;
    end = (addr + len + 0xFFFull) & ~0xFFFull;

    /* Check the whole range first: a partial application would leave the
     * address space in a state the caller never asked for. */
    for (va = addr; va < end; va += 4096ull) {
        uint64_t *pte = hw_pte_lookup(&proc->as, va);
        if (!pte || (*pte & PTE_USER) == 0) {
            return -VIBEOS_EFAULT;
        }
    }
    for (va = addr; va < end; va += 4096ull) {
        uint64_t *pte = hw_pte_lookup(&proc->as, va);
        /* The pass above established that every page in the range is mapped,
         * so this cannot be NULL - but checking there and not here is the kind
         * of asymmetry that survives a later edit to one loop and not the
         * other, and the cost of being consistent is one branch. */
        if (!pte) {
            continue;
        }
        if (prot & PROT_WRITE) {
            *pte |= PTE_WRITE;
        } else {
            *pte &= ~PTE_WRITE;
        }
        hw_invlpg(va);
    }
    return 0;
}

/* munmap(): remove mappings and give the frames back.
 *
 * The arena is a bump allocator, so the address space is not reclaimed for
 * reuse - but the pages are unmapped for real, so a use-after-unmap faults
 * here exactly as it would on Linux instead of quietly still working. */
static long hw_sys_munmap(uint64_t addr, uint64_t len) {
    hw_proc_t *proc;
    uint64_t va, end;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if ((addr & 0xFFFull) != 0u || len == 0u || addr + len < addr) {
        return -VIBEOS_EINVAL;
    }
    proc = &g_tasks[g_current_task].proc;
    end = (addr + len + 0xFFFull) & ~0xFFFull;
    for (va = addr; va < end; va += 4096ull) {
        uint64_t *pte = hw_pte_lookup(&proc->as, va);
        if (pte && (*pte & PTE_USER) != 0) {
            hw_free_page((void *)(uintptr_t)(*pte & 0x000FFFFFFFFFF000ull));
            *pte = 0;
            hw_invlpg(va);
        }
    }
    return 0;
}

/* Duplicate every user mapping of `src` into `dst`, copying the backing pages.
 * User space lives entirely in PML4 slot 1, so only that subtree is walked. */
/* Share one present user leaf with the destination address space.
 *
 * The page itself is not copied. Both sides get a read-only mapping of the
 * same frame, marked copy-on-write, and the first write from either side
 * takes a fault that duplicates it. This is what makes fork cheap: a shell
 * forks for every external command and the exec that follows immediately
 * throws the copy away, so eager copying is work that is guaranteed to be
 * wasted - two megabytes of it per command for a program the size of BusyBox.
 *
 * The parent's entry is rewritten too. Leaving it writable would let the
 * parent modify a page the child can still see, which is precisely the
 * sharing fork exists to prevent.
 *
 * A page that was already read-only is shared as-is: it can never be written,
 * so it never needs duplicating, and marking it COW would turn a legitimate
 * protection fault into a silent success. */
static int hw_share_user_leaf(vibeos_hw_aspace_t *dst, uint64_t va, uint64_t *src_pte) {
    uint64_t pte = *src_pte;
    uint64_t phys = pte & 0x000FFFFFFFFFF000ull;
    uint64_t flags = pte & (PTE_PRESENT | PTE_USER);

    /* Three cases, and conflating the last two is a silent disaster.
     *
     *   writable        -> becomes copy-on-write on both sides
     *   already COW     -> stays copy-on-write; it is read-only because it is
     *                      shared, not because the program may not write it
     *   read-only       -> shared as is; it can never be written, so it never
     *                      needs duplicating
     *
     * A second fork sees the first fork's pages as read-only. Treating them as
     * the third case drops the copy-on-write mark, and the page becomes
     * permanently unwritable for everyone - which is a shell that runs two
     * commands and dies on the third. */
    if (pte & PTE_WRITE) {
        flags |= PTE_COW;
        *src_pte = (pte & ~PTE_WRITE) | PTE_COW;
        hw_invlpg(va);
    } else if (pte & PTE_COW) {
        flags |= PTE_COW;
    }
    frame_ref_inc(phys);
    g_cow_shared++;
    if (va < VIBEOS_HW_IDENTITY_LIMIT) {
        return hw_map_low_user_page(dst, va, phys, flags);
    }
    return hw_map_page(dst, va, phys, flags);
}

/* Duplicate the user pages a process has in the kernel's identity region -
 * a Linux image linked at 0x400000 lives there, and a fork that skipped it
 * would hand the child an address space with no program in it. Only entries
 * marked PTE_USER are copied; everything else down there is the kernel's. */
static int hw_aspace_copy_low_user(vibeos_hw_aspace_t *dst, vibeos_hw_aspace_t *src) {
    uint64_t *spdpt;
    uint32_t i, j, k;

    if ((src->pml4[0] & PTE_PRESENT) == 0) {
        return 0;
    }
    spdpt = (uint64_t *)(uintptr_t)(src->pml4[0] & 0x000FFFFFFFFFF000ull);
    if (spdpt == &g_pdpt[0]) {
        return 0;   /* still fully shared: this process has nothing down here */
    }
    for (i = 0; i < 512u; i++) {
        uint64_t *spd;
        if ((spdpt[i] & PTE_PRESENT) == 0) {
            continue;
        }
        spd = (uint64_t *)(uintptr_t)(spdpt[i] & 0x000FFFFFFFFFF000ull);
        if (i < VIBEOS_HW_IDENTITY_GIB && spd == &g_pd[i][0]) {
            continue;
        }
        for (j = 0; j < 512u; j++) {
            uint64_t *spt;
            if ((spd[j] & PTE_PRESENT) == 0 || (spd[j] & PTE_PS) != 0) {
                continue;
            }
            spt = (uint64_t *)(uintptr_t)(spd[j] & 0x000FFFFFFFFFF000ull);
            for (k = 0; k < 512u; k++) {
                uint64_t va;
                if ((spt[k] & PTE_PRESENT) == 0 || (spt[k] & PTE_USER) == 0) {
                    continue;
                }
                va = ((uint64_t)i << 30) | ((uint64_t)j << 21) | ((uint64_t)k << 12);
                if (hw_share_user_leaf(dst, va, (uint64_t *)&spt[k]) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int hw_aspace_copy_user(vibeos_hw_aspace_t *dst, vibeos_hw_aspace_t *src) {
    uint64_t *spdpt;
    uint32_t i, j, k;

    if (hw_aspace_copy_low_user(dst, src) != 0) {
        return -1;
    }
    if ((src->pml4[1] & PTE_PRESENT) == 0) {
        return 0;
    }
    spdpt = (uint64_t *)(uintptr_t)(src->pml4[1] & 0x000FFFFFFFFFF000ull);
    for (i = 0; i < 512u; i++) {
        uint64_t *spd;
        if ((spdpt[i] & PTE_PRESENT) == 0) {
            continue;
        }
        spd = (uint64_t *)(uintptr_t)(spdpt[i] & 0x000FFFFFFFFFF000ull);
        for (j = 0; j < 512u; j++) {
            uint64_t *spt;
            if ((spd[j] & PTE_PRESENT) == 0) {
                continue;
            }
            spt = (uint64_t *)(uintptr_t)(spd[j] & 0x000FFFFFFFFFF000ull);
            for (k = 0; k < 512u; k++) {
                uint64_t va;

                if ((spt[k] & PTE_PRESENT) == 0) {
                    continue;
                }
                va = (1ull << 39) | ((uint64_t)i << 30) | ((uint64_t)j << 21) | ((uint64_t)k << 12);
                if (hw_share_user_leaf(dst, va, (uint64_t *)&spt[k]) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

/* fork(): duplicate the calling task, address space and all. The child resumes
 * at the same instruction with a 0 return value. */
static long hw_sys_fork(const vibeos_x86_64_isr_frame_t *frame) {
    hw_task_t *parent;
    hw_task_t *child;
    int idx;
    uint32_t my_tenancy;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    idx = hw_task_alloc();
    if (idx < 0) {
        return -VIBEOS_ENOMEM;
    }
    parent = &g_tasks[g_current_task];
    child = &g_tasks[idx];
    my_tenancy = child->alloc_seq;

    if (hw_aspace_create(&child->proc.as) != 0 ||
        hw_aspace_copy_user(&child->proc.as, &parent->proc.as) != 0) {
        child->state = HW_TASK_FREE;
        return -VIBEOS_ENOMEM;
    }
    child->kstack_top = hw_alloc_kstack(&child->kstack_base, &child->kstack_pages);
    if (child->kstack_top == 0) {
        child->state = HW_TASK_FREE;
        return -VIBEOS_ENOMEM;
    }
    child->proc.entry = parent->proc.entry;
    child->proc.brk_cur = parent->proc.brk_cur;
    child->proc.mmap_cur = parent->proc.mmap_cur;
    child->cr3 = hw_proc_cr3(&child->proc);
    child->cr3_set_by = "fork";
    child->ctx = *frame;   /* resume exactly where the parent is */
    child->ctx.rax = 0;    /* ... but fork() returns 0 in the child */
    child->pid = (uint32_t)__sync_fetch_and_add(&g_next_pid, 1u);
    child->ppid = parent->pid;
    child->fs_base = parent->fs_base;   /* the copied image expects its TLS */
    {
        uint32_t sg;
        /* Open descriptors are inherited. This is not a refinement: a shell
         * builds a pipeline by creating the pipe, forking, and having the
         * child move an inherited end onto its standard output. Without
         * inheritance the child has no such descriptor, the redirection fails,
         * and its output goes to the console while the reader waits forever.
         *
         * Task slots are recycled, so the child's table is whatever the
         * previous occupant left; it must be overwritten, not added to. */
        int fi;
        for (fi = 0; fi < VIBEOS_HW_MAX_FDS; fi++) {
            child->fds[fi] = parent->fds[fi];
        }
        for (fi = 0; fi < 3; fi++) {
            child->std_redirect[fi] = parent->std_redirect[fi];
        }
        /* Every inherited pipe end gains an owner. Missing this is the other
         * way a pipeline hangs: the reader waits for an end of file that never
         * arrives because a count went wrong. */
        hw_spin_lock(&g_pipe_lock);
        for (fi = 0; fi < VIBEOS_HW_MAX_FDS; fi++) {
            const hw_fd_t *cf = &child->fds[fi];
            if (cf->used && cf->pipe >= 0) {
                if (cf->writable) {
                    g_pipes[cf->pipe].writers++;
                } else {
                    g_pipes[cf->pipe].readers++;
                }
            }
        }
        for (fi = 0; fi < 3; fi++) {
            const hw_fd_t *cf = &child->std_redirect[fi];
            if (cf->used && cf->pipe >= 0) {
                if (cf->writable) {
                    g_pipes[cf->pipe].writers++;
                } else {
                    g_pipes[cf->pipe].readers++;
                }
            }
        }
        hw_spin_unlock(&g_pipe_lock);

        child->exit_signal = 0;
        child->sig_pending = 0;   /* pending signals are not inherited */
        child->sig_blocked = parent->sig_blocked;
        for (sg = 0; sg < VIBEOS_HW_NSIG; sg++) {
            child->sig_handler[sg] = parent->sig_handler[sg];
            child->sig_restorer[sg] = parent->sig_restorer[sg];
            child->sig_flags[sg] = parent->sig_flags[sg];
            child->sig_mask[sg] = parent->sig_mask[sg];
        }
    }
    child->is_user = 1;
    child->exit_code = 0;
    /* The slot must still be the one this fork was given.
     *
     * Everything above writes into g_tasks[idx] without the scheduler lock,
     * on the strength of hw_task_alloc having marked it RESERVED. If that ever
     * fails to hold, two owners fill one slot and the loser's half-written
     * task is what gets scheduled - which is exactly the shape of the wedge
     * this is hunting. Saying so out loud beats inferring it from wreckage. */
    if (child->alloc_seq != my_tenancy || child->state != HW_TASK_RESERVED) {
        vibeos_x86_64_serial_puts("[SCHED] fork lost its slot: idx=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)idx);
        vibeos_x86_64_serial_puts(" mine=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)my_tenancy);
        vibeos_x86_64_serial_puts(" now=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)child->alloc_seq);
        vibeos_x86_64_serial_puts(" state=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)child->state);
        vibeos_x86_64_serial_puts("\n");
        hw_panic("two owners filled one task slot");
    }
    child->state = HW_TASK_READY;
    child->ready_by = "fork";
    return (long)child->pid;
}

/* waitpid(): reap a finished child. Blocks the caller (state BLOCKED, so the
 * scheduler stops running it) until a child exit wakes it, instead of spinning.
 * The check-and-block is done under cli so a child exit cannot slip in between
 * (lost wakeup); `sti; hlt` then parks the task with interrupts enabled. */
static long hw_sys_waitpid(uint64_t want_pid, uint64_t status_ptr) {
    uint32_t mypid;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    mypid = g_tasks[g_current_task].pid;

    for (;;) {
        int i;
        int have_children = 0;

        __asm__ __volatile__("cli");
        hw_spin_lock(&g_sched_lock);
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            hw_task_t *t = &g_tasks[i];
            if (t->ppid != mypid || t->state == HW_TASK_FREE) {
                continue;
            }
            if (want_pid != (uint64_t)-1 && t->pid != (uint32_t)want_pid) {
                continue;
            }
            if (t->state == HW_TASK_ZOMBIE) {
                uint32_t child_pid = t->pid;
                uint64_t code = t->exit_code;
                uint32_t exit_signal = t->exit_signal;
                int status = (exit_signal != 0u)
                             ? (int)(exit_signal & 0x7Fu)
                             : (int)((code & 0xFFull) << 8);
                /* Publishing the slot as FREE is the last thing done to it,
                 * and everything still needed from it is taken first.
                 *
                 * It used to set FREE, drop the lock, and only then call
                 * hw_free_kstack(t) - which both frees pages and writes to the
                 * task. In between, another core allocating a task slot sees
                 * this one free and starts a fork into it, and the two owners
                 * interleave: the reaper then frees the kernel stack the fork
                 * has just allocated, those pages go back on the freelist, and
                 * the allocator hands them out again as page tables. That is
                 * how a live process ends up with a PML4 whose entry zero is a
                 * freelist pointer instead of the kernel - and a core loading
                 * that CR3 stops mid-instruction with no way to report why.
                 *
                 * The freeing happens outside the lock, from locals, because
                 * hw_free_page takes the memory lock and nesting the two would
                 * be a new ordering rule to get wrong. */
                uint64_t kbase = t->kstack_base;
                uint32_t kpages = t->kstack_pages;

                t->kstack_base = 0;
                t->kstack_pages = 0;
                t->state = HW_TASK_FREE; /* reaped; nothing may touch t now */
                hw_spin_unlock(&g_sched_lock);
                hw_free_kstack_pages(kbase, kpages);
                __asm__ __volatile__("sti");
                if (status_ptr != 0 && hw_user_range_ok(status_ptr, 4, 1)) {
                    /* The wait status word: a normal exit puts the code in the
                     * high byte and leaves the low seven bits clear; a signal
                     * death puts the signal number in those low bits. That is
                     * what WIFEXITED and WIFSIGNALED read. */
                    *(volatile int *)(uintptr_t)status_ptr = status;
                }
                return (long)child_pid;
            }
            have_children = 1;
        }
        if (!have_children) {
            hw_spin_unlock(&g_sched_lock);
            __asm__ __volatile__("sti");
            return -VIBEOS_ECHILD;
        }
        /* Block until a child exit sets us READY again (see hw_task_exit). */
        g_tasks[g_current_task].state = HW_TASK_BLOCKED;
        hw_spin_unlock(&g_sched_lock);
        __asm__ __volatile__("sti; hlt" ::: "memory");
    }
}

/* Copy a NUL-terminated string from user space, validating each byte's page. */
static int hw_copy_user_string(uint64_t uptr, char *dst, int max) {
    int i;
    for (i = 0; i < max - 1; i++) {
        if (!hw_user_range_ok(uptr + (uint64_t)i, 1, 0)) {
            return -1;
        }
        dst[i] = *(const char *)(uintptr_t)(uptr + (uint64_t)i);
        if (dst[i] == 0) {
            return 0;
        }
    }
    dst[max - 1] = 0;
    return 0;
}


/* execve(): replace the current process image with an ELF read from the
 * filesystem. On success the trapframe is rewritten to the new program's entry
 * and CR3 switched, so the syscall return path resumes into the new image. The
 * old address space is not reclaimed yet (no PMM free), which is a known leak. */
/* Copy an argument vector out of user memory.
 *
 * It has to be copied, not pointed at: the strings live in the address space
 * that execve is about to destroy. Both the vector and every string it names
 * are attacker-controlled, so each pointer is validated before it is followed
 * and the whole thing is bounded - a program that asks for more arguments than
 * fit gets E2BIG rather than a kernel that walks off the end of an array. */
#define VIBEOS_HW_MAX_ARGV 16
#define VIBEOS_HW_ARG_BYTES 512

typedef struct {
    const char *slot[VIBEOS_HW_MAX_ARGV + 1];
    char store[VIBEOS_HW_ARG_BYTES];
} hw_argv_t;

static long hw_copy_user_argv(uint64_t uvec, hw_argv_t *out) {
    uint32_t count = 0;
    uint32_t used = 0;

    out->slot[0] = 0;
    if (uvec == 0u) {
        return 0;
    }
    for (;;) {
        uint64_t ptr;
        int len;

        if (count == VIBEOS_HW_MAX_ARGV) {
            return -VIBEOS_E2BIG;
        }
        if (!hw_user_range_ok(uvec + (uint64_t)count * 8u, 8, 0)) {
            return -VIBEOS_EFAULT;
        }
        ptr = *(const uint64_t *)(uintptr_t)(uvec + (uint64_t)count * 8u);
        if (ptr == 0u) {
            break;
        }
        if (used >= VIBEOS_HW_ARG_BYTES) {
            return -VIBEOS_E2BIG;
        }
        if (hw_copy_user_string(ptr, &out->store[used],
                                (int)(VIBEOS_HW_ARG_BYTES - used)) != 0) {
            return -VIBEOS_EFAULT;
        }
        out->slot[count] = &out->store[used];
        for (len = 0; out->store[used + (uint32_t)len]; len++) {
            /* measure */
        }
        used += (uint32_t)len + 1u;
        count++;
    }
    out->slot[count] = 0;
    return (long)count;
}

static long hw_sys_execve(vibeos_x86_64_isr_frame_t *frame, uint64_t path_uptr,
                          uint64_t argv_uptr, uint64_t envp_uptr) {
    char path[128];
    hw_proc_t np;
    hw_task_t *t;
    long n;
    uint32_t k;
    static hw_argv_t g_exec_argv;   /* under g_exec_lock, like the image buffer */
    static hw_argv_t g_exec_envp;
    const char *fallback_argv[2];
    const char *const *argv;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    fallback_argv[0] = path;
    fallback_argv[1] = 0;
    /* g_exec_elf is a single shared staging buffer, so the read and the load out
     * of it have to be one critical section: two cores exec'ing at once would
     * otherwise each load the other's image. */
    hw_spin_lock_preemptible(&g_exec_lock);
    {
        long na = hw_copy_user_argv(argv_uptr, &g_exec_argv);
        long ne = hw_copy_user_argv(envp_uptr, &g_exec_envp);
        if (na < 0 || ne < 0) {
            hw_spin_unlock_preemptible(&g_exec_lock);
            return (na < 0) ? na : ne;
        }
        /* A caller that passes no argv still gets an argv[0]: the path it was
         * started from, which is what a program prints as its own name - and
         * what BusyBox uses to decide which applet it is. */
        argv = (na > 0) ? g_exec_argv.slot : (const char *const *)fallback_argv;
    }
    if (hw_exec_cache_hit(path)) {
        n = g_exec_cached_len;   /* already staged, byte for byte */
    } else {
        hw_exec_cache_drop();
        n = vibeos_fs_read_file(&g_rootfs, path, g_exec_elf, g_exec_elf_cap);
        if (n > 0) {
            uint32_t i;
            for (i = 0; i < sizeof(g_exec_cached) - 1u && path[i]; i++) {
                g_exec_cached[i] = path[i];
            }
            g_exec_cached[i] = 0;
            g_exec_cached_len = n;
            /* Clear whatever the previous program left beyond this one.
             *
             * The staging buffer is shared by every exec, so a read that
             * stops short leaves the tail of the last image in place - and
             * the result is not obviously broken, it is a plausible ELF made
             * of two programs. That parses far enough to fail somewhere
             * confusing. Zeroing costs one pass over memory on a path that
             * already read the file, and turns a subtle corruption into a
             * clean parse failure. */
            {
                uint32_t z;
                for (z = (uint32_t)n; z < g_exec_elf_cap; z++) {
                    g_exec_elf[z] = 0;
                }
            }
        }
    }
    if (n <= 0) {
        /* Which of the two it was matters: a file that cannot be read and a
         * file that reads but does not parse are different bugs, and the shell
         * prints the same "cannot exec" for both. */
        /* DEBUG, not ERROR. A shell resolving a bare command name tries it as
         * a path first, and a C runtime asks for /proc/self/exe, which this
         * filesystem does not have. Both miss on every healthy boot, and an
         * error that fires every time teaches you to ignore errors. It is
         * still recorded, so it is in the ring dump if a failure turns out to
         * be about one of these. */
        hw_log(VIBEOS_LOG_DEBUG, 3u, (uint64_t)n, 0,
               "execve could not read the program image");
        vibeos_x86_64_serial_puts("[EXEC] read failed: ");
        vibeos_x86_64_serial_puts(path);
        vibeos_x86_64_serial_puts("\n");
        hw_spin_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_ENOENT;
    }
    if (hw_proc_create(&np, g_exec_elf, (uint64_t)n, argv,
                       g_exec_envp.slot[0] ? g_exec_envp.slot : 0) != 0) {
        hw_log(VIBEOS_LOG_ERROR, 4u, (uint64_t)n, 0,
               "execve rejected the program image");
        vibeos_x86_64_serial_puts("[EXEC] image rejected: ");
        vibeos_x86_64_serial_puts(path);
        vibeos_x86_64_serial_puts(" bytes=0x");
        vibeos_x86_64_serial_print_hex((uint64_t)n);
        vibeos_x86_64_serial_puts("\n");
        hw_spin_unlock_preemptible(&g_exec_lock);
        return -VIBEOS_ENOMEM;
    }
    hw_spin_unlock_preemptible(&g_exec_lock);

    t = &g_tasks[g_current_task];
    /* Stored with a leading slash even when the caller used a relative path.
     * /proc/self/exe is defined to be absolute, and a C runtime does not merely
     * prefer that: glibc asserts on it during startup and aborts the process,
     * which is how the relative form was found. */
    {
        uint32_t w = 0;
        if (path[0] != '/') {
            np.exe_path[w++] = '/';
        }
        for (k = 0; w < (uint32_t)sizeof(np.exe_path) - 1u && path[k]; k++) {
            np.exe_path[w++] = path[k];
        }
        np.exe_path[w] = 0;
    }
    {
        vibeos_hw_aspace_t old_as = t->proc.as; /* reclaim after switching CR3 */
        t->proc = np;
        t->cr3 = hw_proc_cr3(&t->proc);
        t->cr3_set_by = "execve";
        hw_write_cr3(t->cr3);
        hw_aspace_destroy(&old_as);             /* old CR3 no longer active */
    }

    for (k = 0; k < (uint32_t)sizeof(*frame); k++) {
        ((uint8_t *)(void *)frame)[k] = 0;
    }
    /* The old image is gone and its thread-local storage with it. Clear the
     * base here and in the register, because exec returns straight to user
     * space without passing through the scheduler's restore. */
    t->fs_base = 0;
    hw_wrmsr(MSR_FS_BASE, 0);
    /* Caught signals revert to their default: the handler addresses pointed
     * into an image that no longer exists, and jumping to them would enter
     * whatever the new program happens to have at that address. Ignored stays
     * ignored, which is what execve is defined to do. */
    {
        uint32_t sg;
        for (sg = 0; sg < VIBEOS_HW_NSIG; sg++) {
            if (t->sig_handler[sg] != SIG_IGN_ADDR) {
                t->sig_handler[sg] = SIG_DFL_ADDR;
            }
            t->sig_restorer[sg] = 0;
            t->sig_flags[sg] = 0;
        }
        t->sig_pending = 0;
    }

    frame->rip = np.entry;
    frame->cs = VIBEOS_HW_USER_CODE_SEL;
    frame->rflags = 0x202;
    frame->rsp = np.user_sp;
    frame->ss = VIBEOS_HW_USER_DATA_SEL;
    /* One line per exec: what was loaded, how big it was, and where it
      * starts. Enough to tell a failed load from a failed program without
      * being enough to drown the log. */
    vibeos_x86_64_serial_puts("[EXEC] ");
    vibeos_x86_64_serial_puts(path);
    vibeos_x86_64_serial_puts(" bytes=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)n);
    vibeos_x86_64_serial_puts(" entry=0x");
    vibeos_x86_64_serial_print_hex(np.entry);
    vibeos_x86_64_serial_puts("\n");
    return 0; /* frame replaced; syscall return enters the new image */
}

/* ---- what a program needs once it is running ------------------------------
 *
 * Startup gets a libc to main. These are what the program does afterwards:
 * look at files, read directories, ask who it is. The list came from tracing
 * BusyBox rather than from reasoning about it.
 */

static void hw_stat_wr64(uint64_t base, uint32_t off, uint64_t v) {
    uint8_t *p = (uint8_t *)(uintptr_t)(base + off);
    uint32_t i;
    for (i = 0; i < 8u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

static void hw_stat_wr32(uint64_t base, uint32_t off, uint32_t v) {
    uint8_t *p = (uint8_t *)(uintptr_t)(base + off);
    uint32_t i;
    for (i = 0; i < 4u; i++) {
        p[i] = (uint8_t)(v >> (8u * i));
    }
}

/* Fill a struct stat the caller can believe.
 *
 * The mode matters more than it looks: a libc decides how to buffer a stream
 * from it, and a program decides whether to recurse from it. Reporting a
 * regular file for a directory does not fail here - it fails later, inside the
 * program, doing something that made sense given what it was told. */
static long hw_write_stat(uint64_t ubuf, uint32_t mode, uint64_t size, uint64_t ino) {
    uint32_t i;

    if (!hw_user_range_ok(ubuf, STAT_SIZE, 1)) {
        return -VIBEOS_EFAULT;
    }
    for (i = 0; i < STAT_SIZE; i++) {
        ((uint8_t *)(uintptr_t)ubuf)[i] = 0;
    }
    hw_stat_wr64(ubuf, STAT_OFF_INO, ino);
    hw_stat_wr64(ubuf, STAT_OFF_NLINK, 1);
    hw_stat_wr32(ubuf, STAT_OFF_MODE, mode);
    hw_stat_wr32(ubuf, STAT_OFF_UID, 0);
    hw_stat_wr32(ubuf, STAT_OFF_GID, 0);
    hw_stat_wr64(ubuf, STAT_OFF_SIZE, size);
    hw_stat_wr64(ubuf, STAT_OFF_BLKSIZE, 512);
    hw_stat_wr64(ubuf, STAT_OFF_BLOCKS, (size + 511ull) / 512ull);
    return 0;
}

static long hw_sys_fstat(uint64_t fd, uint64_t ubuf) {
    hw_fd_t *f;

    if (fd < 3u) {
        /* The console. Character device, and deliberately not a terminal -
         * the same answer ioctl gives. */
        return hw_write_stat(ubuf, S_IFCHR | 0620u, 0, fd + 1u);
    }
    f = hw_fd_get(fd);
    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->net_sock >= 0) {
        return hw_write_stat(ubuf, S_IFCHR | 0600u, 0, fd + 1u);
    }
    if (f->isdir) {
        return hw_write_stat(ubuf, S_IFDIR | 0755u, 0,
                             f->cluster ? f->cluster : fd + 1u);
    }
    return hw_write_stat(ubuf, S_IFREG | 0644u, f->size, f->cluster ? f->cluster : fd + 1u);
}

/* newfstatat(dirfd, path, buf, flags): stat by name, or by fd when the path is
 * empty and AT_EMPTY_PATH is set. Relative paths resolve against the volume
 * root, which is the only directory there is. */
static long hw_sys_newfstatat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                              uint64_t flags) {
    char path[64];
    uint32_t cluster = 0, size = 0;

    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (path[0] == 0) {
        if ((flags & AT_EMPTY_PATH) == 0) {
            return -VIBEOS_ENOENT;
        }
        return hw_sys_fstat(dirfd, ubuf);
    }
    if (VIBEOS_ARG_INT(dirfd) != AT_FDCWD && dirfd < 3u) {
        return -VIBEOS_EBADF;
    }
    /* The root of the volume, however it is spelled. */
    if ((path[0] == '/' && path[1] == 0) || (path[0] == '.' && path[1] == 0)) {
        return hw_write_stat(ubuf, S_IFDIR | 0755u, 0, 1);
    }
    {
        /* Directory or file? The answer changes what a program does, not just
         * what it prints: ls given a directory lists it and given a file names
         * it, so reporting the wrong one produces a plausible wrong result
         * rather than an error. The filesystem decides; how it decides is its
         * business. */
        vibeos_fs_node_t node;
        if (vibeos_fs_lookup(&g_rootfs, path, &node) != 0) {
            return -VIBEOS_ENOENT;
        }
        cluster = (uint32_t)node.id;
        size = (uint32_t)node.size;
        if (node.is_dir) {
            return hw_write_stat(ubuf, S_IFDIR | 0755u, 0, cluster ? cluster : 2u);
        }
    }
    return hw_write_stat(ubuf, S_IFREG | 0644u, size, cluster ? cluster : 2u);
}

/* openat(): the modern spelling of open. Only AT_FDCWD is accepted, because a
 * directory fd would have to mean something and here it cannot. */
static long hw_sys_openat(uint64_t dirfd, uint64_t path_uptr, uint64_t flags) {
    if (VIBEOS_ARG_INT(dirfd) != AT_FDCWD) {
        return -VIBEOS_ENOSYS;
    }
    return hw_sys_open(path_uptr, flags);
}

/* getcwd(): there is one directory. Saying so is accurate; inventing a path
 * would make a program build filenames that do not resolve. */
static long hw_sys_getcwd(uint64_t ubuf, uint64_t size) {
    if (size < 2u) {
        return -VIBEOS_ERANGE;
    }
    if (!hw_user_range_ok(ubuf, 2, 1)) {
        return -VIBEOS_EFAULT;
    }
    ((char *)(uintptr_t)ubuf)[0] = '/';
    ((char *)(uintptr_t)ubuf)[1] = 0;
    return 2;   /* Linux returns the length including the terminator */
}

/* readlinkat(): the only symlink that exists here is the one a program uses to
 * find itself, and it is answered from what execve was actually given rather
 * than from a made-up path. Everything else is not a link, which is what
 * EINVAL means. */
static long hw_sys_readlinkat(uint64_t dirfd, uint64_t path_uptr, uint64_t ubuf,
                              uint64_t bufsz) {
    char path[64];
    const char *self;
    uint64_t n = 0;

    (void)dirfd;
    if (hw_copy_user_string(path_uptr, path, sizeof(path)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (!(path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' &&
          path[4] == 'c' && path[5] == '/' && path[6] == 's' && path[7] == 'e' &&
          path[8] == 'l' && path[9] == 'f' && path[10] == '/' && path[11] == 'e' &&
          path[12] == 'x' && path[13] == 'e' && path[14] == 0)) {
        return -VIBEOS_EINVAL;
    }
    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    self = g_tasks[g_current_task].proc.exe_path;
    while (self[n]) {
        n++;
    }
    if (n == 0) {
        return -VIBEOS_ENOENT;
    }
    if (n > bufsz) {
        n = bufsz;
    }
    if (!hw_user_range_ok(ubuf, n, 1)) {
        return -VIBEOS_EFAULT;
    }
    {
        uint64_t i;
        for (i = 0; i < n; i++) {
            ((char *)(uintptr_t)ubuf)[i] = self[i];
        }
    }
    return (long)n;   /* not terminated, as Linux does not terminate it */
}

/* prctl(): the process name is the operation programs actually use, and it is
 * stored rather than acknowledged - it costs sixteen bytes and makes the
 * scheduler's log say which program a pid is. */
static long hw_sys_prctl(uint64_t op, uint64_t arg) {
    hw_task_t *t;
    uint32_t i;

    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];
    if (op == PR_SET_NAME) {
        if (!hw_user_range_ok(arg, 16, 0)) {
            return -VIBEOS_EFAULT;
        }
        for (i = 0; i < 15u; i++) {
            t->comm[i] = ((const char *)(uintptr_t)arg)[i];
            if (t->comm[i] == 0) {
                break;
            }
        }
        t->comm[15] = 0;
        return 0;
    }
    if (op == PR_GET_NAME) {
        if (!hw_user_range_ok(arg, 16, 1)) {
            return -VIBEOS_EFAULT;
        }
        for (i = 0; i < 16u; i++) {
            ((char *)(uintptr_t)arg)[i] = t->comm[i];
        }
        return 0;
    }
    return -VIBEOS_EINVAL;
}

/* What happens to a signal nobody handles.
 *
 * Getting this table wrong is not a small error: an ignored SIGCHLD that kills
 * the process, or a SIGTERM that is quietly dropped, both look like the
 * program misbehaving rather than the kernel. */
static int hw_signal_default_kills(uint32_t sig) {
    switch (sig) {
        case VIBEOS_SIGCHLD:
        case VIBEOS_SIGCONT:
        case VIBEOS_SIGWINCH:
            return 0;   /* ignored by default */
        default:
            return 1;
    }
}

/* Raise a signal against a task. Does not deliver it: delivery happens on the
 * way back to user space, on that task's own stack, which is the only place a
 * signal frame can safely be built. Raising can happen from an interrupt, from
 * another CPU, or from the task itself, and none of those own that stack. */
static int hw_signal_raise(int task_index, uint32_t sig) {
    if (task_index < 0 || task_index >= VIBEOS_HW_MAX_TASKS || sig == 0u || sig >= VIBEOS_HW_NSIG) {
        return -1;
    }
    if (!g_tasks[task_index].is_user || g_tasks[task_index].state == HW_TASK_FREE) {
        return -1;
    }
    /* SIGKILL and SIGSTOP cannot be caught or blocked. Honouring a handler for
     * them would make a process unkillable. */
    if (sig != VIBEOS_SIGKILL && sig != VIBEOS_SIGSTOP &&
        g_tasks[task_index].sig_handler[sig] == SIG_IGN_ADDR) {
        return 0;   /* explicitly ignored: raised and discarded, as Linux does */
    }
    __sync_fetch_and_or(&g_tasks[task_index].sig_pending, 1ull << sig);
    /* A task asleep in read() has to wake up to notice. */
    if (g_tasks[task_index].state == HW_TASK_BLOCKED) {
        g_tasks[task_index].wait_input = 0;
        g_tasks[task_index].state = HW_TASK_READY;
        HW_TASK_MARK(task_index, ready_by, "signal_wake");
    }
    return 0;
}

static int hw_task_by_pid(uint32_t pid) {
    int i;
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (g_tasks[i].is_user && g_tasks[i].state != HW_TASK_FREE &&
            g_tasks[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

/* There is currently one thread per process, so Linux tid and pid are equal.
 * Keep this lookup separate from kill(2), because tkill/tgkill address a
 * thread rather than a process group. */
static int hw_task_by_tid(uint32_t tid) {
    return hw_task_by_pid(tid);
}

static long hw_sys_kill(uint64_t target_pid, uint64_t sig) {
    int target;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if (sig >= VIBEOS_HW_NSIG) {
        return -VIBEOS_EINVAL;
    }
    target = hw_task_by_pid((uint32_t)target_pid);
    if (target < 0) {
        return -VIBEOS_ESRCH;
    }
    if (sig == 0u) {
        return 0;   /* the existence check, and it exists */
    }
    return (hw_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
}

static long hw_sys_tkill(uint64_t target_tid, uint64_t sig) {
    int target;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user ||
        sig >= VIBEOS_HW_NSIG) {
        return -VIBEOS_EINVAL;
    }
    target = hw_task_by_tid((uint32_t)target_tid);
    if (target < 0) {
        return -VIBEOS_ESRCH;
    }
    if (sig == 0u) {
        return 0;
    }
    vibeos_x86_64_serial_puts("[SIG] tkill tid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)(uint32_t)target_tid);
    vibeos_x86_64_serial_puts(" sig=0x");
    vibeos_x86_64_serial_print_hex(sig);
    vibeos_x86_64_serial_puts("\n");
    return (hw_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
}

static long hw_sys_tgkill(uint64_t target_tgid, uint64_t target_tid,
                          uint64_t sig) {
    int target;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user ||
        sig >= VIBEOS_HW_NSIG) {
        return -VIBEOS_EINVAL;
    }
    target = hw_task_by_tid((uint32_t)target_tid);
    if (target < 0 || g_tasks[target].pid != (uint32_t)target_tgid) {
        return -VIBEOS_ESRCH;
    }
    if (sig == 0u) {
        return 0;
    }
    vibeos_x86_64_serial_puts("[SIG] tgkill tgid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)(uint32_t)target_tgid);
    vibeos_x86_64_serial_puts(" tid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)(uint32_t)target_tid);
    vibeos_x86_64_serial_puts(" sig=0x");
    vibeos_x86_64_serial_print_hex(sig);
    vibeos_x86_64_serial_puts("\n");
    return (hw_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
}

/* rt_sigaction(): install, or report, the disposition of one signal. */
static long hw_sys_rt_sigaction(uint64_t sig, uint64_t act_uptr, uint64_t old_uptr) {
    hw_task_t *t;

    if (g_current_task < 0 || sig == 0u || sig >= VIBEOS_HW_NSIG) {
        return -VIBEOS_EINVAL;
    }
    if (sig == VIBEOS_SIGKILL || sig == VIBEOS_SIGSTOP) {
        return -VIBEOS_EINVAL;   /* neither can be caught */
    }
    t = &g_tasks[g_current_task];

    /* struct sigaction: handler at 0, flags at 8, restorer at 16, mask at 24. */
    if (old_uptr != 0u) {
        if (!hw_user_range_ok(old_uptr, 32, 1)) {
            return -VIBEOS_EFAULT;
        }
        ((uint64_t *)(uintptr_t)old_uptr)[0] = t->sig_handler[sig];
        ((uint64_t *)(uintptr_t)old_uptr)[1] = t->sig_flags[sig];
        ((uint64_t *)(uintptr_t)old_uptr)[2] = t->sig_restorer[sig];
        ((uint64_t *)(uintptr_t)old_uptr)[3] = t->sig_mask[sig] >> 1;
    }
    if (act_uptr != 0u) {
        const uint64_t *act;
        if (!hw_user_range_ok(act_uptr, 32, 0)) {
            return -VIBEOS_EFAULT;
        }
        act = (const uint64_t *)(uintptr_t)act_uptr;
        t->sig_handler[sig] = act[0];
        t->sig_flags[sig] = act[1];
        t->sig_restorer[sig] = act[2];
        t->sig_mask[sig] = act[3] << 1;
    }
    return 0;
}

/* rt_sigprocmask(): SIG_BLOCK 0, SIG_UNBLOCK 1, SIG_SETMASK 2. */
/* A Linux sigset_t numbers its bits from zero: bit 0 is signal 1. This kernel
 * numbers them by signal, so bit 9 is signal 9, which keeps every comparison
 * against a signal constant readable. The two representations are converted at
 * the boundary, and only here - getting it wrong shifts every mask by one, so
 * a program blocking SIGUSR2 actually blocks SIGSEGV and its own blocked
 * signal is delivered anyway. */
static uint64_t hw_sigset_from_user(uint64_t user_set) {
    return user_set << 1;
}

static uint64_t hw_sigset_to_user(uint64_t kernel_set) {
    return kernel_set >> 1;
}

static long hw_sys_rt_sigprocmask(uint64_t how, uint64_t set_uptr, uint64_t old_uptr) {
    hw_task_t *t;
    uint64_t set = 0;

    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];
    if (old_uptr != 0u) {
        if (!hw_user_range_ok(old_uptr, 8, 1)) {
            return -VIBEOS_EFAULT;
        }
        *(uint64_t *)(uintptr_t)old_uptr = hw_sigset_to_user(t->sig_blocked);
    }
    if (set_uptr == 0u) {
        return 0;
    }
    if (!hw_user_range_ok(set_uptr, 8, 0)) {
        return -VIBEOS_EFAULT;
    }
    set = hw_sigset_from_user(*(const uint64_t *)(uintptr_t)set_uptr);
    switch (how) {
        case 0: t->sig_blocked |= set; break;
        case 1: t->sig_blocked &= ~set; break;
        case 2: t->sig_blocked = set; break;
        default: return -VIBEOS_EINVAL;
    }
    /* Blocking these would make a process unkillable, so the request is
     * accepted and the two bits are dropped, exactly as Linux does. */
    t->sig_blocked &= ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    return 0;
}

/* setuid()/setgid(): there is one identity and it is root. Becoming it again
 * succeeds; becoming anyone else is refused rather than pretended. */
static long hw_sys_setresid(uint64_t id) {
    return (id == 0u) ? 0 : -VIBEOS_EPERM;
}

/* ---- what a C runtime asks for before it runs the program ----------------
 *
 * These are not conveniences. A static libc executes a fixed opening sequence
 * and dies if any of it fails: it installs thread-local storage, registers a
 * thread id, asks whether its output is a terminal, and only then reaches
 * main. Each one below either does the real thing or returns the error Linux
 * returns, because a syscall that reports a success it did not perform makes
 * the program fail later, somewhere unrelated, with nothing pointing back
 * here.
 */

/* arch_prctl(): the one everything else depends on.
 *
 * On x86-64 a C runtime addresses its thread state through %fs - errno, the
 * stack-protector cookie, the locale pointer. The base of that segment lives
 * in a model-specific register, so setting it is privileged and a program
 * cannot do it itself. Until this works, a libc faults on its first line. */
/* Is this an address a process is allowed to own? There are two user windows -
 * the high one VibeOS programs are linked into and the low one a Linux
 * executable is linked into - and the answer lives in one place so a new
 * caller cannot accidentally know about only one of them. */
static int hw_user_addr_ok(uint64_t va) {
    if (va >= VIBEOS_HW_USER_BASE && va < VIBEOS_HW_USER_BASE + 0x8000000000ull) {
        return 1;
    }
    return va >= VIBEOS_HW_LOW_USER_BASE && va < VIBEOS_HW_LOW_USER_LIMIT;
}

static long hw_sys_arch_prctl(uint64_t code, uint64_t addr) {
    hw_task_t *t;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];

    switch (code) {
        case ARCH_SET_FS:
            /* A non-canonical address in this MSR faults on the wrmsr itself,
             * in ring 0 - user space must not be able to reach that. So the
             * base is checked before the write.
             *
             * It has to accept both user windows, and getting that wrong is
             * not a subtle failure: a static musl binary keeps its thread
             * pointer in its own .bss, down in the low window, and reacts to a
             * refusal by executing hlt on purpose. The kernel then reports a
             * general-protection fault in ring 3 with nothing to say that a
             * bounds check three functions away was the cause. */
            if (!hw_user_addr_ok(addr)) {
                return -VIBEOS_EPERM;
            }
            t->fs_base = addr;
            hw_wrmsr(MSR_FS_BASE, addr);
            return 0;
        case ARCH_GET_FS:
            if (!hw_user_range_ok(addr, 8, 1)) {
                return -VIBEOS_EFAULT;
            }
            *(uint64_t *)(uintptr_t)addr = t->fs_base;
            return 0;
        case ARCH_SET_GS:
        case ARCH_GET_GS:
            /* %gs holds this CPU's per-CPU block. Handing it to a program
             * would let ring 3 relocate the kernel's own state. */
            return -VIBEOS_EPERM;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ioctl(): there is no terminal device here. ENOTTY is not a shortcut, it is
 * the truthful answer - and it is the answer a libc uses to decide that
 * stdout is a file or a pipe and should be block buffered. */
static long hw_sys_ioctl(uint64_t fd, uint64_t req) {
    (void)req;
    if (fd >= 3u && !hw_fd_get(fd)) {
        return -VIBEOS_EBADF;
    }
    return -VIBEOS_ENOTTY;
}

/* writev()/readv(): scatter-gather over the existing single-buffer paths. The
 * iovec array is itself user memory, so it is validated like any other user
 * pointer before being walked. */
typedef struct {
    uint64_t base;
    uint64_t len;
} hw_iovec_t;

static long hw_sys_writev(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt) {
    long total = 0;
    uint64_t i;

    if (iovcnt > 1024u) {
        return -VIBEOS_EINVAL;   /* Linux caps this at UIO_MAXIOV */
    }
    if (!hw_user_range_ok(iov_uptr, iovcnt * sizeof(hw_iovec_t), 0)) {
        return -VIBEOS_EFAULT;
    }
    for (i = 0; i < iovcnt; i++) {
        const hw_iovec_t *v = (const hw_iovec_t *)(uintptr_t)
                              (iov_uptr + i * sizeof(hw_iovec_t));
        long n;
        if (v->len == 0u) {
            continue;
        }
        n = hw_sys_write(fd, v->base, v->len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v->len) {
            break;   /* a short write ends the call, as it does on Linux */
        }
    }
    return total;
}

static long hw_sys_readv(uint64_t fd, uint64_t iov_uptr, uint64_t iovcnt) {
    long total = 0;
    uint64_t i;

    if (iovcnt > 1024u) {
        return -VIBEOS_EINVAL;
    }
    if (!hw_user_range_ok(iov_uptr, iovcnt * sizeof(hw_iovec_t), 0)) {
        return -VIBEOS_EFAULT;
    }
    for (i = 0; i < iovcnt; i++) {
        const hw_iovec_t *v = (const hw_iovec_t *)(uintptr_t)
                              (iov_uptr + i * sizeof(hw_iovec_t));
        long n;
        if (v->len == 0u) {
            continue;
        }
        n = hw_sys_read(fd, v->base, v->len);
        if (n < 0) {
            return total > 0 ? total : n;
        }
        total += n;
        if ((uint64_t)n < v->len) {
            break;
        }
    }
    return total;
}

/* uname(): six fixed 65-byte fields, in order. Programs branch on the release
 * string, so it carries a real version number rather than a placeholder. */
static long hw_sys_uname(uint64_t buf) {
    static const char *const fields[6] = {
        "Linux",            /* sysname: the ABI implemented here, which is    */
                            /* what the question is actually about            */
        "vibeos",           /* nodename   */
        "6.1.0-vibeos",     /* release    */
        "VibeOS",           /* version    */
        "x86_64",           /* machine    */
        "(none)"            /* domainname */
    };
    uint32_t f, i;

    if (!hw_user_range_ok(buf, 6u * 65u, 1)) {
        return -VIBEOS_EFAULT;
    }
    for (f = 0; f < 6u; f++) {
        char *dst = (char *)(uintptr_t)(buf + (uint64_t)f * 65u);
        const char *src = fields[f];
        for (i = 0; i < 65u; i++) {
            dst[i] = (i < 64u) ? src[i] : 0;
            if (!dst[i]) {
                break;
            }
        }
        for (; i < 65u; i++) {
            dst[i] = 0;
        }
    }
    return 0;
}

/* clock_gettime(): derived from the timer tick, so it advances at the
 * resolution the timer really has rather than pretending to a nanosecond
 * accuracy it does not possess. */
static long hw_sys_clock_gettime(uint64_t clk, uint64_t ts_uptr) {
    uint64_t ticks = g_timer_ticks;
    uint64_t *ts;

    (void)clk;   /* monotonic and realtime are one clock here: uptime */
    if (!hw_user_range_ok(ts_uptr, 16, 1)) {
        return -VIBEOS_EFAULT;
    }
    ts = (uint64_t *)(uintptr_t)ts_uptr;
    ts[0] = ticks / VIBEOS_HW_TIMER_HZ;
    ts[1] = (ticks % VIBEOS_HW_TIMER_HZ) * (1000000000ull / VIBEOS_HW_TIMER_HZ);
    return 0;
}

static long hw_sys_time(uint64_t tptr) {
    uint64_t secs = g_timer_ticks / VIBEOS_HW_TIMER_HZ;

    if (tptr != 0u) {
        if (!hw_user_range_ok(tptr, 8, 1)) {
            return -VIBEOS_EFAULT;
        }
        *(uint64_t *)(uintptr_t)tptr = secs;
    }
    return (long)secs;
}

/* prlimit64(): report the limits that are real here. The stack is the one a
 * runtime acts on - some size a guard region from it. */
static long hw_sys_prlimit64(uint64_t resource, uint64_t new_uptr, uint64_t old_uptr) {
    if (new_uptr != 0u) {
        return -VIBEOS_EPERM;   /* the limits here are fixed by the layout */
    }
    if (old_uptr == 0u) {
        return 0;
    }
    if (!hw_user_range_ok(old_uptr, 16, 1)) {
        return -VIBEOS_EFAULT;
    }
    {
        uint64_t *rl = (uint64_t *)(uintptr_t)old_uptr;
        switch (resource) {
            case 3: /* RLIMIT_STACK */
                rl[0] = (uint64_t)VIBEOS_HW_USER_STACK_PAGES * 4096ull;
                rl[1] = rl[0];
                break;
            case 7: /* RLIMIT_NOFILE */
                rl[0] = 3u + VIBEOS_HW_MAX_FDS;
                rl[1] = rl[0];
                break;
            default:
                rl[0] = 0xFFFFFFFFFFFFFFFFull;   /* RLIM64_INFINITY */
                rl[1] = rl[0];
                break;
        }
    }
    return 0;
}

/* futex(): one thread per process here, so there is never another thread to
 * wake or to wait for. WAKE woke nobody, which is 0. WAIT would deadlock, and
 * EAGAIN is what Linux returns when the value already moved - an outcome every
 * caller is written to handle. Real futexes belong with real threads, not
 * before them. */
static long hw_sys_futex(uint64_t op) {
    switch (op & FUTEX_CMD_MASK) {
        case FUTEX_WAKE:
            return 0;
        case FUTEX_WAIT:
            return -VIBEOS_EAGAIN;
        default:
            return -VIBEOS_ENOSYS;
    }
}

/* ---- delivering a signal ---------------------------------------------------
 *
 * A signal is delivered by making the interrupted program call the handler and
 * then return to where it was. The kernel does that by building a frame on the
 * program's own stack holding the entire interrupted register state, pointing
 * the return address at a trampoline the C library supplied, and rewriting the
 * trapframe so the resume goes to the handler instead of the interrupted
 * instruction. rt_sigreturn later reads that frame back.
 *
 * The frame goes below the red zone. The System V ABI lets a leaf function use
 * the 128 bytes below the stack pointer without reserving them, so writing a
 * frame at rsp would corrupt live data in a program that was doing nothing
 * wrong.
 */

/* Saved on the user stack across a handler. The layout is private to this
 * kernel - only the code that writes it and rt_sigreturn read it - so it holds
 * the whole trapframe rather than a Linux-compatible ucontext, which would
 * matter only to a program that inspects it. */
typedef struct {
    uint64_t magic;
    uint64_t blocked;
    vibeos_x86_64_isr_frame_t frame;
} hw_sigframe_t;

#define HW_SIGFRAME_MAGIC 0x5649424553494721ull   /* "VIBESIG!" */

/* Which signal to deliver next: the lowest-numbered pending one that is not
 * blocked. Lowest first is what Linux does, and it puts the fatal ones - which
 * are the low numbers - ahead of the informational ones. */
static uint32_t hw_signal_next(hw_task_t *t) {
    uint64_t ready = t->sig_pending & ~t->sig_blocked;
    uint32_t sig;

    /* SIGKILL and SIGSTOP ignore the mask entirely. */
    ready |= t->sig_pending & ((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    if (ready == 0u) {
        return 0;
    }
    for (sig = 1; sig < VIBEOS_HW_NSIG; sig++) {
        if (ready & (1ull << sig)) {
            return sig;
        }
    }
    return 0;
}

/* Called on the way back to user space. Returns non-zero if the frame was
 * rewritten to enter a handler. May not return at all, if the signal kills. */
static int hw_signal_deliver(vibeos_x86_64_isr_frame_t *frame) {
    hw_task_t *t;
    uint32_t sig;
    uint64_t handler, sp;
    hw_sigframe_t *sf;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return 0;
    }
    t = &g_tasks[g_current_task];
    for (;;) {
        sig = hw_signal_next(t);
        if (sig == 0u) {
            return 0;
        }
        t->sig_pending &= ~(1ull << sig);

        handler = t->sig_handler[sig];
        if (sig == VIBEOS_SIGKILL || sig == VIBEOS_SIGSTOP) {
            handler = SIG_DFL_ADDR;   /* uncatchable */
        }
        if (handler == SIG_IGN_ADDR) {
            continue;
        }
        if (handler == SIG_DFL_ADDR) {
            if (hw_signal_default_kills(sig)) {
                t->exit_signal = sig;
                hw_task_exit(128ull + sig);   /* does not return */
            }
            continue;   /* default is to ignore it */
        }
        break;
    }

    vibeos_x86_64_serial_puts("[SIG] deliver sig=0x");
    vibeos_x86_64_serial_print_hex(sig);
    vibeos_x86_64_serial_puts(" handler=0x");
    vibeos_x86_64_serial_print_hex(handler);
    vibeos_x86_64_serial_puts("\n");


    /* Below the red zone, then aligned. The handler is entered as if by a
     * call, so it wants rsp % 16 == 8 once the return address is pushed. */
    sp = frame->rsp - 128ull;
    sp -= sizeof(hw_sigframe_t);
    sp &= ~15ull;
    sp -= 8ull;   /* room for the return address */

    if (!hw_user_range_ok(sp, sizeof(hw_sigframe_t) + 8ull, 1)) {
        /* No usable stack to deliver on. A program cannot be asked to handle
         * that, so the signal takes its default action instead of being
         * silently dropped. */
        hw_task_exit(128ull + sig);
        return 0;
    }

    sf = (hw_sigframe_t *)(uintptr_t)(sp + 8ull);
    sf->magic = HW_SIGFRAME_MAGIC;
    sf->blocked = t->sig_blocked;
    sf->frame = *frame;

    /* The return address is the C library's trampoline, which issues
     * rt_sigreturn. Without SA_RESTORER there is nothing to return to, and a
     * handler that returns would jump to whatever was on the stack. */
    if ((t->sig_flags[sig] & VIBEOS_SA_RESTORER) == 0u || t->sig_restorer[sig] == 0u) {
        hw_task_exit(128ull + sig);
        return 0;
    }
    *(uint64_t *)(uintptr_t)sp = t->sig_restorer[sig];

    /* While the handler runs, this signal is blocked, plus whatever the
     * program asked to block along with it - otherwise a repeating signal
     * re-enters the handler until the stack is gone. */
    t->sig_blocked |= (1ull << sig) | t->sig_mask[sig];

    frame->rip = handler;
    frame->rsp = sp;
    frame->rdi = sig;    /* the handler's first argument */
    frame->rsi = 0;
    frame->rdx = 0;
    frame->rax = 0;
    return 1;
}

/* rt_sigreturn(): put back everything the handler interrupted. */
static long hw_sys_rt_sigreturn(vibeos_x86_64_isr_frame_t *frame) {
    hw_task_t *t;
    const hw_sigframe_t *sf;
    uint64_t base;

    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];
    /* The handler has returned, so rsp points just past the return address
     * that the trampoline popped. */
    base = frame->rsp - 8ull + 8ull;
    if (!hw_user_range_ok(base, sizeof(hw_sigframe_t), 0)) {
        hw_task_exit(128ull + VIBEOS_SIGSEGV);
        return 0;
    }
    sf = (const hw_sigframe_t *)(uintptr_t)base;
    if (sf->magic != HW_SIGFRAME_MAGIC) {
        /* Somebody called rt_sigreturn without a frame, or overwrote it.
         * Resuming from whatever is on the stack would hand ring 3 a chance to
         * pick its own cs and rflags. */
        hw_task_exit(128ull + VIBEOS_SIGSEGV);
        return 0;
    }
    {
        vibeos_x86_64_isr_frame_t restored = sf->frame;
        t->sig_blocked = sf->blocked;
        /* Only user state is restored, and the segment selectors are forced
         * back to the user ones: the frame is in memory the program can write,
         * so nothing read out of it may decide privilege. */
        restored.cs = VIBEOS_HW_USER_CODE_SEL;
        restored.ss = VIBEOS_HW_USER_DATA_SEL;
        restored.rflags = (restored.rflags & 0x0000000000000CD5ull) | 0x202ull;
        *frame = restored;
    }
    return (long)frame->rax;
}

/* Linux ABI entry: nr in rax, args in rdi/rsi/rdx, with the full trapframe
 * available (fork needs it). Reached from both the native `syscall` trampoline
 * and the int 0x80 gate. Each number is offered to the Linux personality
 * (user/compat/linux) so the portable, host-tested translation model sees and
 * accounts for every real syscall. */
long vibeos_x86_64_linux_syscall(vibeos_x86_64_isr_frame_t *frame,
                                 uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    uint32_t native = 0;
    (void)vibeos_linux_translate_syscall(&g_compat_rt, (uint32_t)nr, &native);

    switch (nr) {
        case LSYS_fork:
            return hw_sys_fork(frame);
        case LSYS_vfork:
            /* ash uses vfork for the short child->exec path. A real vfork
             * shares the parent's address space until exec, but returning a
             * private fork here preserves the observable contract while
             * avoiding a parent that can be modified by a still-running child.
             * The child is constrained by the same exec/exit ABI as vfork's
             * supported use in this personality. */
            return hw_sys_fork(frame);
        case LSYS_execve:
            return hw_sys_execve(frame, a1, a2, a3);
        case LSYS_open:
            return hw_sys_open(a1, a2);
        case LSYS_close:
            return hw_sys_close(a1);
        case LSYS_lseek:
            return hw_sys_lseek(a1, a2, a3);
        case LSYS_getdents64:
            return hw_sys_getdents64(a1, a2, a3);
        case LSYS_unlink:
            return hw_sys_unlink(a1);
        case LSYS_mkdir:
            return hw_sys_mkdir(a1);
        case LSYS_wait4:
            return hw_sys_waitpid(a1, a2);
        case LSYS_write:
            return hw_sys_write(a1, a2, a3);
        case LSYS_read:
            return hw_sys_read(a1, a2, a3);
        case LSYS_brk:
            return hw_sys_brk(a1);
        case LSYS_mmap:
            /* addr, len, prot in the first three; flags and fd are in r10 and
             * r8, which the trapframe carries. */
            return hw_sys_mmap(a1, a2, a3, frame->r10, frame->r8);
        case LSYS_mprotect:
            return hw_sys_mprotect(a1, a2, a3);
        case LSYS_munmap:
            return hw_sys_munmap(a1, a2);
        case LSYS_getpid:
            return (g_current_task >= 0) ? (long)g_tasks[g_current_task].pid : 1;

        /* The opening sequence of a real C runtime. */
        case LSYS_arch_prctl:
            return hw_sys_arch_prctl(a1, a2);
        case LSYS_ioctl:
            return hw_sys_ioctl(a1, a2);
        case LSYS_writev:
            return hw_sys_writev(a1, a2, a3);
        case LSYS_readv:
            return hw_sys_readv(a1, a2, a3);
        case LSYS_uname:
            return hw_sys_uname(a1);
        case LSYS_clock_gettime:
            return hw_sys_clock_gettime(a1, a2);
        case LSYS_prlimit64:
            return hw_sys_prlimit64(a2, a3, frame->r10);
        case LSYS_futex:
            return hw_sys_futex(a2);
        case LSYS_gettid:
            /* One thread per process here, so the thread id is the process
             * id - which is exactly what Linux reports for a single-threaded
             * program too. */
            return (g_current_task >= 0) ? (long)g_tasks[g_current_task].pid : 1;
        case LSYS_set_tid_address:
            /* The clear-on-exit address is only read when a thread exits and
             * something waits on it; with no threads there is nothing to
             * notify. The return value - the caller's tid - is what a libc
             * actually stores. */
            return (g_current_task >= 0) ? (long)g_tasks[g_current_task].pid : 1;
        case LSYS_set_robust_list:
            /* The list is walked by the kernel when a thread dies holding a
             * robust mutex. No threads, no robust mutexes, nothing to walk. */
            return 0;
        case LSYS_rseq:
            /* Restartable sequences are an optimisation with a mandatory
             * fallback. Reporting ENOSYS makes the libc take that fallback;
             * claiming success would make it run a fast path this kernel does
             * not implement. */
            return -VIBEOS_ENOSYS;
        case LSYS_getrandom:
            /* There is no entropy source here yet. Returning predictable bytes
             * from the syscall a program uses for keys is worse than refusing:
             * ENOSYS is visible, weak randomness is not. */
            return -VIBEOS_ENOSYS;
        case LSYS_rt_sigaction:
            return hw_sys_rt_sigaction(a1, a2, a3);
        case LSYS_rt_sigprocmask:
            return hw_sys_rt_sigprocmask(a1, a2, a3);
        case LSYS_sched_yield:
            /* Give up the rest of this slice honestly: hlt parks the CPU until
             * the next timer interrupt, which is where the switch happens. */
            __asm__ __volatile__("sti; hlt");
            return 0;
        /* What a program does once it is running. */
        case LSYS_fstat:
            return hw_sys_fstat(a1, a2);
        case LSYS_newfstatat:
            return hw_sys_newfstatat(a1, a2, a3, frame->r10);
        case LSYS_openat:
            return hw_sys_openat(a1, a2, a3);
        case LSYS_pipe:
            return hw_sys_pipe2(a1, 0);
        case LSYS_pipe2:
            return hw_sys_pipe2(a1, a2);
        case LSYS_dup2:
            return hw_sys_dup2(a1, a2);
        case LSYS_dup: {
            /* dup() is dup2() onto the lowest free descriptor. */
            hw_task_t *dt;
            int i;
            if (g_current_task < 0) {
                return -VIBEOS_EINVAL;
            }
            dt = &g_tasks[g_current_task];
            for (i = 0; i < VIBEOS_HW_MAX_FDS; i++) {
                if (!dt->fds[i].used) {
                    return hw_sys_dup2(a1, (uint64_t)(3 + i));
                }
            }
            return -VIBEOS_EMFILE;
        }
        case LSYS_getcwd:
            return hw_sys_getcwd(a1, a2);
        case LSYS_readlinkat:
            return hw_sys_readlinkat(a1, a2, a3, frame->r10);
        case LSYS_prctl:
            return hw_sys_prctl(a1, a2);
        case LSYS_setuid:
        case LSYS_setgid:
            return hw_sys_setresid(a1);
        case LSYS_time:
            return hw_sys_time(a1);
        case LSYS_clone:
            /* A C library does not call fork(); it calls clone() with the
             * flags that happen to mean fork - a new address space, a new
             * process, SIGCHLD to the parent. Anything sharing the address
             * space is a thread, which this kernel does not have, and saying
             * ENOSYS is better than handing back something that looks like a
             * thread and is not. */
            if ((a1 & (CLONE_VM | CLONE_THREAD)) != 0u) {
                return -VIBEOS_ENOSYS;
            }
            return hw_sys_fork(frame);
        case LSYS_getppid:
            return (g_current_task >= 0) ? (long)g_tasks[g_current_task].ppid : 0;
        case LSYS_rt_sigreturn:
            return hw_sys_rt_sigreturn(frame);
        case LSYS_kill:
            return hw_sys_kill(a1, a2);
        case LSYS_tkill:
            /* raise() goes through tkill, not kill: a library raising a signal
             * in itself targets its own thread, and with one thread per
             * process that is the same destination. */
            return hw_sys_tkill(a1, a2);
        case LSYS_tgkill:
            /* tgkill(tgid, tid, sig): one thread per process, so the thread id
             * is the process id and this is kill with an extra argument. */
            return hw_sys_tgkill(a1, a2, a3);
        case LSYS_sendfile:
            /* Every caller of sendfile has to cope with it failing, and does:
             * a read-and-write loop is the documented fallback. Refusing is
             * therefore free, while serving it would mean a second copy of the
             * file and console paths purely to move bytes between kernel
             * buffers. */
            return -VIBEOS_ENOSYS;

        case LSYS_getuid:
        case LSYS_geteuid:
        case LSYS_getgid:
        case LSYS_getegid:
            /* Everything runs as the one identity this system has. */
            return 0;
        /* Sockets. The Linux ABI passes the 4th, 5th and 6th arguments in r10,
         * r8 and r9; the trapframe has them, so read them straight from it. */
        case LSYS_socket:
            return hw_sys_socket(a1, a2);
        case LSYS_connect:
            return hw_sys_connect(a1, a2);
        case LSYS_accept:
            return hw_sys_accept(a1, a2);
        case LSYS_sendto:
            return hw_sys_sendto(a1, a2, a3, frame->r8);
        case LSYS_recvfrom:
            return hw_sys_recvfrom(a1, a2, a3, frame->r8);
        case LSYS_bind:
            return hw_sys_bind(a1, a2);
        case LSYS_listen:
            return hw_sys_listen(a1);
        case LSYS_netctl:
            return hw_sys_netctl(a1, a2);
        case LSYS_exit:
        case LSYS_exit_group:
            hw_task_exit(a1); /* retires this task and switches away; no return */
            return 0;
        default:
            vibeos_x86_64_serial_puts("[HW][SYS] unimplemented Linux syscall nr=0x");
            vibeos_x86_64_serial_print_hex(nr);
            vibeos_x86_64_serial_puts("\n");
            return -VIBEOS_ENOSYS;
    }
}

/* Entry point for the `syscall` trampoline (isr.S): pull the Linux ABI
 * arguments out of the trapframe and store the result back into rax. */
void vibeos_x86_64_syscall_dispatch(vibeos_x86_64_isr_frame_t *frame) {
    long result = vibeos_x86_64_linux_syscall(frame, frame->rax, frame->rdi,
                                              frame->rsi, frame->rdx);
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

static int hw_net_tx(void *ctx, const void *frame, uint32_t len) {
    (void)ctx;
    return vibeos_x86_64_virtio_net_send(frame, len);
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
    hw_spin_lock(&g_net_lock);
    while (budget-- > 0) {
        n = vibeos_x86_64_virtio_net_recv(g_net_rxframe, (uint32_t)sizeof(g_net_rxframe));
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

    if (vibeos_x86_64_virtio_net_init() != 0) {
        vibeos_x86_64_serial_puts("[NET] no network interface; networking disabled\n");
        return;
    }
    if (vibeos_inet_init(&g_net, vibeos_x86_64_virtio_net_mac(), hw_net_tx, 0) != 0) {
        vibeos_x86_64_serial_puts("[NET] stack init failed\n");
        return;
    }
    g_net_up = 1;

    vibeos_x86_64_serial_puts("[NET] requesting a DHCP lease\n");
    /* Under the lock: this transmits, and the timer's pump is already live. */
    hw_spin_lock(&g_net_lock);
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
            hw_spin_lock(&g_net_lock);
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

        hw_spin_lock(&g_net_lock);
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
    hw_spin_lock(&g_net_lock);
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
    g_tasks[idle].state = HW_TASK_RUNNING;
    g_tasks[idle].on_cpu = 1;       /* this core is about to enter it */
    hw_set_kernel_stack(g_tasks[idle].kstack_top);

    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[SMP] cpu online: lapic_id=0x");
    vibeos_x86_64_serial_print_hex(cpu->lapic_id);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();

    /* Tell the BSP we made it, then start this core's own preemption clock and
     * fall into the idle task; the timer will hand us real work. */
    __asm__ __volatile__("sfence" ::: "memory");
    vibeos_x86_64_ap_alive = 1;
    vibeos_x86_64_lapic_timer_start(VIBEOS_HW_TIMER_HZ, VIBEOS_HW_IRQ_TIMER);
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
    if (vibeos_x86_64_ioapic_route(12u, 44u, bsp_id) != 0) {
        vibeos_x86_64_serial_puts("[APIC] failed to route the mouse IRQ\n");
    }
    if (vibeos_x86_64_ioapic_route(1u, 33u, bsp_id) != 0) {
        vibeos_x86_64_serial_puts("[APIC] failed to route the keyboard IRQ\n");
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

static void hw_sched_bringup(const vibeos_boot_info_t *boot_info) {
    const unsigned char *init_elf = vibeos_user_hello_elf;
    uint64_t init_len = vibeos_user_hello_elf_len;
    int hello_id, a_id, b_id, kern_id;
    uint64_t translated = 0, denied = 0;
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
    (void)vibeos_compat_init(&g_compat_rt);
    (void)vibeos_compat_enable(&g_compat_rt, VIBEOS_COMPAT_TARGET_LINUX, 1);

    /* Keyboard is live (IRQ1 unmasked). Seed a test line so the blocking read()
     * path is exercised on the non-interactive CI console; real keystrokes fill
     * the same ring on hardware. */
    vibeos_x86_64_serial_puts("[KBD] keyboard armed (IRQ1); seeding read() self-test input\n");
    vibeos_x86_64_keyboard_inject("vibeos\n"
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
                                  "EFI/BOOT/SIGNAL.ELF\n"
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
    g_sched_running = 1;
    __asm__ __volatile__("sti");

    /* With a run queue in place, wake the other cores. */
    hw_smp_bringup();

    /* Wait for the spawned tasks to finish before the kernel task goes on to
     * the console (init-style child reaping). The kernel task is preempted
     * while it waits, so the user tasks make progress; hlt idles until the next
     * timer tick instead of spinning. */
    for (;;) {
        int i;
        int alive = 0;
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (g_tasks[i].is_user &&
                (g_tasks[i].state == HW_TASK_READY || g_tasks[i].state == HW_TASK_RUNNING)) {
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

    /* The graphical shell cannot be checked from a log - a log can only say a
     * desktop was composed. What it can report is whether the pieces beneath
     * it did work. Keep this marker independent from the compatibility stats:
     * a compat accounting failure must not erase evidence that the GUI itself
     * was initialized and rendered. */
    if (vibeos_x86_64_gui_active()) {
        vibeos_x86_64_serial_puts("[GUI] GUI_STATS frames=0x");
        vibeos_x86_64_serial_print_hex(vibeos_x86_64_gui_frames());
        vibeos_x86_64_serial_puts(" termchars=0x");
        vibeos_x86_64_serial_print_hex(vibeos_x86_64_gui_term_chars());
        vibeos_x86_64_serial_puts("\n");
    }
    if (vibeos_compat_stats(&g_compat_rt, &translated, &denied) == 0) {
        vibeos_x86_64_serial_puts("[MM] COW_STATS shared=0x");
        vibeos_x86_64_serial_print_hex(g_cow_shared);
        vibeos_x86_64_serial_puts(" copied=0x");
        vibeos_x86_64_serial_print_hex(g_cow_copied);
        vibeos_x86_64_serial_puts("\n");
        vibeos_x86_64_serial_puts("[COMPAT] linux syscalls translated=0x");
        vibeos_x86_64_serial_print_hex(translated);
        vibeos_x86_64_serial_puts(" denied=0x");
        vibeos_x86_64_serial_print_hex(denied);
        vibeos_x86_64_serial_puts("\n");
    }
}

/* Entry point invoked from entry.s before vibeos_kmain. */
void vibeos_x86_64_hw_early_init(const vibeos_boot_info_t *boot_info) {
    vibeos_x86_64_serial_puts("[HW] early init: loading GDT\n");
    hw_load_gdt(0);
    vibeos_x86_64_serial_puts("[HW] GDT loaded (CS=0x08 DS=0x10)\n");

    hw_load_idt();
    vibeos_x86_64_serial_puts("[HW] IDT loaded (256 gates, 48 vectors wired: 32 exceptions + 16 IRQs)\n");

    (void)vibeos_trap_state_init(&g_arch_trap_state);
    g_arch_trap_ready = 1;
    vibeos_x86_64_serial_puts("[HW] trap model armed (routing faults via vibeos_trap_dispatch_ex)\n");

    hw_enable_paging();

    vibeos_x86_64_serial_puts("[HW] self-test: raising int3\n");
    __asm__ __volatile__("int3");
    vibeos_x86_64_serial_puts("[HW] resumed after int3 (trap routed through model)\n");

    hw_enable_syscall();
    hw_enable_timer_irq();

    /* Move off the legacy PIC/PIT onto the local + IO APIC pair (per-CPU timer,
     * IO-APIC interrupt routing) now that basic IRQ delivery is proven. */
    hw_apic_bringup(boot_info);

    /* From here the system is scheduled: the kernel itself becomes a task and
     * user tasks are preempted alongside it. */
    hw_pmm_bringup(boot_info);

    /* Display console: render text into the firmware framebuffer, if any. */
    if (boot_info && boot_info->framebuffer_base != 0u) {
        /* The graphical shell needs a screen-sized back buffer. Taken from the
         * page allocator, since a static one would put several megabytes of
         * .bss into every kernel image including the ones that never see a
         * framebuffer. */
        uint64_t px = (uint64_t)boot_info->framebuffer_width *
                      boot_info->framebuffer_height;
        uint64_t pages = (px * 4ull + 4095ull) / 4096ull;
        void *back = g_hw_pmm_ready ? vibeos_pmm_alloc_pages(&g_hw_pmm, (size_t)pages) : 0;

        if (vibeos_x86_64_mouse_init(boot_info->framebuffer_width,
                                     boot_info->framebuffer_height) == 0) {
            vibeos_x86_64_serial_puts("[MOUSE] PS/2 mouse ready on IRQ12\n");
        } else {
            vibeos_x86_64_serial_puts("[MOUSE] no PS/2 mouse\n");
        }
        if (back && ((uint64_t)(uintptr_t)back + px * 4ull) <= VIBEOS_HW_IDENTITY_LIMIT &&
            vibeos_x86_64_gui_init(boot_info->framebuffer_base,
                                   boot_info->framebuffer_width,
                                   boot_info->framebuffer_height, back) == 0) {
            vibeos_x86_64_serial_puts("[GUI] desktop up: 0x");
            vibeos_x86_64_serial_print_hex(boot_info->framebuffer_width);
            vibeos_x86_64_serial_puts("x0x");
            vibeos_x86_64_serial_print_hex(boot_info->framebuffer_height);
            vibeos_x86_64_serial_puts("\n");
        }
    }
    if (boot_info && !vibeos_x86_64_gui_active() &&
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

    (void)vibeos_log_init(&g_kernel_log);
    hw_log(VIBEOS_LOG_INFO, 0, 0, 0, "kernel log ready");

    /* Real storage: bring up virtio-blk, mount the FAT filesystem, and load the
     * init program straight from disk (EFI/BOOT/INIT.ELF -> INIT.ELF at root). */
    if (vibeos_x86_64_virtio_blk_init() == 0 &&
        vibeos_x86_64_fat_vfs_mount(&g_rootfs) == 0) {
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

    hw_sched_bringup(boot_info);

    vibeos_x86_64_serial_puts("[HW] HW_INIT_OK\n");
}
