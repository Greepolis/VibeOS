/* ACPI discovery + local/IO APIC + SMP application-processor startup.
 *
 * This replaces the legacy 8259 PIC / PIT pair with the interrupt hardware a
 * modern x86-64 machine actually uses:
 *
 *   - ACPI: the RSDP handed over by the bootloader leads to the MADT, which
 *     lists every CPU's local-APIC id, the IO-APIC, and the ISA interrupt
 *     source overrides.
 *   - Local APIC: per-CPU interrupt controller. Provides the EOI register and
 *     a per-CPU periodic timer (calibrated against the PIT) that drives
 *     preemption independently on every core.
 *   - IO-APIC: routes external (ISA/PCI) interrupts to a chosen vector on a
 *     chosen CPU. The keyboard IRQ moves here from the PIC.
 *   - SMP: application processors are woken with INIT-SIPI-SIPI and enter a
 *     real-mode trampoline (ap_boot.S) that walks them through protected mode
 *     into long mode on the kernel's own page tables.
 *
 * Image-only: never linked into host tests.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

/* ---- port I/O ------------------------------------------------------------ */

static inline void ap_outb(uint16_t p, uint8_t v) {
    __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(p));
}
static inline uint8_t ap_inb(uint16_t p) {
    uint8_t v;
    __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

/* ---- MMIO ---------------------------------------------------------------- */

static inline uint32_t mmio_read32(uint64_t addr) {
    return *(volatile uint32_t *)(uintptr_t)addr;
}
static inline void mmio_write32(uint64_t addr, uint32_t val) {
    *(volatile uint32_t *)(uintptr_t)addr = val;
}

/* ---- ACPI tables --------------------------------------------------------- */

#define MADT_TYPE_LAPIC 0u
#define MADT_TYPE_IOAPIC 1u
#define MADT_TYPE_ISO 2u

#define VIBEOS_APIC_MAX_CPUS 8u
#define VIBEOS_APIC_MAX_ISO 16u

typedef struct {
    uint8_t source;   /* ISA IRQ number      */
    uint32_t gsi;     /* global system interrupt it actually arrives on */
    uint16_t flags;   /* polarity / trigger mode */
} apic_iso_t;

static uint32_t g_lapic_ids[VIBEOS_APIC_MAX_CPUS];
static uint32_t g_cpu_count;
static uint64_t g_lapic_base = 0xFEE00000ull;
static uint64_t g_ioapic_base;
static uint32_t g_ioapic_gsi_base;
static apic_iso_t g_iso[VIBEOS_APIC_MAX_ISO];
static uint32_t g_iso_count;
static int g_acpi_ok;

static int sig_eq(const uint8_t *p, const char *sig, uint32_t n) {
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (p[i] != (uint8_t)sig[i]) {
            return 0;
        }
    }
    return 1;
}

static uint8_t checksum(const uint8_t *p, uint32_t len) {
    uint8_t sum = 0;
    uint32_t i;
    for (i = 0; i < len; i++) {
        sum = (uint8_t)(sum + p[i]);
    }
    return sum;
}

/* Read a possibly-unaligned little-endian field out of a firmware table. */
static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint64_t ld64(const uint8_t *p) {
    return (uint64_t)ld32(p) | ((uint64_t)ld32(p + 4) << 32);
}

static void apic_parse_madt(const uint8_t *madt) {
    uint32_t len = ld32(madt + 4);
    uint32_t off = 44;

    g_lapic_base = (uint64_t)ld32(madt + 36);
    while (off + 2u <= len) {
        uint8_t type = madt[off];
        uint8_t elen = madt[off + 1];
        if (elen < 2u || off + elen > len) {
            break;
        }
        if (type == MADT_TYPE_LAPIC && elen >= 8u) {
            uint32_t flags = ld32(madt + off + 4);
            /* bit0 = enabled, bit1 = online-capable; either can be started. */
            if ((flags & 3u) != 0u && g_cpu_count < VIBEOS_APIC_MAX_CPUS) {
                g_lapic_ids[g_cpu_count++] = madt[off + 3];
            }
        } else if (type == MADT_TYPE_IOAPIC && elen >= 12u) {
            if (g_ioapic_base == 0u) {
                g_ioapic_base = (uint64_t)ld32(madt + off + 4);
                g_ioapic_gsi_base = ld32(madt + off + 8);
            }
        } else if (type == MADT_TYPE_ISO && elen >= 10u) {
            if (g_iso_count < VIBEOS_APIC_MAX_ISO) {
                g_iso[g_iso_count].source = madt[off + 3];
                g_iso[g_iso_count].gsi = ld32(madt + off + 4);
                g_iso[g_iso_count].flags =
                    (uint16_t)((uint16_t)madt[off + 8] | ((uint16_t)madt[off + 9] << 8));
                g_iso_count++;
            }
        }
        off += elen;
    }
}

