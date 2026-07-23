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

#define VIBEOS_HW_USER_CODE_SEL 0x1Bu   /* GDT index 3, RPL 3 */
#define VIBEOS_HW_USER_DATA_SEL 0x23u   /* GDT index 4, RPL 3 */
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

/* null, kcode, kdata, ucode, udata, then a 16-byte (two-slot) TSS descriptor. */
static uint64_t g_gdt[7];
static struct tss64 g_tss;
static uint8_t g_kernel_syscall_stack[16384] __attribute__((aligned(16)));

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
extern char vibeos_x86_64_user_program[];

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

static void hw_load_gdt(void) {
    struct gdt_pointer gdtr;
    uint32_t i;

    g_gdt[0] = 0x0000000000000000ull;        /* null                        */
    g_gdt[1] = 0x00AF9A000000FFFFull;        /* kernel code (0x08, DPL=0)   */
    g_gdt[2] = 0x00CF92000000FFFFull;        /* kernel data (0x10, DPL=0)   */
    g_gdt[3] = 0x00AFFA000000FFFFull;        /* user code   (0x18, DPL=3)   */
    g_gdt[4] = 0x00CFF2000000FFFFull;        /* user data   (0x20, DPL=3)   */

    for (i = 0; i < (uint32_t)sizeof(g_tss); i++) {
        ((uint8_t *)(void *)&g_tss)[i] = 0;
    }
    g_tss.rsp0 = (uint64_t)(uintptr_t)&g_kernel_syscall_stack[sizeof(g_kernel_syscall_stack)];
    g_tss.iomap_base = (uint16_t)sizeof(g_tss);
    hw_setup_tss_descriptor(5, (uint64_t)(uintptr_t)&g_tss, (uint32_t)(sizeof(g_tss) - 1u));

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

    __asm__ __volatile__("ltr %0" : : "r"((uint16_t)VIBEOS_HW_TSS_SEL));
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

extern char vibeos_isr_128[];   /* isr.S: stub for the 0x80 syscall gate */

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
    hw_outb(PIC1_DATA, 0xFE);                /* mask all master IRQs but IRQ0 */
    hw_outb(PIC2_DATA, 0xFF);                /* mask all slave IRQs          */
}

/* Program PIT channel 0 to a periodic square wave at VIBEOS_HW_TIMER_HZ. */
static void hw_pit_init(void) {
    uint32_t divisor = PIT_BASE_HZ / VIBEOS_HW_TIMER_HZ;
    hw_outb(PIT_CMD, 0x36);                       /* ch0, lo/hi byte, mode 3 */
    hw_outb(PIT_CH0, (uint8_t)(divisor & 0xFF));
    hw_outb(PIT_CH0, (uint8_t)((divisor >> 8) & 0xFF));
}

static void hw_pic_send_eoi(uint32_t vector) {
    if (vector >= 40u) {          /* IRQ came via the slave PIC */
        hw_outb(PIC2_CMD, PIC_EOI);
    }
    hw_outb(PIC1_CMD, PIC_EOI);
}

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

