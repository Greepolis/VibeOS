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

static void hw_load_gdt(void) {
    struct gdt_pointer gdtr;
    uint32_t i;

    g_gdt[0] = 0x0000000000000000ull;        /* null                        */
    g_gdt[1] = 0x00AF9A000000FFFFull;        /* kernel code (0x08, DPL=0)   */
    g_gdt[2] = 0x00CF92000000FFFFull;        /* kernel data (0x10, DPL=0)   */
    g_gdt[3] = 0x00CFF2000000FFFFull;        /* user data   (0x18, DPL=3)   */
    g_gdt[4] = 0x00AFFA000000FFFFull;        /* user code   (0x20, DPL=3)   */

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

/* Linux x86-64 syscall translation. Args follow the Linux ABI (nr in rax;
 * a1..a3 in rdi, rsi, rdx). This is the on-metal front end that a real
 * implementation will route into kernel/core/syscall.c and the Linux
 * personality in user/compat/linux. Reached from both the int 0x80 gate and
 * the native `syscall` trampoline.
 *   1/write(fd, buf, len) : emit len bytes from the user buffer over serial
 *   60/exit, 231/exit_group : return control to the kernel boot flow
 *   39/getpid : returns 1
 */
long vibeos_x86_64_linux_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    switch (nr) {
        case 1: {
            const char *p = (const char *)(uintptr_t)a2;
            uint64_t len = a3;
            uint64_t i;
            (void)a1; /* fd ignored during bring-up; all output goes to serial */
            vibeos_x86_64_serial_puts("[HW][SYS] write(ring3): ");
            for (i = 0; i < len; i++) {
                char c = p[i];
                if (c == '\n') {
                    vibeos_x86_64_serial_putc('\r');
                }
                vibeos_x86_64_serial_putc(c);
            }
            return (long)len;
        }
        case 39:
            return 1; /* getpid */
        case 60:
        case 231:
            hw_task_exit(a1); /* retires this task and switches away; no return */
            return 0;
        default:
            vibeos_x86_64_serial_puts("[HW][SYS] unimplemented Linux syscall nr=0x");
            vibeos_x86_64_serial_print_hex(nr);
            vibeos_x86_64_serial_puts("\n");
            return -38; /* -ENOSYS */
    }
}

