#include "vibeos/kernel.h"
#include "vibeos/arch_x86_64.h"
#include "vibeos/mm_model.h"
#include "vibeos/task_stats.h"
#include "vibeos/exec_stats.h"
#include "vibeos/backing.h"
#include "vibeos/rmap.h"
#include "vibeos/io_stats.h"
#include "vibeos/swapmap.h"
#include "vibeos/swaparea.h"
#include "vibeos/anon.h"
#include "vibeos/reclaim.h"
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
    vibeos_x86_64_serial_puts("[CLI] Commands: help, status, log, meminfo, tasks, exec, crash, echo <text>, halt, reboot\n");
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

/* The real one lives with the task table, which the host test binary does not
 * link. Weak, so the kernel build still gets the version that can signal a
 * process group and the tests get one that does nothing. */
/* The wait counters, for a link that has no arch layer.
 *
 * Weak, and in the same translation unit as the caller - which is the one
 * arrangement CLAUDE.md says is portable, because PE/COFF will not resolve a
 * weak definition living in a different object and the Windows CI job builds
 * this core with mingw. Every other weak stub in this kernel sits beside its
 * caller for the same reason.
 *
 * Zero is the honest answer here: a build with no disk and no network card had
 * no wait to time out. */
__attribute__((weak)) uint64_t vibeos_x86_64_blk_timeouts(void) { return 0ull; }
__attribute__((weak)) uint64_t vibeos_x86_64_virtio_net_tx_timeouts(void) { return 0ull; }

__attribute__((weak)) void vibeos_x86_64_console_interrupt(void) { }

/* The host test binary links kmain without the arch layer, and has no
 * userland to start. */
__attribute__((weak)) void vibeos_x86_64_hw_start_userland(void) { }

__attribute__((weak)) void vibeos_x86_64_log_dump_recent(uint32_t want) { (void)want; }

/* Same arrangement: the crash records live with the task table. */
__attribute__((weak)) void vibeos_x86_64_crash_dump(void) {
    vibeos_x86_64_serial_puts("[CRASH] no crash recorder in this build\n");
}

/* meminfo: what memory is being used for, and what has happened to it.
 *
 * Two halves on purpose. The counters say what has *happened* - pages mapped,
 * copies fork forced, references refused - and the usage figures say what *is*,
 * which is the question somebody actually asks when a machine misbehaves. This
 * project has spent whole sessions inferring both from a serial log.
 *
 * Figures the current kernel cannot answer are printed as "not measured"
 * rather than as zero. A memory tool that reports an invented number is worse
 * than one that admits a gap, because the invented number is the one somebody
 * acts on. They fill in as the phases in docs/mm/ land.
 */