static void hw_panic(const char *why) {
    vibeos_x86_64_serial_puts(" FATAL: ");
    vibeos_x86_64_serial_puts(why);
    vibeos_x86_64_serial_puts(", halting\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/* Minimal int 0x80 syscall ABI: rax=number, rdi/rsi=args. A real dispatcher
 * will move into kernel/core/syscall.c; this proves the ring-3 -> ring-0 path.
 *   1  = write(ptr, len): emit len bytes from the user pointer over serial
 *   60 = exit(code):      return control to the kernel boot flow */
static void hw_syscall_dispatch(vibeos_x86_64_isr_frame_t *frame) {
    uint64_t nr = frame->rax;

    if (nr == 1u) {
        const char *p = (const char *)(uintptr_t)frame->rdi;
        uint64_t len = frame->rsi;
        uint64_t i;
        vibeos_x86_64_serial_puts("[HW][SYS] write(ring3): ");
        for (i = 0; i < len; i++) {
            char c = p[i];
            if (c == '\n') {
                vibeos_x86_64_serial_putc('\r');
            }
            vibeos_x86_64_serial_putc(c);
        }
        frame->rax = len;                 /* bytes written (returned to ring 3) */
        return;
    }
    if (nr == 60u) {
        vibeos_x86_64_serial_puts("[HW][SYS] exit(ring3) code=0x");
        vibeos_x86_64_serial_print_hex(frame->rdi);
        vibeos_x86_64_serial_puts("\n[HW] RING3_SYSCALL_OK (returning to kernel)\n");
        vibeos_x86_64_ring3_resume();     /* does not return */
    }
    vibeos_x86_64_serial_puts("[HW][SYS] unknown syscall\n");
    frame->rax = (uint64_t)-1;
}

/* Called from the assembly common stub with a pointer to the saved frame. */
void vibeos_x86_64_isr_handler(vibeos_x86_64_isr_frame_t *frame) {
    vibeos_trap_frame_t tf;
    vibeos_trap_decision_t decision;
    uint64_t fault_address;

    /* Software syscall gate. */
    if (frame->vector == 0x80u) {
        hw_syscall_dispatch(frame);
        return;
    }

    /* Hardware IRQs (post-remap vectors 0x20-0x2F): acknowledge to the PIC and
     * return quietly. The timer fires ~100x/s, so no per-interrupt logging. */
    if (frame->vector >= VIBEOS_HW_IRQ_BASE && frame->vector < VIBEOS_HW_WIRED_VECTORS) {
        if (frame->vector == VIBEOS_HW_IRQ_TIMER) {
            g_timer_ticks++;
        }
        hw_pic_send_eoi((uint32_t)frame->vector);
        return;
    }

    fault_address = (frame->vector == 14u) ? hw_read_cr2() : 0u;

    vibeos_x86_64_serial_puts("[HW][TRAP] ");
    hw_log_field("vector", frame->vector);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("err", frame->error_code);
    vibeos_x86_64_serial_puts(" ");
    hw_log_field("rip", frame->rip);
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

    if (vibeos_trap_dispatch_ex(&g_arch_trap_state, &tf, 0, 0, &decision) != 0) {
        hw_panic("trap dispatch failed");
    }

    vibeos_x86_64_serial_puts(" -> action=");
    vibeos_x86_64_serial_puts(hw_action_name(decision.action));
    vibeos_x86_64_serial_puts(" count=0x");
    vibeos_x86_64_serial_print_hex(g_arch_trap_state.trap_count);
    vibeos_x86_64_serial_puts("\n");

    if (decision.action == VIBEOS_TRAP_ACTION_CONTINUE) {
        /* Resumable trap (e.g. #BP): iretq returns to the saved RIP. */
        return;
    }

    /* KILL_CURRENT has no meaning yet (no user processes on metal); both
     * remaining actions are fatal during bring-up. */
    hw_panic("unrecoverable CPU exception");
}

/* ---- Paging ------------------------------------------------------------- */

#define PTE_PRESENT 0x001ull
#define PTE_WRITE   0x002ull
#define PTE_USER    0x004ull            /* ring-3 accessible */
#define PTE_PS      0x080ull            /* 2 MiB page at PD level */
#define VIBEOS_HW_IDENTITY_GIB 4u       /* identity-map the first 4 GiB */

/* setjmp-style kernel context saved by ring3_enter (see isr.S). Global so the
 * assembly can reference it by name. Layout: rbx,rbp,r12,r13,r14,r15,rsp. */
uint64_t g_ring3_kctx[8];

static uint8_t g_user_stack[8192] __attribute__((aligned(16)));

/* Kernel-owned page tables (static BSS, no PMM dependency during bring-up). */
static uint64_t g_pml4[512] __attribute__((aligned(4096)));
static uint64_t g_pdpt[512] __attribute__((aligned(4096)));
static uint64_t g_pd[VIBEOS_HW_IDENTITY_GIB][512] __attribute__((aligned(4096)));

/* Extra tables + backing frame to prove a non-identity VA->PA mapping. */
static uint64_t g_pdpt_hi[512] __attribute__((aligned(4096)));
static uint64_t g_pd_hi[512] __attribute__((aligned(4096)));
static uint64_t g_pt_hi[512] __attribute__((aligned(4096)));
static uint8_t  g_test_frame[4096] __attribute__((aligned(4096)));

/* Canonical VA at 512 GiB (PML4[1]) - far outside the 4 GiB identity map, so a
 * successful access there can only come from our explicit mapping. */
#define VIBEOS_HW_TEST_VA 0x8000000000ull
#define VIBEOS_HW_TEST_MAGIC 0x5642454F50414745ull /* "VBEOPAGE" */

static uint64_t hw_read_cr3(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* Map VIBEOS_HW_TEST_VA -> g_test_frame with a full PDPT/PD/PT walk. */
static void hw_map_test_page(void) {
    g_pt_hi[0]   = (uint64_t)(uintptr_t)&g_test_frame[0] | PTE_PRESENT | PTE_WRITE;
    g_pd_hi[0]   = (uint64_t)(uintptr_t)&g_pt_hi[0]      | PTE_PRESENT | PTE_WRITE;
    g_pdpt_hi[0] = (uint64_t)(uintptr_t)&g_pd_hi[0]      | PTE_PRESENT | PTE_WRITE;
    g_pml4[1]    = (uint64_t)(uintptr_t)&g_pdpt_hi[0]    | PTE_PRESENT | PTE_WRITE;
}

/* Write through the high VA and confirm it lands in the backing frame. */
static void hw_verify_test_mapping(void) {
    volatile uint64_t *va = (volatile uint64_t *)(uintptr_t)VIBEOS_HW_TEST_VA;
    uint64_t via_phys;

    *va = VIBEOS_HW_TEST_MAGIC;
    via_phys = *(volatile uint64_t *)(void *)&g_test_frame[0];
    if (*va == VIBEOS_HW_TEST_MAGIC && via_phys == VIBEOS_HW_TEST_MAGIC) {
        vibeos_x86_64_serial_puts("[HW] MAP_TEST_OK (VA 0x8000000000 -> frame translated)\n");
    } else {
        vibeos_x86_64_serial_puts("[HW] MAP_TEST_FAIL\n");
    }
}

/* Build a fresh 4-level page table identity-mapping the first N GiB with 2 MiB
 * pages, then switch CR3 to it. Everything the kernel touches (its image at
 * 0x4000000, boot structures at 0x200000, the stack, and these tables) lives
 * within the first GiB and QEMU RAM/MMIO is below 4 GiB, so the switch from the
 * firmware's identity map to our own is safe and the mapping is unchanged. */
static void hw_enable_paging(void) {
    uint32_t g, e;

    vibeos_x86_64_serial_puts("[HW] building page tables (identity, first 4GiB, 2MiB pages)\n");
    /* US bit is set throughout so ring-3 code can execute during bring-up.
     * This is a temporary simplification: kernel/user isolation (per-page US
     * and separate address spaces) is a later milestone. */
    for (g = 0; g < VIBEOS_HW_IDENTITY_GIB; g++) {
        for (e = 0; e < 512u; e++) {
            uint64_t phys = ((uint64_t)g * 0x40000000ull) + ((uint64_t)e * 0x200000ull);
            g_pd[g][e] = phys | PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_PS;
        }
        g_pdpt[g] = (uint64_t)(uintptr_t)&g_pd[g][0] | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    g_pml4[0] = (uint64_t)(uintptr_t)&g_pdpt[0] | PTE_PRESENT | PTE_WRITE | PTE_USER;
    hw_map_test_page();

    __asm__ __volatile__("mov %0, %%cr3"
                         : : "r"((uint64_t)(uintptr_t)&g_pml4[0]) : "memory");

    vibeos_x86_64_serial_puts("[HW] CR3 loaded with kernel-owned tables: 0x");
    vibeos_x86_64_serial_print_hex(hw_read_cr3());
    vibeos_x86_64_serial_puts("\n[HW] PAGING_OK (executing on kernel page tables)\n");
    hw_verify_test_mapping();
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

/* Drop to ring 3, run a small user program that issues int 0x80 syscalls, and
 * return here once it exits. Runs with interrupts disabled (before the timer is
 * enabled), so int 0x80 is the only control transfer back into the kernel. */
static void hw_enter_usermode_demo(void) {
    uint64_t user_rip = (uint64_t)(uintptr_t)vibeos_x86_64_user_program;
    uint64_t user_rsp = (uint64_t)(uintptr_t)&g_user_stack[sizeof(g_user_stack)];

    vibeos_x86_64_serial_puts("[HW] entering ring 3 (user mode)...\n");
    vibeos_x86_64_ring3_enter(user_rip, user_rsp);
    vibeos_x86_64_serial_puts("[HW] RING3_OK (back in ring 0)\n");
}

/* Entry point invoked from entry.s before vibeos_kmain. */
void vibeos_x86_64_hw_early_init(void) {
    vibeos_x86_64_serial_puts("[HW] early init: loading GDT\n");
    hw_load_gdt();
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

    hw_enter_usermode_demo();

    hw_enable_timer_irq();

    vibeos_x86_64_serial_puts("[HW] HW_INIT_OK\n");
}
