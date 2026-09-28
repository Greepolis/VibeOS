/* Application-processor bring-up: the local APIC on the bootstrap core, then
 * every other core woken, given its per-CPU area and an idle task, and handed to
 * the scheduler.
 *
 * The hardware half - ACPI's MADT, the local and IO APIC, INIT-SIPI-SIPI and the
 * real-mode trampoline - is apic.c, which knows nothing of tasks. This is the
 * half that does: it needs the per-CPU table and the idle tasks, which is why it
 * sits beside the scheduler rather than inside the driver.
 *
 * Lifted out of arch_hw.c whole (2026-09-28). It sat under a banner called
 * "APIC + SMP bring-up" that also covered starting userland, the supervisor
 * manifest and the memory totals - 640 lines of which this is 145. Nothing
 * changed in the move. */

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
#include "vibeos/log.h"
#include "vibeos/klog.h"
#include "vibeos/crash.h"
#include "vibeos/device.h"
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

/* APIC / SMP (apic.c + ap_boot.S). */
extern int vibeos_x86_64_acpi_init(uint64_t rsdp_addr);
extern uint32_t vibeos_x86_64_acpi_cpu_count(void);
extern uint32_t vibeos_x86_64_acpi_lapic_id(uint32_t index);
extern void vibeos_x86_64_lapic_enable(uint32_t spurious_vector);
extern void vibeos_x86_64_lapic_timer_start(uint32_t hz, uint32_t vector);
extern uint32_t vibeos_x86_64_lapic_id(void);
extern int vibeos_x86_64_ioapic_route(uint8_t irq, uint8_t vector, uint32_t dest);
extern int vibeos_x86_64_smp_start_cpu(uint32_t lapic_id, uint64_t cr3, uint64_t stack_top,
                                       uint64_t entry);
extern void vibeos_x86_64_pic_disable(void);
extern int vibeos_x86_64_apic_available(void);
extern volatile uint32_t vibeos_x86_64_ap_alive;

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
void hw_apic_bringup(const vibeos_boot_info_t *boot_info) {
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
void hw_smp_bringup(void) {
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
