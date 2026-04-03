#include "vibeos/kernel.h"

static int kernel_bootinfo_validate(const vibeos_boot_info_t *boot_info) {
    if (!boot_info) {
        return -1;
    }
    if (boot_info->version != VIBEOS_BOOTINFO_VERSION) {
        return -1;
    }
    if (!boot_info->memory_map || boot_info->memory_map_entries == 0) {
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
    uintptr_t pmm_base;
    size_t pmm_size;

    if (!kernel || kernel_bootinfo_validate(boot_info) != 0) {
        return -1;
    }

    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_EARLY;
    kernel->boot_state.last_error_code = 0;
    vibeos_event_init(&kernel->boot_event);

    pmm_base = (uintptr_t)boot_info->memory_map[0].base;
    pmm_size = (size_t)boot_info->memory_map[0].length;
    if (vibeos_pmm_init(&kernel->pmm, pmm_base, pmm_size, 4096) != 0) {
        kernel->boot_state.last_error_code = 1001;
        return -1;
    }
    if (vibeos_vm_init(&kernel->kernel_aspace) != 0) {
        kernel->boot_state.last_error_code = 1003;
        return -1;
    }
    if (vibeos_handle_table_init(&kernel->handles) != 0) {
        kernel->boot_state.last_error_code = 1008;
        return -1;
    }
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_MEMORY_READY;

    if (vibeos_sched_init(&kernel->scheduler, 1) != 0) {
        kernel->boot_state.last_error_code = 1002;
        return -1;
    }
    if (vibeos_proc_init(&kernel->proc_table) != 0) {
        kernel->boot_state.last_error_code = 1007;
        return -1;
    }
    if (vibeos_timer_init(&kernel->timer, 1000) != 0) {
        kernel->boot_state.last_error_code = 1004;
        return -1;
    }
    vibeos_intc_init(&kernel->intc);
    if (vibeos_x86_64_idt_init(&kernel->idt) != 0) {
        kernel->boot_state.last_error_code = 1005;
        return -1;
    }
    if (vibeos_x86_64_idt_set(&kernel->idt, (uint32_t)vibeos_x86_64_timer_vector()) != 0) {
        kernel->boot_state.last_error_code = 1006;
        return -1;
    }
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_SCHED_READY;

    vibeos_event_signal(&kernel->boot_event);
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_CORE_READY;
    return 0;
}
