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

/* ---- GDT ---------------------------------------------------------------- */

static uint64_t g_gdt[3];

struct gdt_pointer {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

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

/* Populated by isr.S: one assembly stub entry point per vector. */
extern uint64_t vibeos_isr_stub_table[VIBEOS_HW_IDT_GATES < 32 ? VIBEOS_HW_IDT_GATES : 32];

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
    g_gdt[0] = 0x0000000000000000ull;        /* null                       */
    g_gdt[1] = 0x00AF9A000000FFFFull;        /* kernel code (L=1, DPL=0)   */
    g_gdt[2] = 0x00CF92000000FFFFull;        /* kernel data (DPL=0)        */
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
}

static void hw_set_gate(uint32_t vector, uint64_t handler) {
    struct idt_entry *e = &g_idt[vector];
    e->offset_low = (uint16_t)(handler & 0xFFFFull);
    e->selector = (uint16_t)VIBEOS_HW_KERNEL_CS;
    e->ist = 0;
    e->type_attr = (uint8_t)VIBEOS_HW_GATE_INTERRUPT;
    e->offset_mid = (uint16_t)((handler >> 16) & 0xFFFFull);
    e->offset_high = (uint32_t)((handler >> 32) & 0xFFFFFFFFull);
    e->reserved = 0;
}

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
    for (i = 0; i < 32u; i++) {
        hw_set_gate(i, vibeos_isr_stub_table[i]);
    }
    idtr.limit = (uint16_t)(sizeof(g_idt) - 1u);
    idtr.base = (uint64_t)(uintptr_t)&g_idt[0];
    __asm__ __volatile__("lidt %0" : : "m"(idtr) : "memory");
}

static uint64_t hw_read_cr2(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(v));
    return v;
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

/* Called from the assembly common stub with a pointer to the saved frame. */
void vibeos_x86_64_isr_handler(vibeos_x86_64_isr_frame_t *frame) {
    vibeos_trap_frame_t tf;
    vibeos_trap_decision_t decision;
    uint64_t fault_address = (frame->vector == 14u) ? hw_read_cr2() : 0u;

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

/* Entry point invoked from entry.s before vibeos_kmain. */
void vibeos_x86_64_hw_early_init(void) {
    vibeos_x86_64_serial_puts("[HW] early init: loading GDT\n");
    hw_load_gdt();
    vibeos_x86_64_serial_puts("[HW] GDT loaded (CS=0x08 DS=0x10)\n");

    hw_load_idt();
    vibeos_x86_64_serial_puts("[HW] IDT loaded (256 gates, 32 CPU exceptions wired)\n");

    (void)vibeos_trap_state_init(&g_arch_trap_state);
    g_arch_trap_ready = 1;
    vibeos_x86_64_serial_puts("[HW] trap model armed (routing faults via vibeos_trap_dispatch_ex)\n");

    vibeos_x86_64_serial_puts("[HW] self-test: raising int3\n");
    __asm__ __volatile__("int3");
    vibeos_x86_64_serial_puts("[HW] resumed after int3 (trap routed through model)\n");

    vibeos_x86_64_serial_puts("[HW] HW_INIT_OK\n");
}
