#include "vibeos/kernel.h"
#include "vibeos/arch_x86_64.h"
#include <stddef.h>

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

static int kernel_str_eq(const char *a, const char *b) {
    size_t i = 0;
    if (!a || !b) {
        return 0;
    }
    while (a[i] && b[i]) {
        if (a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return (a[i] == '\0' && b[i] == '\0') ? 1 : 0;
}

static int kernel_str_starts_with(const char *s, const char *prefix) {
    size_t i = 0;
    if (!s || !prefix) {
        return 0;
    }
    while (prefix[i]) {
        if (s[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static void kernel_log_u32_hex(uint32_t value) {
    int shift;
    for (shift = 28; shift >= 0; shift -= 4) {
        uint32_t nibble = (value >> (uint32_t)shift) & 0xfu;
        vibeos_x86_64_serial_putc((char)(nibble < 10u ? ('0' + nibble) : ('a' + nibble - 10u)));
    }
}

static void kernel_log_u64_hex(uint64_t value) {
    int shift;
    for (shift = 60; shift >= 0; shift -= 4) {
        uint64_t nibble = (value >> (uint32_t)shift) & 0xfu;
        vibeos_x86_64_serial_putc((char)(nibble < 10u ? ('0' + nibble) : ('a' + nibble - 10u)));
    }
}

static void kernel_boot_log(vibeos_kernel_t *kernel, vibeos_log_level_t level, uint32_t code, uint64_t arg0, uint64_t arg1, const char *message) {
    if (!kernel || !kernel->log.initialized) {
        return;
    }
    (void)vibeos_log_record(&kernel->log, level, code, arg0, arg1, message);
}

static int kernel_boot_fail(vibeos_kernel_t *kernel, size_t code, const char *message) {
    if (kernel) {
        kernel->boot_state.last_error_code = code;
        kernel->boot_failure_fatal = 1;
        kernel_boot_log(kernel, VIBEOS_LOG_FATAL, (uint32_t)code, kernel->boot_health_flags, 0, message);
    }
    vibeos_x86_64_serial_puts("[BOOT] FATAL: ");
    vibeos_x86_64_serial_puts(message ? message : "unknown");
    vibeos_x86_64_serial_puts("\n");
    return -1;
}

static void kernel_cli_print_help(void) {
    vibeos_x86_64_serial_puts("[CLI] Commands: help, status, log, echo <text>, halt, reboot\n");
}

static void kernel_cli_print_status(const vibeos_kernel_t *kernel) {
    uint32_t log_count = 0;
    uint32_t log_dropped = 0;
    vibeos_x86_64_serial_puts("[CLI] stage=");
    vibeos_x86_64_serial_puts(vibeos_kernel_stage_name(kernel->boot_state.stage));
    vibeos_x86_64_serial_puts(" health=0x");
    kernel_log_u32_hex(kernel->boot_health_flags);
    vibeos_x86_64_serial_puts(" fatal=");
    vibeos_x86_64_serial_putc(kernel->boot_failure_fatal ? '1' : '0');
    if (vibeos_log_count(&kernel->log, &log_count) == 0 && vibeos_log_dropped(&kernel->log, &log_dropped) == 0) {
        vibeos_x86_64_serial_puts(" log_count=0x");
        kernel_log_u32_hex(log_count);
        vibeos_x86_64_serial_puts(" log_dropped=0x");
        kernel_log_u32_hex(log_dropped);
    }
    vibeos_x86_64_serial_puts("\n");
}

static void kernel_cli_print_log(const vibeos_kernel_t *kernel) {
    uint32_t count = 0;
    uint32_t dropped = 0;
    uint32_t start = 0;
    uint32_t i;
    if (!kernel || vibeos_log_count(&kernel->log, &count) != 0 || vibeos_log_dropped(&kernel->log, &dropped) != 0) {
        vibeos_x86_64_serial_puts("[CLI] log unavailable\n");
        return;
    }
    vibeos_x86_64_serial_puts("[CLI] log_count=0x");
    kernel_log_u32_hex(count);
    vibeos_x86_64_serial_puts(" dropped=0x");
    kernel_log_u32_hex(dropped);
    vibeos_x86_64_serial_puts("\n");
    if (count > 5u) {
        start = count - 5u;
    }
    for (i = start; i < count; i++) {
        vibeos_log_event_t event;
        if (vibeos_log_get(&kernel->log, i, &event) != 0) {
            continue;
        }
        vibeos_x86_64_serial_puts("[CLI] log seq=0x");
        kernel_log_u64_hex(event.seq);
        vibeos_x86_64_serial_puts(" level=");
        vibeos_x86_64_serial_puts(vibeos_log_level_name((vibeos_log_level_t)event.level));
        vibeos_x86_64_serial_puts(" code=0x");
        kernel_log_u32_hex(event.code);
        vibeos_x86_64_serial_puts(" msg=");
        vibeos_x86_64_serial_puts(event.message);
        vibeos_x86_64_serial_puts("\n");
    }
}

static void kernel_cli_prompt(void) {
    vibeos_x86_64_serial_puts("vibeos> ");
}

static int kernel_cli_read_line(char *buffer, size_t buffer_size) {
    size_t len = 0;
    if (!buffer || buffer_size < 2u) {
        return -1;
    }
    for (;;) {
        int ch = vibeos_x86_64_serial_readc();
        if (ch < 0) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            vibeos_x86_64_serial_puts("\n");
            buffer[len] = '\0';
            return 0;
        }
        if (ch == 8 || ch == 127) {
            if (len > 0u) {
                len--;
                vibeos_x86_64_serial_puts("\b \b");
            }
            continue;
        }
        if (ch < 32 || ch > 126) {
            continue;
        }
        if (len + 1u >= buffer_size) {
            continue;
        }
        buffer[len++] = (char)ch;
        vibeos_x86_64_serial_putc((char)ch);
    }
}

static void kernel_cli_run(vibeos_kernel_t *kernel) {
    char line[128];
    vibeos_x86_64_serial_puts("[CLI] Serial console ready\n");
    kernel_cli_print_help();
    for (;;) {
        const char *payload;
        kernel_cli_prompt();
        if (kernel_cli_read_line(line, sizeof(line)) != 0) {
            continue;
        }
        if (line[0] == '\0') {
            continue;
        }
        if (kernel_str_eq(line, "help")) {
            kernel_cli_print_help();
            continue;
        }
        if (kernel_str_eq(line, "status")) {
            kernel_cli_print_status(kernel);
            continue;
        }
        if (kernel_str_eq(line, "log")) {
            kernel_cli_print_log(kernel);
            continue;
        }
        if (kernel_str_eq(line, "halt")) {
            vibeos_x86_64_serial_puts("[CLI] Halt requested\n");
            break;
        }
        if (kernel_str_eq(line, "reboot")) {
            vibeos_x86_64_serial_puts("[CLI] Reboot requested (not implemented)\n");
            break;
        }
        if (kernel_str_starts_with(line, "echo ")) {
            payload = line + 5;
            vibeos_x86_64_serial_puts(payload);
            vibeos_x86_64_serial_puts("\n");
            continue;
        }
        vibeos_x86_64_serial_puts("[CLI] Unknown command\n");
    }
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

int vibeos_kernel_boot_health(const vibeos_kernel_t *kernel, uint32_t *out_health_flags, uint32_t *out_failure_fatal) {
    if (!kernel || !out_health_flags || !out_failure_fatal) {
        return -1;
    }
    *out_health_flags = kernel->boot_health_flags;
    *out_failure_fatal = kernel->boot_failure_fatal;
    return 0;
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
    kernel->boot_health_flags = 0;
    kernel->boot_failure_fatal = 0;
    if (vibeos_log_init(&kernel->log) != 0) {
        vibeos_x86_64_serial_puts("[BOOT] FATAL: log_init failed\n");
        return -1;
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_LOG_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 1, 0, 0, "kernel_boot_start");
    vibeos_event_init(&kernel->boot_event);

    if (vibeos_pmm_init_from_boot_info(&kernel->pmm, boot_info, 4096) != 0) {
        return kernel_boot_fail(kernel, 1001, "pmm_init failed");
    }
    if (vibeos_pmm_remaining(&kernel->pmm) < kernel->pmm.page_size) {
        return kernel_boot_fail(kernel, 1001, "pmm has no free page");
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_PMM_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 100, kernel->pmm.page_size, vibeos_pmm_remaining(&kernel->pmm), "pmm_ready");
    vibeos_x86_64_serial_puts("[BOOT] Memory manager initialized\n");
    
    if (vibeos_vm_init(&kernel->kernel_aspace) != 0) {
        return kernel_boot_fail(kernel, 1003, "vm_init failed");
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_VM_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 101, 0, 0, "vm_ready");
    vibeos_x86_64_serial_puts("[BOOT] Virtual memory initialized\n");
    
    if (vibeos_handle_table_init(&kernel->handles) != 0) {
        return kernel_boot_fail(kernel, 1008, "handle_table_init failed");
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_HANDLES_READY;
    if (vibeos_policy_init(&kernel->policy) != 0) {
        return kernel_boot_fail(kernel, 1010, "policy_init failed");
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_POLICY_READY;
    if (vibeos_sec_token_init(&kernel->kernel_token, 0, 0, (1u << 0) | (1u << 1) | (1u << 2)) != 0) {
        return kernel_boot_fail(kernel, 1011, "sec_token_init failed");
    }
    if (vibeos_sec_audit_init(&kernel->sec_audit) != 0) {
        return kernel_boot_fail(kernel, 1012, "sec_audit_init failed");
    }
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_MEMORY_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 102, kernel->boot_health_flags, 0, "memory_stage_ready");
    vibeos_x86_64_serial_puts("[BOOT] Memory stage ready\n");

    if (vibeos_sched_init(&kernel->scheduler, 1) != 0) {
        return kernel_boot_fail(kernel, 1002, "sched_init failed");
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_SCHED_READY;
    if (vibeos_proc_init(&kernel->proc_table) != 0) {
        return kernel_boot_fail(kernel, 1007, "proc_init failed");
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_PROC_READY;
    if (vibeos_timer_init(&kernel->timer, 1000) != 0) {
        return kernel_boot_fail(kernel, 1004, "timer_init failed");
    }
    vibeos_intc_init(&kernel->intc);
    if (vibeos_x86_64_idt_init(&kernel->idt) != 0) {
        return kernel_boot_fail(kernel, 1005, "idt_init failed");
    }
    if (vibeos_trap_state_init(&kernel->trap_state) != 0) {
        return kernel_boot_fail(kernel, 1009, "trap_state_init failed");
    }
    if (vibeos_x86_64_idt_set(&kernel->idt, (uint32_t)vibeos_x86_64_timer_vector()) != 0) {
        return kernel_boot_fail(kernel, 1006, "idt_set failed");
    }
    timer_irq = (uint32_t)vibeos_x86_64_timer_vector();
    if (vibeos_intc_bind_timer_irq(&kernel->intc, &kernel->timer, timer_irq) != 0) {
        return kernel_boot_fail(kernel, 1013, "intc_bind_timer_irq failed");
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_IRQ_READY;
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_SCHED_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 103, kernel->boot_health_flags, timer_irq, "scheduler_stage_ready");
    vibeos_x86_64_serial_puts("[BOOT] Scheduler stage ready\n");

    vibeos_event_signal(&kernel->boot_event);
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_BOOT_EVENT_SIGNALLED;
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_CORE_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 104, kernel->boot_health_flags, 0, "core_stage_ready");
    
    /* M2 boot completion marker */
    vibeos_x86_64_serial_puts("[BOOT] BOOT_OK\n");
    vibeos_x86_64_serial_puts("[BOOT] VibeOS kernel ready for user-space\n");

    if (vibeos_x86_64_serial_available()) {
        vibeos_x86_64_serial_puts("[BOOT] CLI_READY\n");
        kernel_cli_run(kernel);
    }
    return 0;
}