/* Walk RSDP -> XSDT/RSDT -> MADT. Firmware tables live in ACPI-reclaimable
 * memory below 4 GiB, which the kernel identity-maps, so they can be read
 * directly. */
int vibeos_x86_64_acpi_init(uint64_t rsdp_addr) {
    const uint8_t *rsdp;
    const uint8_t *sdt = 0;
    uint32_t entry_size = 4;
    uint32_t sdt_len, i, entries;

    g_cpu_count = 0;
    g_ioapic_base = 0;
    g_iso_count = 0;

    if (rsdp_addr == 0u || rsdp_addr >= 0x100000000ull) {
        vibeos_x86_64_serial_puts("[ACPI] no usable RSDP from firmware: 0x");
        vibeos_x86_64_serial_print_hex(rsdp_addr);
        vibeos_x86_64_serial_puts("\n");
        return -1;
    }
    rsdp = (const uint8_t *)(uintptr_t)rsdp_addr;
    if (!sig_eq(rsdp, "RSD PTR ", 8) || checksum(rsdp, 20) != 0u) {
        vibeos_x86_64_serial_puts("[ACPI] RSDP failed validation\n");
        return -1;
    }

    if (rsdp[15] >= 2u) { /* ACPI 2.0+: prefer the 64-bit XSDT */
        uint64_t xsdt = ld64(rsdp + 24);
        if (xsdt != 0u && xsdt < 0x100000000ull) {
            sdt = (const uint8_t *)(uintptr_t)xsdt;
            entry_size = 8;
        }
    }
    if (sdt == 0) {
        /* RsdtAddress is a 32-bit field, so it is always below 4 GiB and
         * always inside the identity map; only the null case can fail. */
        uint64_t rsdt = (uint64_t)ld32(rsdp + 16);
        if (rsdt == 0u) {
            vibeos_x86_64_serial_puts("[ACPI] no usable RSDT/XSDT\n");
            return -1;
        }
        sdt = (const uint8_t *)(uintptr_t)rsdt;
        entry_size = 4;
    }

    sdt_len = ld32(sdt + 4);
    if (sdt_len < 36u) {
        return -1;
    }
    entries = (sdt_len - 36u) / entry_size;
    for (i = 0; i < entries; i++) {
        const uint8_t *ep = sdt + 36u + i * entry_size;
        uint64_t addr = (entry_size == 8u) ? ld64(ep) : (uint64_t)ld32(ep);
        const uint8_t *tbl;
        if (addr == 0u || addr >= 0x100000000ull) {
            continue;
        }
        tbl = (const uint8_t *)(uintptr_t)addr;
        if (sig_eq(tbl, "APIC", 4)) {
            apic_parse_madt(tbl);
            break;
        }
    }

    if (g_cpu_count == 0u) {
        vibeos_x86_64_serial_puts("[ACPI] MADT not found or lists no CPUs\n");
        return -1;
    }

    g_acpi_ok = 1;
    vibeos_x86_64_serial_puts("[ACPI] MADT: cpus=0x");
    vibeos_x86_64_serial_print_hex(g_cpu_count);
    vibeos_x86_64_serial_puts(" lapic=0x");
    vibeos_x86_64_serial_print_hex(g_lapic_base);
    vibeos_x86_64_serial_puts(" ioapic=0x");
    vibeos_x86_64_serial_print_hex(g_ioapic_base);
    vibeos_x86_64_serial_puts("\n");
    return 0;
}

uint32_t vibeos_x86_64_acpi_cpu_count(void) { return g_cpu_count; }
uint32_t vibeos_x86_64_acpi_lapic_id(uint32_t index) {
    return (index < g_cpu_count) ? g_lapic_ids[index] : 0xFFFFFFFFu;
}

/* ---- local APIC ---------------------------------------------------------- */

#define LAPIC_ID 0x020u
#define LAPIC_TPR 0x080u
#define LAPIC_EOI 0x0B0u
#define LAPIC_SVR 0x0F0u
#define LAPIC_ICR_LOW 0x300u
#define LAPIC_ICR_HIGH 0x310u
#define LAPIC_LVT_TIMER 0x320u
#define LAPIC_LVT_LINT0 0x350u
#define LAPIC_LVT_LINT1 0x360u
#define LAPIC_TIMER_INIT 0x380u
#define LAPIC_TIMER_CUR 0x390u
#define LAPIC_TIMER_DIV 0x3E0u

#define LAPIC_LVT_MASKED 0x10000u
#define LAPIC_TIMER_PERIODIC 0x20000u

#define MSR_APIC_BASE 0x1Bu

static uint32_t g_timer_count;   /* LAPIC timer initial count for the tick rate */