/* Called from the assembly common stub with a pointer to the saved frame. */
void vibeos_x86_64_isr_handler(vibeos_x86_64_isr_frame_t *frame) {
    vibeos_trap_frame_t tf;
    vibeos_trap_decision_t decision;
    uint64_t fault_address;

    /* Software syscall gate (int 0x80), Linux argument order. */
    if (frame->vector == 0x80u) {
        frame->rax = (uint64_t)vibeos_x86_64_linux_syscall(frame->rax, frame->rdi, frame->rsi, frame->rdx);
        return;
    }

    /* Hardware IRQs (post-remap vectors 0x20-0x2F): acknowledge to the PIC. The
     * timer (IRQ0) additionally drives the preemptive scheduler. */
    if (frame->vector >= VIBEOS_HW_IRQ_BASE && frame->vector < VIBEOS_HW_WIRED_VECTORS) {
        if (frame->vector == VIBEOS_HW_IRQ_TIMER) {
            g_timer_ticks++;
            hw_pic_send_eoi((uint32_t)frame->vector);
            hw_schedule(frame); /* may rewrite the frame to switch tasks */
            return;
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

/* Used by the `syscall` trampoline (isr.S): the instruction does not switch
 * stacks, so we stash the user rsp and load a kernel stack ourselves. */
uint64_t g_user_saved_rsp;
uint64_t g_syscall_kstack_top;

/* Kernel-owned page tables (static BSS, no PMM dependency during bring-up).
 * The identity map is supervisor-only; user memory lives in its own PML4 slot
 * with per-process tables, so ring 3 cannot reach kernel pages. */
static uint64_t g_pml4[512] __attribute__((aligned(4096)));
static uint64_t g_pdpt[512] __attribute__((aligned(4096)));
static uint64_t g_pd[VIBEOS_HW_IDENTITY_GIB][512] __attribute__((aligned(4096)));

/* ---- Page pool + per-process address spaces ------------------------------ */

/* Bump allocator over a static pool: backs per-process page tables and user
 * pages this early in boot, before the real PMM is available. */
#define VIBEOS_HW_POOL_PAGES 128u
static uint8_t g_page_pool[VIBEOS_HW_POOL_PAGES][4096] __attribute__((aligned(4096)));
static uint32_t g_pool_next;

/* User virtual layout: PML4 slot 1 (512 GiB). Programs are linked at
 * VIBEOS_HW_USER_BASE (user/prog/user.ld); their stack sits above the image. */
#define VIBEOS_HW_USER_BASE 0x8000000000ull
#define VIBEOS_HW_USER_STACK_TOP (VIBEOS_HW_USER_BASE + 0x00400000ull) /* +4 MiB */
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

static void *hw_alloc_page(void) {
    uint8_t *p;
    uint32_t i;
    if (g_pool_next >= VIBEOS_HW_POOL_PAGES) {
        return 0;
    }
    p = g_page_pool[g_pool_next++];
    for (i = 0; i < 4096u; i++) {
        p[i] = 0;
    }
    return p;
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
    g_syscall_kstack_top =
        (uint64_t)(uintptr_t)&g_kernel_syscall_stack[sizeof(g_kernel_syscall_stack)];
    hw_wrmsr(MSR_EFER, hw_rdmsr(MSR_EFER) | 1ull);                 /* SCE */
    hw_wrmsr(MSR_STAR, ((uint64_t)0x10 << 48) | ((uint64_t)0x08 << 32));
    hw_wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)vibeos_x86_64_syscall_entry);
    hw_wrmsr(MSR_SFMASK, 0x200ull);                                /* clear IF on entry */
    vibeos_x86_64_serial_puts("[HW] syscall/sysret enabled (LSTAR set)\n");
}

/* ---- Process creation (ELF -> private address space) --------------------- */

typedef struct {
    vibeos_hw_aspace_t as;
    uint64_t entry;
} hw_proc_t;

typedef struct {
    vibeos_hw_aspace_t *as;
} hw_load_ctx_t;

/* Per PT_LOAD segment: allocate private pages, copy the file image into them
 * through the kernel's identity view, then map them into the process address
 * space with the segment's permissions (US=1; writable only if PF_W). */
static int hw_elf_seg_cb(void *ctx, uint64_t vaddr, const unsigned char *data,
                         uint64_t filesz, uint64_t memsz, uint32_t flags) {
    hw_load_ctx_t *lc = (hw_load_ctx_t *)ctx;
    uint64_t leaf = PTE_PRESENT | PTE_USER;
    uint64_t va;

    if ((flags & 2u) != 0) { /* PF_W */
        leaf |= PTE_WRITE;
    }
    for (va = vaddr & ~0xFFFull; va < vaddr + memsz; va += 4096ull) {
        uint8_t *page = (uint8_t *)hw_alloc_page();
        uint64_t i;
        if (!page) {
            return -1;
        }
        for (i = 0; i < 4096ull; i++) {
            uint64_t byte_va = va + i;
            if (byte_va >= vaddr && byte_va < vaddr + filesz) {
                page[i] = data[byte_va - vaddr];
            }
            /* remaining bytes stay zero: .bss and page padding */
        }
        if (hw_map_page(lc->as, va, (uint64_t)(uintptr_t)page, leaf) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Build a process: private address space, the ELF image loaded into it, and a
 * user stack mapped just below VIBEOS_HW_USER_STACK_TOP. */
static int hw_proc_create(hw_proc_t *p, const unsigned char *elf, uint64_t len) {
    hw_load_ctx_t lc;
    uint32_t i;

    if (hw_aspace_create(&p->as) != 0) {
        return -1;
    }
    lc.as = &p->as;
    if (vibeos_x86_64_elf_load(elf, len, &lc, hw_elf_seg_cb, &p->entry) != 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_HW_USER_STACK_PAGES; i++) {
        void *page = hw_alloc_page();
        uint64_t va = VIBEOS_HW_USER_STACK_TOP - ((uint64_t)(i + 1u) * 4096ull);
        if (!page || hw_map_page(&p->as, va, (uint64_t)(uintptr_t)page,
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

#define VIBEOS_HW_MAX_TASKS 8

enum {
    HW_TASK_FREE = 0,
    HW_TASK_READY = 1,
    HW_TASK_RUNNING = 2,
    HW_TASK_ZOMBIE = 3
};

extern void vibeos_x86_64_task_enter(vibeos_x86_64_isr_frame_t *task);
extern const unsigned char vibeos_user_task_elf[];
extern const unsigned long vibeos_user_task_elf_len;

typedef struct {
    vibeos_x86_64_isr_frame_t ctx;
    hw_proc_t proc;
    uint64_t cr3;
    uint64_t exit_code;
    uint32_t pid;
    int state;
    int is_user;
} hw_task_t;

static hw_task_t g_tasks[VIBEOS_HW_MAX_TASKS];
static int g_current_task = -1;
static int g_sched_running;
static uint32_t g_next_pid = 1;

static int hw_task_alloc(void) {
    int i;
    for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
        if (g_tasks[i].state == HW_TASK_FREE) {
            return i;
        }
    }
    return -1;
}

/* Next runnable task after `from`, round robin. Returns -1 if none. */
static int hw_pick_next(int from) {
    int n;
    for (n = 1; n <= VIBEOS_HW_MAX_TASKS; n++) {
        int i = (from + n) % VIBEOS_HW_MAX_TASKS;
        if (g_tasks[i].state == HW_TASK_READY || g_tasks[i].state == HW_TASK_RUNNING) {
            return i;
        }
    }
    return -1;
}

/* Timer-driven preemption: save the interrupted task, pick the next runnable
 * one, and resume it by rewriting the live IRQ frame and switching CR3. Runs
 * for the life of the system - there is no "demo over" exit. */
static void hw_schedule(vibeos_x86_64_isr_frame_t *frame) {
    int next;

    if (!g_sched_running || g_current_task < 0) {
        return;
    }
    next = hw_pick_next(g_current_task);
    if (next < 0 || next == g_current_task) {
        return; /* nothing else runnable: keep running the current task */
    }
    g_tasks[g_current_task].ctx = *frame;
    if (g_tasks[g_current_task].state == HW_TASK_RUNNING) {
        g_tasks[g_current_task].state = HW_TASK_READY;
    }
    g_current_task = next;
    g_tasks[next].state = HW_TASK_RUNNING;
    *frame = g_tasks[next].ctx;
    hw_write_cr3(g_tasks[next].cr3);
}

static void hw_task_init_user_ctx(vibeos_x86_64_isr_frame_t *c, uint64_t entry, uint64_t arg) {
    uint32_t k;
    for (k = 0; k < (uint32_t)sizeof(*c); k++) {
        ((uint8_t *)(void *)c)[k] = 0;
    }
    c->rip = entry;
    c->cs = VIBEOS_HW_USER_CODE_SEL;
    c->rflags = 0x202;   /* reserved bit + IF */
    c->rsp = VIBEOS_HW_USER_STACK_TOP;
    c->ss = VIBEOS_HW_USER_DATA_SEL;
    c->rdi = arg;        /* passed to the task's _start */
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
    g_tasks[i].is_user = 0;
    g_tasks[i].pid = g_next_pid++;
    g_tasks[i].cr3 = (uint64_t)(uintptr_t)&g_pml4[0];
    g_current_task = i;
    return i;
}

/* Create a ring-3 task from an ELF image: private address space, mapped stack,
 * READY to be picked by the scheduler. */
static int hw_task_spawn_user(const unsigned char *elf, uint64_t len, uint64_t arg) {
    int i = hw_task_alloc();
    if (i < 0) {
        return -1;
    }
    if (hw_proc_create(&g_tasks[i].proc, elf, len) != 0) {
        return -1;
    }
    g_tasks[i].cr3 = hw_proc_cr3(&g_tasks[i].proc);
    hw_task_init_user_ctx(&g_tasks[i].ctx, g_tasks[i].proc.entry, arg);
    g_tasks[i].state = HW_TASK_READY;
    g_tasks[i].is_user = 1;
    g_tasks[i].pid = g_next_pid++;
    return i;
}

/* exit(): retire the calling task and switch to another runnable one. Called
 * from the syscall path, so it enters the next task directly and never returns
 * to the caller. */
static void hw_task_exit(uint64_t code) {
    int next;

    if (g_current_task >= 0) {
        g_tasks[g_current_task].state = HW_TASK_ZOMBIE;
        g_tasks[g_current_task].exit_code = code;
        vibeos_x86_64_serial_puts("[SCHED] task pid=0x");
        vibeos_x86_64_serial_print_hex(g_tasks[g_current_task].pid);
        vibeos_x86_64_serial_puts(" exited code=0x");
        vibeos_x86_64_serial_print_hex(code);
        vibeos_x86_64_serial_puts("\n");
    }
    next = hw_pick_next(g_current_task < 0 ? 0 : g_current_task);
    if (next < 0) {
        vibeos_x86_64_serial_puts("[SCHED] no runnable task; halting\n");
        for (;;) {
            __asm__ __volatile__("hlt");
        }
    }
    g_current_task = next;
    g_tasks[next].state = HW_TASK_RUNNING;
    hw_write_cr3(g_tasks[next].cr3);
    vibeos_x86_64_task_enter(&g_tasks[next].ctx); /* does not return */
}

/* Bring the scheduler up: spawn the initial user tasks, adopt the kernel as a
 * task, and let the timer preempt from here on. */
static void hw_sched_bringup(void) {
    int hello_id, a_id, b_id, kern_id;

    hello_id = hw_task_spawn_user(vibeos_user_hello_elf, vibeos_user_hello_elf_len, 0);
    a_id = hw_task_spawn_user(vibeos_user_task_elf, vibeos_user_task_elf_len, 0);
    b_id = hw_task_spawn_user(vibeos_user_task_elf, vibeos_user_task_elf_len, 1);
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
    g_sched_running = 1;
    __asm__ __volatile__("sti");

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
        __asm__ __volatile__("hlt");
    }
    vibeos_x86_64_serial_puts("[SCHED] all user tasks retired; kernel task continues\n");
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

    hw_enable_syscall();
    hw_enable_timer_irq();

    /* From here the system is scheduled: the kernel itself becomes a task and
     * user tasks are preempted alongside it. */
    hw_sched_bringup();

    vibeos_x86_64_serial_puts("[HW] HW_INIT_OK\n");
}
