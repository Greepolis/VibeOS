#include "vibeos/kernel.h"
#include "vibeos/arch_x86_64.h"

static int kernel_bootinfo_validate(const vibeos_boot_info_t *boot_info) {
    if (!boot_info) {
        vibeos_x86_64_serial_puts("[KERN] ERROR: boot_info is NULL\n");
        return -1;
    }
    if (boot_info->version != VIBEOS_BOOTINFO_VERSION) {
        vibeos_x86_64_serial_puts("[KERN] ERROR: boot_info version mismatch\n");
        return -1;
    }
    if (!boot_info->memory_map || boot_info->memory_map_entries == 0) {
        vibeos_x86_64_serial_puts("[KERN] ERROR: boot_info memory_map invalid\n");
        return -1;
    }
    return 0;
}

const char *vibeos_kernel_stage_name(vibeos_boot_stage_t stage) {
    switch (stage) {
        case VIBEOS_BOOT_STAGE_EARLY:
            return "early";
        case VIBEOS_BOOT_STAGE_MEMORY_READY:
            return "memory_ready";
        case VIBEOS_BOOT_STAGE_SCHED_READY:
            return "sched_ready";
        case VIBEOS_BOOT_STAGE_CORE_READY:
            return "core_ready";
        default:
            return "unknown";
    }
}

int vibeos_kmain(vibeos_kernel_t *kernel, const vibeos_boot_info_t *boot_info) {
    uint32_t timer_irq;
    
    /* Initialize serial output first (M2 requirement) */
    vibeos_x86_64_serial_init();
    vibeos_x86_64_serial_puts("[BOOT] VibeOS kernel starting...\n");
    
    if (!kernel || kernel_bootinfo_validate(boot_info) != 0) {
        vibeos_x86_64_serial_puts("[BOOT] FATAL: kernel validation failed\n");
        return -1;
    }
    
    vibeos_x86_64_serial_puts("[BOOT] Boot info validated\n");

    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_EARLY;
    kernel->boot_state.last_error_code = 0;
    vibeos_event_init(&kernel->boot_event);

    if (vibeos_pmm_init_from_boot_info(&kernel->pmm, boot_info, 4096) != 0) {
        kernel->boot_state.last_error_code = 1001;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: pmm_init failed\n");
        return -1;
    }
    vibeos_x86_64_serial_puts("[BOOT] Memory manager initialized\n");
    
    if (vibeos_vm_init(&kernel->kernel_aspace) != 0) {
        kernel->boot_state.last_error_code = 1003;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: vm_init failed\n");
        return -1;
    }
    vibeos_x86_64_serial_puts("[BOOT] Virtual memory initialized\n");
    
    if (vibeos_handle_table_init(&kernel->handles) != 0) {
        kernel->boot_state.last_error_code = 1008;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: handle_table_init failed\n");
        return -1;
    }
    if (vibeos_policy_init(&kernel->policy) != 0) {
        kernel->boot_state.last_error_code = 1010;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: policy_init failed\n");
        return -1;
    }
    if (vibeos_sec_token_init(&kernel->kernel_token, 0, 0, (1u << 0) | (1u << 1) | (1u << 2)) != 0) {
        kernel->boot_state.last_error_code = 1011;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: sec_token_init failed\n");
        return -1;
    }
    if (vibeos_sec_audit_init(&kernel->sec_audit) != 0) {
        kernel->boot_state.last_error_code = 1012;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: sec_audit_init failed\n");
        return -1;
    }
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_MEMORY_READY;
    vibeos_x86_64_serial_puts("[BOOT] Memory stage ready\n");

    if (vibeos_sched_init(&kernel->scheduler, 1) != 0) {
        kernel->boot_state.last_error_code = 1002;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: sched_init failed\n");
        return -1;
    }
    if (vibeos_proc_init(&kernel->proc_table) != 0) {
        kernel->boot_state.last_error_code = 1007;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: proc_init failed\n");
        return -1;
    }
    if (vibeos_timer_init(&kernel->timer, 1000) != 0) {
        kernel->boot_state.last_error_code = 1004;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: timer_init failed\n");
        return -1;
    }
    vibeos_intc_init(&kernel->intc);
    if (vibeos_x86_64_idt_init(&kernel->idt) != 0) {
        kernel->boot_state.last_error_code = 1005;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: idt_init failed\n");
        return -1;
    }
    if (vibeos_trap_state_init(&kernel->trap_state) != 0) {
        kernel->boot_state.last_error_code = 1009;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: trap_state_init failed\n");
        return -1;
    }
    if (vibeos_x86_64_idt_set(&kernel->idt, (uint32_t)vibeos_x86_64_timer_vector()) != 0) {
        kernel->boot_state.last_error_code = 1006;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: idt_set failed\n");
        return -1;
    }
    timer_irq = (uint32_t)vibeos_x86_64_timer_vector();
    if (vibeos_intc_bind_timer_irq(&kernel->intc, &kernel->timer, timer_irq) != 0) {
        kernel->boot_state.last_error_code = 1013;
        vibeos_x86_64_serial_puts("[BOOT] FATAL: intc_bind_timer_irq failed\n");
        return -1;
    }
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_SCHED_READY;
    vibeos_x86_64_serial_puts("[BOOT] Scheduler stage ready\n");

    vibeos_event_signal(&kernel->boot_event);
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_CORE_READY;
    
    /* M2 boot completion marker */
    vibeos_x86_64_serial_puts("[BOOT] BOOT_OK\n");
    vibeos_x86_64_serial_puts("[BOOT] VibeOS kernel ready for user-space\n");
    
    return 0;
}