static uint32_t lapic_read(uint32_t reg) { return mmio_read32(g_lapic_base + reg); }
static void lapic_write(uint32_t reg, uint32_t val) { mmio_write32(g_lapic_base + reg, val); }

uint32_t vibeos_x86_64_lapic_id(void) {
    return lapic_read(LAPIC_ID) >> 24;
}

void vibeos_x86_64_lapic_eoi(void) {
    lapic_write(LAPIC_EOI, 0);
}

/* Enable this CPU's local APIC and accept all priorities. Safe to call on any
 * core; the LAPIC base MSR is per-CPU. */
void vibeos_x86_64_lapic_enable(uint32_t spurious_vector) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_APIC_BASE));
    lo |= 0x800u; /* global enable */
    __asm__ __volatile__("wrmsr" : : "c"(MSR_APIC_BASE), "a"(lo), "d"(hi));

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_SVR, 0x100u | (spurious_vector & 0xFFu));
}

/* Busy-wait `us` microseconds using PIT channel 2 as a one-shot reference.
 * Needs no interrupts, so it works during AP startup with IF clear. */
static void apic_udelay(uint32_t us) {
    uint32_t count = (uint32_t)(((uint64_t)us * 1193182ull) / 1000000ull);
    uint8_t gate;
    if (count == 0u) {
        count = 1u;
    }
    if (count > 0xFFFFu) {
        count = 0xFFFFu;
    }
    gate = (uint8_t)((ap_inb(0x61) & ~0x02u) | 0x01u); /* gate on, speaker off */
    ap_outb(0x61, (uint8_t)(gate & ~0x01u));
    ap_outb(0x43, 0xB2);                              /* ch2, lo/hi, mode 0 */
    ap_outb(0x42, (uint8_t)(count & 0xFFu));
    ap_outb(0x42, (uint8_t)(count >> 8));
    ap_outb(0x61, gate);                              /* start counting */
    while ((ap_inb(0x61) & 0x20u) == 0u) {
        __asm__ __volatile__("pause" ::: "memory");
    }
}

/* Calibrate the LAPIC timer against the PIT, then run it periodically at `hz`
 * delivering `vector`. The calibration result is cached so APs reuse it. */
void vibeos_x86_64_lapic_timer_start(uint32_t hz, uint32_t vector) {
    lapic_write(LAPIC_TIMER_DIV, 0x3);      /* divide by 16 */

    if (g_timer_count == 0u) {
        uint32_t elapsed;
        lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
        lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
        apic_udelay(10000u);                /* 10 ms */
        elapsed = 0xFFFFFFFFu - lapic_read(LAPIC_TIMER_CUR);
        lapic_write(LAPIC_TIMER_INIT, 0);
        /* elapsed counts per 10 ms -> counts per tick at `hz`. */
        g_timer_count = (elapsed * 100u) / (hz ? hz : 100u);
        if (g_timer_count < 1000u) {
            g_timer_count = 1000u;          /* implausibly slow read: use a floor */
        }
        vibeos_x86_64_serial_puts("[APIC] LAPIC timer calibrated: count=0x");
        vibeos_x86_64_serial_print_hex(g_timer_count);
        vibeos_x86_64_serial_puts("\n");
    }

    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | (vector & 0xFFu));
    lapic_write(LAPIC_TIMER_INIT, g_timer_count);
}

/* ---- IO-APIC ------------------------------------------------------------- */

static void ioapic_write(uint32_t reg, uint32_t val) {
    mmio_write32(g_ioapic_base + 0x00, reg);
    mmio_write32(g_ioapic_base + 0x10, val);
}

/* Route an ISA IRQ to `vector` on the CPU with `dest` local-APIC id, honouring
 * any ACPI interrupt source override for polarity/trigger and GSI remapping. */
int vibeos_x86_64_ioapic_route(uint8_t irq, uint8_t vector, uint32_t dest) {
    uint32_t gsi = irq;
    uint16_t flags = 0;
    uint32_t low, high, reg, i;

    if (g_ioapic_base == 0u) {
        return -1;
    }
    for (i = 0; i < g_iso_count; i++) {
        if (g_iso[i].source == irq) {
            gsi = g_iso[i].gsi;
            flags = g_iso[i].flags;
            break;
        }
    }
    if (gsi < g_ioapic_gsi_base) {
        return -1;
    }
    reg = 0x10u + (gsi - g_ioapic_gsi_base) * 2u;

    low = vector;                       /* fixed delivery, physical dest, unmasked */
    if ((flags & 0x3u) == 0x3u) {
        low |= 0x2000u;                 /* active low  */
    }
    if ((flags & 0xCu) == 0xCu) {
        low |= 0x8000u;                 /* level triggered */
    }
    high = dest << 24;

    ioapic_write(reg + 1u, high);
    ioapic_write(reg, low);
    return 0;
}

/* ---- SMP startup --------------------------------------------------------- */