static void kernel_cli_print_meminfo(void) {
    const vibeos_mm_stats_t *st = vibeos_mm_stats();
    vibeos_mm_usage_t use;
    unsigned i;

    vibeos_x86_64_serial_puts("[MEM] bytes total=0x");
    if (vibeos_mm_usage(&use) != 0) {
        vibeos_x86_64_serial_puts("unavailable\n");
        return;
    }
    kernel_log_u64_hex(use.bytes_total);
    vibeos_x86_64_serial_puts(" free=0x");
    kernel_log_u64_hex(use.bytes_free);
    vibeos_x86_64_serial_puts(" reserved=0x");
    kernel_log_u64_hex(use.bytes_reserved);
    /* Said out loud because it is the one figure here that is not a slice of
     * the frame table: it also covers the low user window, which is taken out
     * before the allocator's region even begins. Somebody adding the frame
     * states up and comparing them to this number deserves to be told why they
     * differ rather than to find out by not trusting either. */
    vibeos_x86_64_serial_puts(" (reserved includes the low user window, "
                              "which is outside the frame table)\n");

    vibeos_x86_64_serial_puts("[MEM] frames");
    for (i = 0; i < (unsigned)VIBEOS_FRAME_STATE_COUNT; i++) {
        vibeos_x86_64_serial_puts(" ");
        vibeos_x86_64_serial_puts(vibeos_frame_state_name((vibeos_frame_state_t)i));
        vibeos_x86_64_serial_puts("=0x");
        kernel_log_u64_hex(use.frames_by_state[i]);
    }
    vibeos_x86_64_serial_puts("\n");

    /* Fragmentation, which is a different question from "how much is free" and
     * the one that actually decides whether a large contiguous request can be
     * served. Free memory scattered in single frames will not give you a 4 MiB
     * staging buffer, and without this number that only shows up as an
     * allocation failure with no explanation attached. Compaction is plan P6;
     * being able to see the problem comes first. */
    vibeos_x86_64_serial_puts("[MEM] largest_free_run=0x");
    kernel_log_u64_hex(use.largest_free_run);
    vibeos_x86_64_serial_puts(" frames (0x");
    kernel_log_u64_hex(use.largest_free_run * 4096ull);
    vibeos_x86_64_serial_puts(" bytes contiguous)\n");

    /* Not measured yet: said plainly, with the phase that will answer it.
     * The frame states above are counted; this split needs to know *who* owns
     * a frame, which is the address-space layer at plan P2. */
    vibeos_x86_64_serial_puts("[MEM] kernel/user/shared/cache bytes: not measured "
                              "until ownership is recorded (plan P2)\n");

    vibeos_x86_64_serial_puts("[MEM] maps=0x");
    kernel_log_u64_hex(st->maps);
    vibeos_x86_64_serial_puts(" unmaps=0x");
    kernel_log_u64_hex(st->unmaps);
    vibeos_x86_64_serial_puts(" cow_shared=0x");
    kernel_log_u64_hex(st->cow_shared);
    vibeos_x86_64_serial_puts(" cow_copied=0x");
    kernel_log_u64_hex(st->cow_copied);
    vibeos_x86_64_serial_puts("\n");

    vibeos_x86_64_serial_puts("[MEM] tlb_shootdowns=0x");
    kernel_log_u64_hex(st->tlb_shootdowns);
    vibeos_x86_64_serial_puts(" tlb_acks=0x");
    kernel_log_u64_hex(st->tlb_acks);
    vibeos_x86_64_serial_puts(" tlb_timeouts=0x");
    kernel_log_u64_hex(st->tlb_timeouts);
    vibeos_x86_64_serial_puts("\n");

    /* The three that must be zero. Printed together and last, so the eye lands
     * on them, and asserted by the boot gate. */
    vibeos_x86_64_serial_puts("[MEM] MUSTBEZERO frames_leaked=0x");
    kernel_log_u64_hex(st->frames_leaked);
    vibeos_x86_64_serial_puts(" frames_double_put=0x");
    kernel_log_u64_hex(st->frames_double_put);
    vibeos_x86_64_serial_puts(" poison_hits=0x");
    kernel_log_u64_hex(st->poison_hits);
    vibeos_x86_64_serial_puts(" double_allocs=0x");
    kernel_log_u64_hex(st->double_allocs);
    vibeos_x86_64_serial_puts(" free_while_mapped=0x");
    kernel_log_u64_hex(st->free_while_mapped);
    vibeos_x86_64_serial_puts(" fork_undercounted=0x");
    kernel_log_u64_hex(st->fork_undercounted);
    vibeos_x86_64_serial_puts(" rmap_mismatch=0x");
    kernel_log_u64_hex(st->rmap_mismatch);
    vibeos_x86_64_serial_puts(" rmap_cycles=0x");
    kernel_log_u64_hex(vibeos_rmap_stats()->cycles);
    vibeos_x86_64_serial_puts(" rmap_missing_remove=0x");
    kernel_log_u64_hex(vibeos_rmap_stats()->missing_remove);
    vibeos_x86_64_serial_puts(" rmap_peak=0x");
    kernel_log_u64_hex(vibeos_rmap_stats()->nodes_peak);
    vibeos_x86_64_serial_puts(" rmap_exhausted=0x");
    kernel_log_u64_hex(vibeos_rmap_stats()->exhausted);
    vibeos_x86_64_serial_puts("\n");

    vibeos_x86_64_serial_puts("[MEM] cache_hits=0x");
    kernel_log_u64_hex(st->cache_hits);
    vibeos_x86_64_serial_puts(" cache_misses=0x");
    kernel_log_u64_hex(st->cache_misses);
    vibeos_x86_64_serial_puts(" swap_ins=0x");
    kernel_log_u64_hex(st->swap_ins);
    vibeos_x86_64_serial_puts(" swap_outs=0x");
    kernel_log_u64_hex(st->swap_outs);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_puts("[MEM] vmas_live=0x");
    kernel_log_u64_hex(st->vmas_live);
    vibeos_x86_64_serial_puts(" vmas_peak=0x");
    kernel_log_u64_hex(st->vmas_peak);
    vibeos_x86_64_serial_puts(" vmas_created=0x");
    kernel_log_u64_hex(st->vmas_created);
    vibeos_x86_64_serial_puts(" vmas_split=0x");
    kernel_log_u64_hex(st->vmas_split);
    vibeos_x86_64_serial_puts(" vmas_refused=0x");
    kernel_log_u64_hex(st->vmas_refused);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_puts("[MEM] end\n");
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
        if (ch == 3) {
            /* Ctrl-C is not a character here either. The PS/2 path has always
             * turned it into a signal, with a comment saying so; this one
             * dropped it on the floor along with every other control byte. So
             * the foreground process group, and everything built on top of it,
             * were unreachable from the console VibeOS is actually driven
             * through - the machinery existed and no input could reach it. */
            vibeos_x86_64_serial_puts("^C\n");
            vibeos_x86_64_console_interrupt();
            buffer[0] = '\0';
            return 0;
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
            /* And the ring that holds what the machine actually did. These are
             * two different logs: the one above records boot stages, this one
             * records fork, exec, exit, signals and memory. Showing only the
             * first was showing the table of contents and not the book. */
            vibeos_x86_64_log_dump_recent(24u);
            continue;
        }
        /* The last process to die from a fault, in full: registers, the fault
         * address, and as much of its stack as was readable. Captured at the
         * moment of the fault, because by the time anyone asks, the process is
         * gone - which is why every hard bug here has been diagnosed twice. */
        if (kernel_str_eq(line, "tasks")) {
            /* The table and the counters together: what the tasks are, and
             * what has happened to them. Reading one without the other has
             * been the shape of every task investigation here - a snapshot
             * with no history, or a history with no snapshot. */
            vibeos_task_print_table();
            vibeos_task_print_stats();
            vibeos_x86_64_serial_puts("[TASKS] end\n");
            continue;
        }
        if (kernel_str_eq(line, "exec")) {
            /* Why programs failed to start, by reason. Before this, every way
             * of failing printed the same sentence, so the log could not tell
             * an absent file from a short one from an exhausted allocator. */
            vibeos_exec_audit_cache();
            vibeos_exec_print_stats();
            continue;
        }
        if (kernel_str_eq(line, "meminfo")) {
            kernel_cli_print_meminfo();
            continue;
        }
        if (kernel_str_eq(line, "crash")) {
            vibeos_x86_64_crash_dump();
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

int vibeos_kernel_dispatch_trap(vibeos_kernel_t *kernel, const vibeos_trap_frame_t *frame, uint32_t current_pid, vibeos_trap_decision_t *out_decision) {
    vibeos_trap_decision_t local_decision;
    vibeos_trap_decision_t *decision = out_decision ? out_decision : &local_decision;
    if (!kernel || !frame) {
        return -1;
    }
    if (vibeos_trap_dispatch_ex(&kernel->trap_state, frame, &kernel->log, current_pid, decision) != 0) {
        return -1;
    }
    if (decision->action == VIBEOS_TRAP_ACTION_KILL_CURRENT) {
        if (vibeos_proc_terminate(&kernel->proc_table, current_pid) != 0) {
            kernel->boot_failure_fatal = 1;
            kernel_boot_log(kernel, VIBEOS_LOG_FATAL, 1202, current_pid, frame->vector, "trap_kill_failed");
            return -1;
        }
        kernel_boot_log(kernel, VIBEOS_LOG_WARN, 1200, current_pid, frame->vector, "trap_user_process_terminated");
        return 0;
    }
    if (decision->action == VIBEOS_TRAP_ACTION_PANIC) {
        kernel->boot_failure_fatal = 1;
        kernel->boot_state.last_error_code = 1201;
        kernel_boot_log(kernel, VIBEOS_LOG_FATAL, 1201, frame->vector, frame->rip, "trap_kernel_panic");
        return 0;
    }
    return 0;
}

/* Portable kernel entry, called once the architecture layer has finished
 * bring-up (descriptor tables, paging, interrupts, scheduler, devices).
 *
 * Brings the portable subsystems up in dependency order - logging, memory,
 * virtual memory, interrupt registration, timer, scheduler - recording the
 * boot stage as it goes so a failure reports how far it got. Each step is
 * checked; the first failure marks the kernel unhealthy and stops rather than
 * continuing on a half-initialized system.
 *
 * Returns 0 when the system reached the ready state, non-zero otherwise.
 */
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
    /* Logging first: every later step reports through it, so a failure
     * after this point is visible rather than silent. */
    if (vibeos_log_init(&kernel->log) != 0) {
        vibeos_x86_64_serial_puts("[BOOT] FATAL: log_init failed\n");
        return -1;
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_LOG_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 1, 0, 0, "kernel_boot_start");
    vibeos_event_init(&kernel->boot_event);

    /* Physical memory next: the frame allocator underpins everything that
     * follows, and it can only be built from the firmware memory map. */
    if (vibeos_pmm_init_from_boot_info(&kernel->pmm, boot_info, 4096) != 0) {
        return kernel_boot_fail(kernel, 1001, "pmm_init failed");
    }
    if (vibeos_pmm_remaining(&kernel->pmm) < kernel->pmm.page_size) {
        return kernel_boot_fail(kernel, 1001, "pmm has no free page");
    }
    kernel->boot_health_flags |= VIBEOS_BOOT_HEALTH_PMM_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 100, kernel->pmm.page_size, vibeos_pmm_remaining(&kernel->pmm), "pmm_ready");
    vibeos_x86_64_serial_puts("[BOOT] Memory manager initialized\n");
    
    /* Virtual memory on top of the frame allocator. */
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

    /* The scheduler last: it needs memory and a live timer before it can
     * put anything on a run queue. */
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
    
    /* The kernel is up. This is the honest place to say so: everything the
     * kernel needs exists, and nothing of userland has run yet. It used to be
     * printed after the entire userland had already finished. */
    vibeos_x86_64_serial_puts("[BOOT] BOOT_OK\n");

    /* Now the machine does its work. Returns when every user task has
     * retired, which is what leaves the serial console free for the CLI
     * below.
     *
     * Both ends are recorded, at INFO, in the kernel log as well as on the
     * serial line. The boundary between "the kernel is up" and "userland is
     * running" is the single most useful fact when a boot goes wrong - it was
     * invisible until now, and its absence is why every userland hang was
     * being attributed to the bootloader. */
    kernel->boot_state.stage = VIBEOS_BOOT_STAGE_CORE_READY;
    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 105, kernel->boot_health_flags, 0,
                    "userland_starting");
    vibeos_x86_64_serial_puts("[BOOT] USERLAND_START\n");

    /* Free frames on the way in and on the way out, in one line each.
     *
     * P7's leak property, and the only one of the five that costs nothing to
     * run: every user process that started before this point has exited by the
     * time userland finishes, so whatever memory they held has been given back
     * and the two numbers should agree.
     *
     * It is worth its own assertion because a leak of one frame per fork is
     * invisible in any single measurement - meminfo looks healthy, the totals
     * still partition, and nothing fails until a long-lived machine runs out
     * hours later with no event to point at. A difference here names it on the
     * boot that introduced it.
     *
     * The pair is deliberately not a single computed delta: the two raw
     * numbers say which direction it went, and *fewer* free frames afterwards
     * is a leak while *more* is a frame released twice - two different
     * defects, and this subsystem has produced both. */
    {
        uint64_t before = vibeos_mm_stats()->frames_free;
        vibeos_x86_64_serial_puts("[MM] FRAMES_AT_USERLAND_START=0x");
        kernel_log_u64_hex(before);
        vibeos_x86_64_serial_puts("\n");
    }

    vibeos_x86_64_hw_start_userland();

    kernel_boot_log(kernel, VIBEOS_LOG_INFO, 106, kernel->boot_health_flags, 0,
                    "userland_finished");
    {
        /* The page cache is reported alongside, because it is the one pool
         * that legitimately still holds frames here and it is large enough to
         * swamp the thing being measured.
         *
         * The first version of this check compared the two free counts alone
         * and found 1848 frames "leaked". They were not leaked - the cache was
         * holding 1820 of them on purpose, which is what a page cache is for.
         * An assertion on the raw difference would have accused correct
         * behaviour on every boot, and the natural next move would have been to
         * loosen it until it passed, at which point it would no longer catch
         * the leak it exists for.
         *
         * So the line carries both numbers and the gate does the arithmetic:
         * free + cache-resident, compared against the free count on the way in.
         * Anything the cache is holding is accounted for by name rather than
         * absorbed into a fudge factor. */
        vibeos_x86_64_serial_puts("[MM] FRAMES_AT_USERLAND_DONE=0x");
        kernel_log_u64_hex(vibeos_mm_stats()->frames_free);
        vibeos_x86_64_serial_puts(" cache_resident=0x");
        kernel_log_u64_hex((uint64_t)vibeos_cache_resident());
        vibeos_x86_64_serial_puts("\n");
    }
    /* What the disk did, in one line under the console lock.
     *
     * The storage path carried no counters at all until now, so "the disk is
     * slow", "the disk is retrying" and "the disk is fine" were the same
     * silence. The results are printed by reason rather than as a total,
     * because an absent device and a broken one are different states and only
     * one of them is a defect.
     *
     * One call, bracketed: a line assembled from several is several critical
     * sections, and this project has had a gate report failures that were a
     * marker cut in half. */
    {
        const vibeos_io_stats_t *io = vibeos_io_stats();
        uint32_t r;

        vibeos_x86_64_serial_lock();
        vibeos_x86_64_serial_puts("[IO] MUSTBEZERO medium=0x");
        kernel_log_u64_hex(io->results[VIBEOS_BLK_MEDIUM]);
        vibeos_x86_64_serial_puts(" short=0x");
        kernel_log_u64_hex(io->results[VIBEOS_BLK_SHORT]);
        vibeos_x86_64_serial_puts(" timeout=0x");
        kernel_log_u64_hex(io->results[VIBEOS_BLK_TIMEOUT]);
        vibeos_x86_64_serial_puts(" bad_request=0x");
        kernel_log_u64_hex(io->results[VIBEOS_BLK_BAD_REQUEST]);
        vibeos_x86_64_serial_puts(" out_of_range=0x");
        kernel_log_u64_hex(io->results[VIBEOS_BLK_OUT_OF_RANGE]);
        vibeos_x86_64_serial_puts(" register_refused=0x");
        kernel_log_u64_hex(io->register_refused);
        vibeos_x86_64_serial_puts("\n");

        /* The waits, on their own line and in the must-be-zero family.
         *
         * P7 asks that no path reachable from a syscall waits for another core
         * or a device without a bound, and that the bound is asserted. The
         * bounds were all there; what was missing was any way to tell one had
         * fired. virtio-net counted its transmit timeouts and nothing read the
         * number, and neither disk driver counted at all - so the timeout the
         * gate asserts to be zero was a result no driver could produce. */
        /* Swap, in two lines: what must be zero, and what happened.
         *
         * Split that way on purpose. slot_leaked, double_free and out_of_range
         * are defects - a leaked slot is swap space no reboot gets back, and
         * out_of_range is a transfer that would have landed on somebody else's
         * file. The rest are a description of a boot, and a boot with no
         * memory pressure legitimately shows zeroes throughout. Asserting on
         * those would be asserting that this machine ran short of memory. */
        {
            const vibeos_swap_stats_t *sw = vibeos_swap_stats();
            const vibeos_swaparea_stats_t *ar = vibeos_swaparea_stats();
            const vibeos_anon_stats_t *an = vibeos_anon_stats();
            const vibeos_reclaim_stats_t *rc = vibeos_reclaim_stats();

            vibeos_x86_64_serial_puts("[MM] SWAP MUSTBEZERO double_free=0x");
            kernel_log_u64_hex(sw->double_free);
            vibeos_x86_64_serial_puts(" out_of_range=0x");
            kernel_log_u64_hex(ar->out_of_range);
            vibeos_x86_64_serial_puts(" slot_leaked=0x");
            kernel_log_u64_hex(an->slot_leaked);
            vibeos_x86_64_serial_puts("\n");

            vibeos_x86_64_serial_puts("[MM] SWAP slots=0x");
            kernel_log_u64_hex((uint64_t)vibeos_swap_slots());
            vibeos_x86_64_serial_puts(" allocated=0x");
            kernel_log_u64_hex(sw->allocated);
            vibeos_x86_64_serial_puts(" peak=0x");
            kernel_log_u64_hex(sw->peak);
            vibeos_x86_64_serial_puts(" full=0x");
            kernel_log_u64_hex(sw->full);
            vibeos_x86_64_serial_puts(" sectors_written=0x");
            kernel_log_u64_hex(ar->sectors_written);
            vibeos_x86_64_serial_puts(" sectors_read=0x");
            kernel_log_u64_hex(ar->sectors_read);
            vibeos_x86_64_serial_puts("\n");

            vibeos_x86_64_serial_puts("[MM] RECLAIM scans=0x");
            kernel_log_u64_hex(rc->scans);
            vibeos_x86_64_serial_puts(" freed_clean=0x");
            kernel_log_u64_hex(rc->freed_clean);
            vibeos_x86_64_serial_puts(" freed_anon=0x");
            kernel_log_u64_hex(rc->freed_anon);
            vibeos_x86_64_serial_puts(" skipped_no_swap=0x");
            kernel_log_u64_hex(rc->skipped_no_swap);
            vibeos_x86_64_serial_puts(" anon_scanned=0x");
            kernel_log_u64_hex(an->scanned);
            vibeos_x86_64_serial_puts(" anon_evicted=0x");
            kernel_log_u64_hex(an->evicted);
            vibeos_x86_64_serial_puts(" anon_refused=0x");
            kernel_log_u64_hex(an->refused);
            vibeos_x86_64_serial_puts("\n");
        }

        vibeos_x86_64_serial_puts("[CONSOLE] MUSTBEZERO bad_unlocks=0x");
        kernel_log_u64_hex(vibeos_x86_64_serial_bad_unlocks());
        vibeos_x86_64_serial_puts(" stuck=0x");
        kernel_log_u64_hex(vibeos_x86_64_serial_stuck());
        vibeos_x86_64_serial_puts(" stuck_owner=0x");
        kernel_log_u64_hex((uint64_t)vibeos_x86_64_serial_stuck_owner());
        vibeos_x86_64_serial_puts("\n");

        vibeos_x86_64_serial_puts("[IO] WAITS blk_timeouts=0x");
        kernel_log_u64_hex(vibeos_x86_64_blk_timeouts());
        vibeos_x86_64_serial_puts(" net_tx_timeouts=0x");
        kernel_log_u64_hex(vibeos_x86_64_virtio_net_tx_timeouts());
        vibeos_x86_64_serial_puts("\n");

        vibeos_x86_64_serial_puts("[IO] BLK reads=0x");
        kernel_log_u64_hex(io->reads);
        vibeos_x86_64_serial_puts(" writes=0x");
        kernel_log_u64_hex(io->writes);
        vibeos_x86_64_serial_puts(" sectors_read=0x");
        kernel_log_u64_hex(io->sectors_read);
        vibeos_x86_64_serial_puts(" sectors_written=0x");
        kernel_log_u64_hex(io->sectors_written);
        vibeos_x86_64_serial_puts(" devices=0x");
        kernel_log_u64_hex(io->devices_registered);
        vibeos_x86_64_serial_puts(" no_device=0x");
        kernel_log_u64_hex(io->results[VIBEOS_BLK_NO_DEVICE]);
        vibeos_x86_64_serial_puts("\n");
        (void)r;
        vibeos_x86_64_serial_unlock();
    }
    vibeos_x86_64_serial_puts("[BOOT] USERLAND_DONE\n");
    vibeos_x86_64_serial_puts("[BOOT] VibeOS kernel ready for user-space\n");

    if (vibeos_x86_64_serial_available()) {
        vibeos_x86_64_serial_puts("[BOOT] CLI_READY\n");
        kernel_cli_run(kernel);
    }
    return 0;
}