#define AP_TRAMPOLINE_ADDR 0x8000ull
#define AP_OFF_CR3 0xF00u
#define AP_OFF_STACK 0xF08u
#define AP_OFF_ENTRY 0xF10u
#define AP_OFF_GDTR 0xF20u
#define AP_OFF_GDT 0xF40u

extern const uint8_t vibeos_ap_trampoline_start[];
extern const uint8_t vibeos_ap_trampoline_end[];

/* Set to 1 by the AP itself once it reaches C; the BSP polls it. */
volatile uint32_t vibeos_x86_64_ap_alive;

static void ap_install_trampoline(uint64_t cr3) {
    uint8_t *dst = (uint8_t *)(uintptr_t)AP_TRAMPOLINE_ADDR;
    uint64_t len = (uint64_t)(vibeos_ap_trampoline_end - vibeos_ap_trampoline_start);
    uint64_t *gdt = (uint64_t *)(uintptr_t)(AP_TRAMPOLINE_ADDR + AP_OFF_GDT);
    uint8_t *gdtr = dst + AP_OFF_GDTR;
    uint64_t gdt_addr = AP_TRAMPOLINE_ADDR + AP_OFF_GDT;
    uint16_t limit = (uint16_t)(4u * 8u - 1u);
    uint32_t i;

    for (i = 0; i < len; i++) {
        dst[i] = vibeos_ap_trampoline_start[i];
    }

    /* The trampoline's own GDT: 32-bit code/data to leave real mode, then a
     * 64-bit code segment (L=1) to enter long mode. */
    gdt[0] = 0x0000000000000000ull;
    gdt[1] = 0x00CF9A000000FFFFull;  /* 0x08: 32-bit code */
    gdt[2] = 0x00CF92000000FFFFull;  /* 0x10: 32-bit data */
    gdt[3] = 0x00AF9A000000FFFFull;  /* 0x18: 64-bit code */

    gdtr[0] = (uint8_t)(limit & 0xFFu);
    gdtr[1] = (uint8_t)(limit >> 8);
    gdtr[2] = (uint8_t)(gdt_addr & 0xFFu);
    gdtr[3] = (uint8_t)((gdt_addr >> 8) & 0xFFu);
    gdtr[4] = (uint8_t)((gdt_addr >> 16) & 0xFFu);
    gdtr[5] = (uint8_t)((gdt_addr >> 24) & 0xFFu);

    *(volatile uint64_t *)(uintptr_t)(AP_TRAMPOLINE_ADDR + AP_OFF_CR3) = cr3;
}

static void lapic_ipi(uint32_t dest, uint32_t low) {
    lapic_write(LAPIC_ICR_HIGH, dest << 24);
    lapic_write(LAPIC_ICR_LOW, low);
    /* Wait for delivery status to clear. */
    {
        uint32_t spins = 0;
        while ((lapic_read(LAPIC_ICR_LOW) & 0x1000u) != 0u && ++spins < 1000000u) {
            __asm__ __volatile__("pause" ::: "memory");
        }
    }
}

/* Wake one application processor and wait for it to report in.
 * `stack_top` is the ring-0 stack it starts on, `entry` its C entry point. */
int vibeos_x86_64_smp_start_cpu(uint32_t lapic_id, uint64_t cr3, uint64_t stack_top,
                                uint64_t entry) {
    uint32_t spins;

    ap_install_trampoline(cr3);
    *(volatile uint64_t *)(uintptr_t)(AP_TRAMPOLINE_ADDR + AP_OFF_STACK) = stack_top;
    *(volatile uint64_t *)(uintptr_t)(AP_TRAMPOLINE_ADDR + AP_OFF_ENTRY) = entry;
    vibeos_x86_64_ap_alive = 0;
    __asm__ __volatile__("sfence" ::: "memory");

    lapic_ipi(lapic_id, 0x00004500u);                       /* INIT assert   */
    apic_udelay(10000u);
    lapic_ipi(lapic_id, 0x00004600u | (AP_TRAMPOLINE_ADDR >> 12)); /* SIPI    */
    apic_udelay(200u);
    lapic_ipi(lapic_id, 0x00004600u | (AP_TRAMPOLINE_ADDR >> 12)); /* SIPI #2 */

    for (spins = 0; spins < 200u; spins++) {
        if (vibeos_x86_64_ap_alive) {
            return 0;
        }
        apic_udelay(1000u);
    }
    return -1;
}

/* Mask the legacy 8259 pair completely: with the IO-APIC routing external
 * interrupts, a PIC line would deliver a second, unacknowledged copy. */
void vibeos_x86_64_pic_disable(void) {
    ap_outb(0xA1, 0xFF);
    ap_outb(0x21, 0xFF);
}

int vibeos_x86_64_apic_available(void) {
    return g_acpi_ok && g_ioapic_base != 0u;
}
