#include <stdio.h>
#include <string.h>

#include "vibeos/kernel.h"
#include "vibeos/bootloader.h"
#include "vibeos/driver_host.h"
#include "vibeos/drivers.h"
#include "vibeos/fs.h"
#include "vibeos/compat.h"
#include "vibeos/services.h"
#include "vibeos/userland.h"
#include "vibeos/security_model.h"
#include "vibeos/service_ipc.h"
#include "vibeos/policy.h"
#include "vibeos/syscall.h"
#include "vibeos/syscall_abi.h"
#include "vibeos/timer.h"
#include "vibeos/net.h"
#include "vibeos/inet.h"
#include "vibeos/elf.h"
#include "vibeos/fat_chain.h"
#include "vibeos/blockdev.h"
#include "vibeos/partition.h"
#include "vibeos/ext2.h"
#include "vibeos/iso9660.h"
#include "vibeos/exfat.h"
#include "vibeos/ntfs.h"
#include "vibeos/journal.h"
#include "vibeos/storage.h"
#include "vibeos/tls.h"
#include "vibeos/net_policy.h"
#include "vibeos/trap.h"
#include "vibeos/user_api.h"
#include "vibeos/vm.h"
#include "vibeos/waitset.h"
#include "vibeos/ipc_transfer.h"

static void irq_handler(uint32_t irq, void *ctx) {
    uint32_t *acc = (uint32_t *)ctx;
    if (acc) {
        *acc += irq;
    }
}

typedef struct handle_hook_stats {
    uint32_t alloc_events;
    uint32_t close_events;
    uint32_t revoke_events;
} handle_hook_stats_t;

static void handle_lifecycle_hook(vibeos_handle_lifecycle_event_t event, const vibeos_handle_entry_t *entry, void *ctx) {
    handle_hook_stats_t *stats = (handle_hook_stats_t *)ctx;
    (void)entry;
    if (!stats) {
        return;
    }
    if (event == VIBEOS_HANDLE_EVENT_ALLOC) {
        stats->alloc_events++;
    } else if (event == VIBEOS_HANDLE_EVENT_CLOSE) {
        stats->close_events++;
    } else if (event == VIBEOS_HANDLE_EVENT_REVOKE) {
        stats->revoke_events++;
    }
}

static int test_pmm(void) {
    vibeos_pmm_t pmm;
    vibeos_memory_region_t regions[3];
    vibeos_boot_info_t boot_info;
    uintptr_t picked_base = 0;
    size_t picked_size = 0;
    void *a;
    void *b;
    void *block;
    if (vibeos_pmm_init(&pmm, 0x100000, 8192, 4096) != 0) {
        return -1;
    }
    if (vibeos_pmm_allocated_pages(&pmm) != 0) {
        return -1;
    }
    a = vibeos_pmm_alloc_page(&pmm);
    b = vibeos_pmm_alloc_page(&pmm);
    if (!a || !b || a == b) {
        return -1;
    }
    if (vibeos_pmm_allocated_pages(&pmm) != 2) {
        return -1;
    }
    if (vibeos_pmm_alloc_page(&pmm) != 0) {
        return -1;
    }
    regions[0].base = 0x100000;
    regions[0].length = 0x9000;
    regions[0].type = VIBEOS_MEMORY_REGION_USABLE;
    regions[0].reserved = 0;
    regions[1].base = 0x200000;
    regions[1].length = 0x3000;
    regions[1].type = VIBEOS_MEMORY_REGION_RESERVED;
    regions[1].reserved = 0;
    regions[2].base = 0x300000;
    regions[2].length = 0x6000;
    regions[2].type = VIBEOS_MEMORY_REGION_USABLE;
    regions[2].reserved = 0;
    if (vibeos_bootloader_build_boot_info(&boot_info, regions, 3) != 0) {
        return -1;
    }
    if (vibeos_bootloader_set_initrd(&boot_info, 0x100000, 0x4000) != 0) {
        return -1;
    }
    if (vibeos_pmm_pick_usable_region(&boot_info, 0x1000, &picked_base, &picked_size) != 0) {
        return -1;
    }
    if (picked_base != 0x300000 || picked_size != 0x6000) {
        return -1;
    }
    if (vibeos_pmm_init_from_boot_info(&pmm, &boot_info, 0x1000) != 0) {
        return -1;
    }
    if (pmm.base != 0x300000 || pmm.size_bytes != 0x6000) {
        return -1;
    }
    block = vibeos_pmm_alloc_pages(&pmm, 2);
    if (block == 0 || (uintptr_t)block != 0x300000) {
        return -1;
    }
    if (vibeos_pmm_allocated_pages(&pmm) != 2 || vibeos_pmm_remaining(&pmm) != 0x4000) {
        return -1;
    }
    if (vibeos_pmm_alloc_pages(&pmm, 5) != 0) {
        return -1;
    }
    return 0;
}

static int test_scheduler(void) {
    vibeos_scheduler_t sched;
    vibeos_thread_t t1 = { .id = 1, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 0 };
    vibeos_thread_t t2 = { .id = 2, .cpu_hint = 0, .klass = VIBEOS_THREAD_INTERACTIVE, .timeslice_ticks = 2 };
    vibeos_thread_t *out;
    uint64_t wait_begin = 0;
    uint64_t wait_end = 0;
    uint64_t requeues = 0;
    uint64_t requeue_failures = 0;
    uint32_t loads[4];
    uint32_t load_entries = 0;
    vibeos_sched_thread_runtime_state_t runtime_state = VIBEOS_SCHED_THREAD_ABSENT;
    uint32_t runtime_cpu = 0;
    if (vibeos_sched_init(&sched, 1) != 0) {
        return -1;
    }
    if (vibeos_sched_enqueue(&sched, &t1) != 0 || vibeos_sched_enqueue(&sched, &t2) != 0) {
        return -1;
    }
    if (t1.timeslice_ticks != 4) {
        return -1;
    }
    if (vibeos_sched_runqueue_depth(&sched, 0) != 2 || vibeos_sched_runnable_threads(&sched) != 2) {
        return -1;
    }
    if (vibeos_sched_load_snapshot(&sched, loads, 4, &load_entries) != 0 || load_entries != 1 || loads[0] != 2) {
        return -1;
    }
    out = vibeos_sched_next(&sched, 0);
    if (!out || out->id != 1) {
        return -1;
    }
    out = vibeos_sched_next(&sched, 0);
    if (!out || out->id != 2) {
        return -1;
    }
    if (vibeos_sched_runqueue_depth(&sched, 0) != 0 || vibeos_sched_runnable_threads(&sched) != 0) {
        return -1;
    }
    out = &t2;
    out->timeslice_ticks = 1;
    if (vibeos_sched_tick(&sched, out, 0) != 1) {
        return -1;
    }
    if (vibeos_sched_preemptions(&sched, 0) != 1) {
        return -1;
    }
    if (vibeos_sched_wait_begin(&sched, 2, 0) != 0) {
        return -1;
    }
    if (vibeos_sched_blocked_threads(&sched) != 1 || vibeos_sched_runqueue_depth(&sched, 0) != 0) {
        return -1;
    }
    if (vibeos_sched_wait_end(&sched, 2, 0, &runtime_cpu) != 0 || runtime_cpu != 0) {
        return -1;
    }
    if (vibeos_sched_thread_runtime_get(&sched, 2, &runtime_state, &runtime_cpu, &wait_begin, &wait_end, 0) != 0) {
        return -1;
    }
    if (runtime_state != VIBEOS_SCHED_THREAD_RUNNABLE || runtime_cpu != 0 || wait_begin != 1 || wait_end != 1) {
        return -1;
    }
    if (vibeos_sched_wait_transition_summary(&sched, &wait_begin, &wait_end, &requeues, &requeue_failures) != 0) {
        return -1;
    }
    if (wait_begin != 1 || wait_end != 1 || requeues != 1 || requeue_failures != 0) {
        return -1;
    }
    return 0;
}

static int test_scheduler_balanced(void) {
    vibeos_scheduler_t sched;
    vibeos_thread_t t1 = { .id = 11, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    vibeos_thread_t t2 = { .id = 12, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    vibeos_thread_t t3 = { .id = 13, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    uint32_t cpu = 0;
    uint32_t aged = 0;
    if (vibeos_sched_init(&sched, 2) != 0) {
        return -1;
    }
    if (vibeos_sched_enqueue_balanced(&sched, &t1, &cpu) != 0 || cpu != 0) {
        return -1;
    }
    if (vibeos_sched_enqueue_balanced(&sched, &t2, &cpu) != 0 || cpu != 1) {
        return -1;
    }
    if (vibeos_sched_enqueue_balanced(&sched, &t3, &cpu) != 0 || cpu != 0) {
        return -1;
    }
    if (vibeos_sched_runqueue_depth(&sched, 0) != 2 || vibeos_sched_runqueue_depth(&sched, 1) != 1) {
        return -1;
    }
    if (vibeos_sched_least_loaded_cpu(&sched, &cpu) != 0 || cpu != 1) {
        return -1;
    }
    if (vibeos_sched_cpu_count(&sched, &cpu) != 0 || cpu != 2) {
        return -1;
    }
    if (vibeos_sched_age_cpu(&sched, 1, 2, 6, &aged) != 0 || aged != 1) {
        return -1;
    }
    if (t2.timeslice_ticks != 5) {
        return -1;
    }
    if (vibeos_sched_age_all(&sched, 4, 6, &aged) != 0 || aged != 3) {
        return -1;
    }
    if (t1.timeslice_ticks != 6 || t2.timeslice_ticks != 6 || t3.timeslice_ticks != 6) {
        return -1;
    }
    return 0;
}

static int test_scheduler_wait_runtime(void) {
    vibeos_scheduler_t sched;
    vibeos_thread_t t1 = { .id = 21, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    vibeos_thread_t t2 = { .id = 22, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    vibeos_sched_thread_runtime_state_t state = VIBEOS_SCHED_THREAD_ABSENT;
    uint32_t cpu = 0;
    uint64_t wait_begin = 0;
    uint64_t wait_end = 0;
    uint64_t migrations = 0;
    uint64_t total_wait_begin = 0;
    uint64_t total_wait_end = 0;
    uint64_t total_requeues = 0;
    uint64_t total_requeue_failures = 0;
    if (vibeos_sched_init(&sched, 2) != 0) {
        return -1;
    }
    if (vibeos_sched_enqueue(&sched, &t1) != 0 || vibeos_sched_enqueue(&sched, &t2) != 0) {
        return -1;
    }
    if (vibeos_sched_tracked_threads(&sched) != 2 || vibeos_sched_blocked_threads(&sched) != 0) {
        return -1;
    }
    if (vibeos_sched_wait_begin(&sched, t1.id, &cpu) != 0 || cpu != 0) {
        return -1;
    }
    if (vibeos_sched_blocked_threads(&sched) != 1 || vibeos_sched_runqueue_depth(&sched, 0) != 1) {
        return -1;
    }
    if (vibeos_sched_wait_end(&sched, t1.id, 1, &cpu) != 0 || cpu != 1) {
        return -1;
    }
    if (vibeos_sched_runqueue_depth(&sched, 0) != 1 || vibeos_sched_runqueue_depth(&sched, 1) != 1) {
        return -1;
    }
    if (vibeos_sched_thread_runtime_get(&sched, t1.id, &state, &cpu, &wait_begin, &wait_end, &migrations) != 0) {
        return -1;
    }
    if (state != VIBEOS_SCHED_THREAD_RUNNABLE || cpu != 1 || wait_begin != 1 || wait_end != 1 || migrations != 1) {
        return -1;
    }
    if (vibeos_sched_wait_transition_summary(&sched, &total_wait_begin, &total_wait_end, &total_requeues, &total_requeue_failures) != 0) {
        return -1;
    }
    if (total_wait_begin != 1 || total_wait_end != 1 || total_requeues != 1 || total_requeue_failures != 0) {
        return -1;
    }
    if (vibeos_sched_untrack_thread(&sched, t2.id) != 0 || vibeos_sched_tracked_threads(&sched) != 1) {
        return -1;
    }
    if (vibeos_sched_untrack_thread(&sched, t1.id) != 0 || vibeos_sched_tracked_threads(&sched) != 0) {
        return -1;
    }
    if (vibeos_sched_runnable_threads(&sched) != 0) {
        return -1;
    }
    return 0;
}

static int test_scheduler_qos_affinity(void) {
    vibeos_scheduler_t sched;
    vibeos_thread_t t1 = { .id = 31, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    vibeos_thread_t t2 = { .id = 32, .cpu_hint = 1, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    vibeos_thread_t t3 = { .id = 33, .cpu_hint = 2, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    vibeos_thread_t t4 = { .id = 34, .cpu_hint = 2, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    vibeos_thread_t t5 = { .id = 35, .cpu_hint = 2, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 3 };
    uint64_t affinity_mask = 0;
    uint32_t moved = 0;
    uint32_t boosted = 0;
    int32_t nice_level = 0;
    uint64_t rebalance_passes = 0;
    uint64_t rebalance_moves = 0;
    uint64_t affinity_misses = 0;
    uint64_t priority_boosts = 0;
    uint32_t wake_cpu = 0;
    if (vibeos_sched_init(&sched, 3) != 0) {
        return -1;
    }
    if (vibeos_sched_enqueue(&sched, &t1) != 0 || vibeos_sched_enqueue(&sched, &t2) != 0 || vibeos_sched_enqueue(&sched, &t3) != 0) {
        return -1;
    }
    if (vibeos_sched_set_thread_affinity(&sched, t1.id, (1ull << 2)) != 0) {
        return -1;
    }
    if (vibeos_sched_get_thread_affinity(&sched, t1.id, &affinity_mask) != 0 || affinity_mask != (1ull << 2)) {
        return -1;
    }
    if (vibeos_sched_wait_begin(&sched, t1.id, 0) != 0) {
        return -1;
    }
    if (vibeos_sched_wait_end(&sched, t1.id, 0, &wake_cpu) != 0 || wake_cpu != 2) {
        return -1;
    }
    if (vibeos_sched_set_thread_nice(&sched, t2.id, -10) != 0) {
        return -1;
    }
    if (vibeos_sched_get_thread_nice(&sched, t2.id, &nice_level) != 0 || nice_level != -10) {
        return -1;
    }
    if (vibeos_sched_enqueue(&sched, &t4) != 0 || vibeos_sched_enqueue(&sched, &t5) != 0) {
        return -1;
    }
    if (vibeos_sched_starvation_tick(&sched, 2) != 0 || vibeos_sched_starvation_tick(&sched, 2) != 0) {
        return -1;
    }
    if (vibeos_sched_boost_starving(&sched, 2, 2, 8, &boosted) != 0 || boosted == 0) {
        return -1;
    }
    if (vibeos_sched_rebalance(&sched, 4, &moved) != 0 || moved == 0) {
        return -1;
    }
    if (vibeos_sched_qos_summary(&sched, &rebalance_passes, &rebalance_moves, &affinity_misses, &priority_boosts) != 0) {
        return -1;
    }
    if (rebalance_passes == 0 || rebalance_moves == 0 || affinity_misses == 0 || priority_boosts == 0) {
        return -1;
    }
    return 0;
}

static int test_ipc(void) {
    vibeos_event_t event;
    vibeos_channel_t ch;
    vibeos_message_t send = { .code = 7, .payload = 0xAA55u };
    vibeos_message_t recv;
    uint32_t out_handle = 0;
    uint32_t out_rights = 0;

    vibeos_event_init(&event);
    if (vibeos_event_is_signaled(&event)) {
        return -1;
    }
    vibeos_event_signal(&event);
    if (!vibeos_event_is_signaled(&event)) {
        return -1;
    }
    vibeos_channel_init(&ch);
    if (vibeos_channel_send(&ch, send) != 0) {
        return -1;
    }
    if (vibeos_channel_recv(&ch, &recv) != 0) {
        return -1;
    }
    if (recv.code != send.code || recv.payload != send.payload) {
        return -1;
    }
    if (vibeos_channel_send_with_handle(&ch, 9, 0x55AAu, 17, VIBEOS_HANDLE_RIGHT_SIGNAL) != 0) {
        return -1;
    }
    if (vibeos_channel_recv_with_handle(&ch, &recv, &out_handle, &out_rights) != 0) {
        return -1;
    }
    if (recv.code != 9 || recv.payload != 0x55AAu || out_handle != 17 || out_rights != VIBEOS_HANDLE_RIGHT_SIGNAL) {
        return -1;
    }
    return 0;
}

static int test_kernel_log(void) {
    vibeos_log_t log;
    vibeos_log_event_t event;
    uint32_t count = 0;
    uint32_t dropped = 0;
    uint32_t i;

    if (vibeos_log_init(&log) != 0) {
        return -1;
    }
    if (vibeos_log_count(&log, &count) != 0 || count != 0) {
        return -1;
    }
    if (vibeos_log_record(&log, VIBEOS_LOG_INFO, 7, 11, 13, "boot_start") != 0) {
        return -1;
    }
    if (vibeos_log_latest(&log, &event) != 0) {
        return -1;
    }
    if (event.seq != 1 || event.level != VIBEOS_LOG_INFO || event.code != 7 || event.arg0 != 11 || event.arg1 != 13 || strcmp(event.message, "boot_start") != 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_LOG_CAPACITY + 3u; i++) {
        if (vibeos_log_record(&log, VIBEOS_LOG_WARN, 100u + i, i, 0, "fill") != 0) {
            return -1;
        }
    }
    if (vibeos_log_count(&log, &count) != 0 || count != VIBEOS_LOG_CAPACITY) {
        return -1;
    }
    if (vibeos_log_dropped(&log, &dropped) != 0 || dropped != 4u) {
        return -1;
    }
    if (vibeos_log_get(&log, 0, &event) != 0 || event.seq != 5u || event.code != 103u) {
        return -1;
    }
    if (vibeos_log_latest(&log, &event) != 0 || event.seq != VIBEOS_LOG_CAPACITY + 4u || event.code != 100u + VIBEOS_LOG_CAPACITY + 2u) {
        return -1;
    }
    if (strcmp(vibeos_log_level_name(VIBEOS_LOG_FATAL), "FATAL") != 0) {
        return -1;
    }
    return 0;
}

static int test_kmain(void) {
    vibeos_memory_region_t region;
    vibeos_boot_info_t boot;
    vibeos_kernel_t kernel;
    uint32_t health_flags = 0;
    uint32_t fatal_failure = 1;
    uint32_t log_count = 0;
    vibeos_log_event_t latest;

    memset(&kernel, 0, sizeof(kernel));
    memset(&boot, 0, sizeof(boot));
    region.base = 0x100000;
    region.length = 0x200000;
    region.type = 1;
    region.reserved = 0;

    boot.version = VIBEOS_BOOTINFO_VERSION;
    boot.flags = 0;
    boot.memory_map_entries = 1;
    boot.memory_map = &region;
    boot.acpi_rsdp = 0;
    boot.smbios_entry = 0;
    boot.initrd_base = 0;
    boot.initrd_size = 0;
    boot.framebuffer_base = 0;
    boot.framebuffer_width = 0;
    boot.framebuffer_height = 0;

    if (vibeos_kmain(&kernel, &boot) != 0) {
        return -1;
    }
    if (kernel.boot_state.stage != VIBEOS_BOOT_STAGE_CORE_READY) {
        return -1;
    }
    if (!vibeos_event_is_signaled(&kernel.boot_event)) {
        return -1;
    }
    if (vibeos_kernel_boot_health(&kernel, &health_flags, &fatal_failure) != 0) {
        return -1;
    }
    if ((health_flags & VIBEOS_BOOT_HEALTH_BOOT_EVENT_SIGNALLED) == 0 || fatal_failure != 0) {
        return -1;
    }
    if ((health_flags & VIBEOS_BOOT_HEALTH_LOG_READY) == 0) {
        return -1;
    }
    if (vibeos_log_count(&kernel.log, &log_count) != 0 || log_count < 5u) {
        return -1;
    }
    /* The boot order, asserted here rather than only in a serial log.
     *
     * This used to check that the last thing kmain logged was
     * "core_stage_ready", and that was right when kmain was the last thing to
     * run - the architecture layer had already executed the whole of userland
     * before kmain was ever entered, so BOOT_OK meant "everything finished".
     *
     * That inversion cost a full session of hunting a bootloader bug that did
     * not exist: the boot gate waits for BOOT_OK, so any userland hang was
     * reported against the last bootloader marker anyone had seen. The order is
     * now kernel first, then userland, and this is the cheapest place to keep
     * it that way - a host test, with no machine to boot.
     */
    if (vibeos_log_latest(&kernel.log, &latest) != 0 ||
        strcmp(latest.message, "userland_finished") != 0) {
        return -1;
    }
    {
        /* core_stage_ready, then userland_starting, then userland_finished -
         * in that order and each exactly once. */
        uint32_t i;
        int core_at = -1, start_at = -1, done_at = -1;

        for (i = 0; i < log_count; i++) {
            vibeos_log_event_t ev;
            if (vibeos_log_get(&kernel.log, i, &ev) != 0) {
                return -1;
            }
            if (strcmp(ev.message, "core_stage_ready") == 0) {
                if (core_at >= 0) { return -1; }
                core_at = (int)i;
            } else if (strcmp(ev.message, "userland_starting") == 0) {
                if (start_at >= 0) { return -1; }
                start_at = (int)i;
            } else if (strcmp(ev.message, "userland_finished") == 0) {
                if (done_at >= 0) { return -1; }
                done_at = (int)i;
            }
        }
        if (core_at < 0 || start_at < 0 || done_at < 0) {
            return -1;
        }
        if (!(core_at < start_at && start_at < done_at)) {
            return -1;
        }
    }
    return 0;
}

static int test_vm(void) {
    vibeos_address_space_t aspace;
    vibeos_address_space_t cloned;
    const vibeos_vm_map_t *found;
    uint32_t merged = 0;
    uintptr_t gap = 0;
    if (vibeos_vm_init(&aspace) != 0) {
        return -1;
    }
    if (vibeos_vm_map(&aspace, 0x400000, 0x100000, 0x2000, VIBEOS_VM_PERM_READ | VIBEOS_VM_PERM_WRITE) != 0) {
        return -1;
    }
    found = vibeos_vm_lookup(&aspace, 0x400010);
    if (!found || found->pa != 0x100000) {
        return -1;
    }
    if (vibeos_vm_map_count(&aspace) != 1 || vibeos_vm_total_mapped(&aspace) != 0x2000) {
        return -1;
    }
    if (vibeos_vm_validate(&aspace) != 0) {
        return -1;
    }
    if (vibeos_vm_protect(&aspace, 0x400000, 0x2000, VIBEOS_VM_PERM_READ) != 0) {
        return -1;
    }
    found = vibeos_vm_lookup(&aspace, 0x400010);
    if (!found || found->perms != VIBEOS_VM_PERM_READ) {
        return -1;
    }
    if (vibeos_vm_unmap_range(&aspace, 0x401000, 0x800) != 0) {
        return -1;
    }
    if (vibeos_vm_map_count(&aspace) != 2 || vibeos_vm_total_mapped(&aspace) != 0x1800) {
        return -1;
    }
    if (vibeos_vm_clone_readonly(&cloned, &aspace) != 0) {
        return -1;
    }
    found = vibeos_vm_lookup(&cloned, 0x400010);
    if (!found || (found->perms & VIBEOS_VM_PERM_WRITE) != 0) {
        return -1;
    }
    if (vibeos_vm_unmap(&aspace, 0x400000, 0x1000) != 0) {
        return -1;
    }
    if (vibeos_vm_unmap(&aspace, 0x401800, 0x800) != 0) {
        return -1;
    }
    if (vibeos_vm_map_count(&aspace) != 0) {
        return -1;
    }
    if (vibeos_vm_map(&aspace, 0x500000, 0x200000, 0x1000, VIBEOS_VM_PERM_READ) != 0) {
        return -1;
    }
    if (vibeos_vm_map(&aspace, 0x501000, 0x201000, 0x1000, VIBEOS_VM_PERM_READ) != 0) {
        return -1;
    }
    if (vibeos_vm_compact(&aspace, &merged) != 0 || merged != 1) {
        return -1;
    }
    if (vibeos_vm_map_count(&aspace) != 1 || vibeos_vm_total_mapped(&aspace) != 0x2000) {
        return -1;
    }
    if (vibeos_vm_map(&aspace, UINTPTR_MAX - 0x100, 0x300000, 0x1000, VIBEOS_VM_PERM_READ) == 0) {
        return -1;
    }
    if (vibeos_vm_find_gap(&aspace, 0x500000, 0x1000, 0x1000, &gap) != 0 || gap != 0x502000) {
        return -1;
    }
    if (vibeos_vm_find_gap(&aspace, 0x500000, 0x1000, 0x300, &gap) == 0) {
        return -1;
    }
    return 0;
}

static int test_vm_user_address_space_contract(void) {
    vibeos_address_space_t user_aspace;
    vibeos_address_space_t next_aspace;
    vibeos_vm_context_t ctx;
    uintptr_t kernel_va = VIBEOS_VM_KERNEL_BASE;

    if (vibeos_address_space_create(&user_aspace) != 0 || vibeos_address_space_create(&next_aspace) != 0) {
        return -1;
    }
    if (vibeos_vm_map_user(&user_aspace, 0x400000, 0x200000, VIBEOS_VM_PAGE_SIZE * 2u, VIBEOS_VM_PERM_READ | VIBEOS_VM_PERM_WRITE) != 0) {
        return -1;
    }
    if (vibeos_vm_validate_user_range(&user_aspace, 0x400000, 16, VIBEOS_VM_PERM_READ) != 0) {
        return -1;
    }
    if (vibeos_vm_validate_user_range(&user_aspace, 0x401000, VIBEOS_VM_PAGE_SIZE, VIBEOS_VM_PERM_WRITE) != 0) {
        return -1;
    }
    if (vibeos_vm_validate_user_range(&user_aspace, 0x402000, 1, VIBEOS_VM_PERM_READ) == 0) {
        return -1;
    }
    if (vibeos_vm_validate_user_range(&user_aspace, 0x400000, 16, VIBEOS_VM_PERM_EXEC) == 0) {
        return -1;
    }
    if (vibeos_vm_map_user(&user_aspace, kernel_va, 0x300000, VIBEOS_VM_PAGE_SIZE, VIBEOS_VM_PERM_READ) == 0) {
        return -1;
    }
    if (vibeos_vm_map_user(&user_aspace, 0x500000, 0x300000, VIBEOS_VM_PAGE_SIZE, VIBEOS_VM_PERM_WRITE | VIBEOS_VM_PERM_EXEC) == 0) {
        return -1;
    }
    if (vibeos_vm_map_kernel(&user_aspace, kernel_va, 0x400000, VIBEOS_VM_PAGE_SIZE, VIBEOS_VM_PERM_READ) != 0) {
        return -1;
    }
    if (vibeos_vm_validate_user_range(&user_aspace, kernel_va, 8, VIBEOS_VM_PERM_READ) == 0) {
        return -1;
    }
    if (vibeos_vm_map_user(&next_aspace, 0x600000, 0x500000, VIBEOS_VM_PAGE_SIZE, VIBEOS_VM_PERM_READ | VIBEOS_VM_PERM_EXEC) != 0) {
        return -1;
    }
    if (vibeos_vm_context_init(&ctx, &user_aspace) != 0) {
        return -1;
    }
    if (vibeos_vm_switch_address_space(&ctx, &next_aspace) != 0 || ctx.current != &next_aspace || ctx.switch_count != 1) {
        return -1;
    }
    if (vibeos_vm_switch_address_space(&ctx, &next_aspace) != 0 || ctx.switch_count != 1) {
        return -1;
    }
    return 0;
}

static int test_interrupts(void) {
    vibeos_interrupt_controller_t intc;
    uint32_t acc = 0;
    uint64_t denied_bad = 0;
    uint64_t denied_unhandled = 0;
    uint64_t denied_masked = 0;
    uint64_t denied_disabled = 0;
    vibeos_intc_init(&intc);
    if (vibeos_intc_register(&intc, 32, irq_handler, &acc) != 0) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, 32) != 0) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, 33) == 0) {
        return -1;
    }
    if (vibeos_intc_mask(&intc, 32) != 0 || vibeos_intc_is_masked(&intc, 32) != 1) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, 32) == 0) {
        return -1;
    }
    if (vibeos_intc_unmask(&intc, 32) != 0 || vibeos_intc_is_masked(&intc, 32) != 0) {
        return -1;
    }
    if (vibeos_intc_set_enabled(&intc, 0) != 0) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, 32) == 0) {
        return -1;
    }
    if (vibeos_intc_set_enabled(&intc, 1) != 0) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, 999) == 0) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, 32) != 0) {
        return -1;
    }
    if (acc != 64 || vibeos_intc_counter(&intc, 32) != 2) {
        return -1;
    }
    if (vibeos_intc_denied_counters(&intc, &denied_bad, &denied_unhandled, &denied_masked, &denied_disabled) != 0) {
        return -1;
    }
    if (denied_bad != 1 || denied_unhandled != 1 || denied_masked != 1 || denied_disabled != 1) {
        return -1;
    }
    if (vibeos_intc_counters_reset(&intc) != 0) {
        return -1;
    }
    if (vibeos_intc_counter(&intc, 32) != 0) {
        return -1;
    }
    return 0;
}

/* Exercises the syscall dispatcher end to end: ABI version negotiation,
 * handle allocation and closing, waitset operations and their statistics, and
 * the error paths for bad ids, bad arguments and policy denials.
 *
 * This is an integration-style contract test for syscall dispatch behavior,
 * not a unit test of any single syscall implementation. The body is organized
 * in phases that validate:
 *   1) successful calls and expected side effects,
 *   2) rejection paths (invalid ids/arguments),
 *   3) ownership and policy enforcement checks,
 *   4) observable accounting/statistics consistency.
 */
static int test_syscalls(void) {
    vibeos_kernel_t kernel;
    vibeos_syscall_frame_t frame;
    /* Test fixtures:
     * - Two threads in two processes are sufficient to trigger cross-owner
     *   access checks, which drive many negative dispatcher paths.
     * - The scalar ids/handles below are populated incrementally as setup and
     *   syscall phases progress, then reused by later assertions. */
    vibeos_thread_t sthread1 = { .id = 101, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 1 };
    vibeos_thread_t sthread2 = { .id = 102, .cpu_hint = 1, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 2 };
    uint32_t pid1 = 0;
    uint32_t pid2 = 0;
    uint32_t pid3 = 0;
    uint32_t p1_handle = 0;
    uint32_t tid1 = 0;
    uint32_t signal_handle = 0;
    uint32_t waitset_event_handle = 0;
    uint32_t revoke_root = 0;
    uint32_t revoke_dup = 0;
    uint32_t i;
    uint64_t proc_transitions = 0;
    uint64_t thread_transitions = 0;
    uint64_t proc_terms = 0;
    uint64_t thread_exits = 0;
    uint32_t abi_version = 0;
    vibeos_handle_table_t *pid1_handles = 0;
    memset(&kernel, 0, sizeof(kernel));
    memset(&frame, 0, sizeof(frame));
    vibeos_event_init(&kernel.boot_event);
    /* ABI first: everything after this assumes the version the kernel
     * reports is the one this test was written against. */
    vibeos_syscall_make_abi_version_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    abi_version = (uint32_t)frame.result;
    if (abi_version != vibeos_syscall_abi_version_current()) {
        return -1;
    }
    vibeos_syscall_make_abi_compat_check(&frame, abi_version);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_abi_compat_check(&frame, VIBEOS_SYSCALL_ABI_VERSION_PACK(VIBEOS_SYSCALL_ABI_VERSION_MAJOR + 1u, 0));
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }

    /* Handles: allocate one with signal rights, then check that signalling
     * through handle 0 is refused while the real handle works. */
    vibeos_syscall_make_handle_alloc(&frame, VIBEOS_HANDLE_RIGHT_SIGNAL | VIBEOS_HANDLE_RIGHT_MANAGE, 0);
    if (vibeos_handle_table_init(&kernel.handles) != 0) {
        return -1;
    }
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    signal_handle = (uint32_t)frame.result;
    vibeos_syscall_make_event_signal(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_event_signal(&frame, signal_handle);
    frame.result = -1;
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (!vibeos_event_is_signaled(&kernel.boot_event)) {
        return -1;
    }
    if (vibeos_policy_init(&kernel.policy) != 0) {
        return -1;
    }
    if (vibeos_sec_token_init(&kernel.kernel_token, 0, 0, (1u << 0) | (1u << 1) | (1u << 2)) != 0) {
        return -1;
    }
    /* Processes: spawn, state transitions and the counters over them. */
    vibeos_syscall_make_process_spawn(&frame, 0);
    if (vibeos_proc_init(&kernel.proc_table) != 0) {
        return -1;
    }
    if (vibeos_sched_init(&kernel.scheduler, 2) != 0) {
        return -1;
    }
    if (vibeos_sched_enqueue(&kernel.scheduler, &sthread1) != 0 || vibeos_sched_enqueue(&kernel.scheduler, &sthread2) != 0) {
        return -1;
    }
    if (vibeos_sched_tick(&kernel.scheduler, &sthread1, 0) != 1) {
        return -1;
    }
    if (vibeos_sched_note_wait_timeout(&kernel.scheduler, 0) != 0) {
        return -1;
    }
    if (vibeos_sched_note_wait_wake(&kernel.scheduler, 1) != 0) {
        return -1;
    }
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    pid1 = (uint32_t)frame.result;
    vibeos_syscall_make_process_spawn(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 2) {
        return -1;
    }
    pid2 = (uint32_t)frame.result;
    vibeos_syscall_make_process_spawn(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 3) {
        return -1;
    }
    pid3 = (uint32_t)frame.result;
    vibeos_syscall_make_process_token_get(&frame, pid1, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.arg0 != 0 || frame.arg1 != 0 || frame.arg2 == 0) {
        return -1;
    }
    vibeos_syscall_make_process_token_get(&frame, pid3, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_policy_capability_get(&frame, VIBEOS_POLICY_TARGET_PROCESS_SPAWN, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 2) {
        return -1;
    }
    vibeos_syscall_make_policy_capability_set(&frame, VIBEOS_POLICY_TARGET_PROCESS_SPAWN, 7, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_policy_summary_get(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.arg2 != 7 || frame.result != 3) {
        return -1;
    }
    vibeos_syscall_make_process_security_label_set(&frame, pid1, 10, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_process_interact_check(&frame, pid2, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_process_security_label_set(&frame, pid2, 10, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_process_security_label_get(&frame, pid2, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 10) {
        return -1;
    }
    vibeos_syscall_make_process_interact_check(&frame, pid2, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_process_token_set(&frame, pid1, (1u << 7), 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_process_token_set(&frame, pid2, 0, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_process_spawn_as(&frame, pid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 4) {
        return -1;
    }
    vibeos_syscall_make_process_spawn_as(&frame, pid2, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_policy_capability_set(&frame, VIBEOS_POLICY_TARGET_PROCESS_SPAWN, 2, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_policy_capability_set(&frame, VIBEOS_POLICY_TARGET_PROCESS_SPAWN, 2, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    /* Security: tokens and the capability checks that gate the rest. */
    vibeos_syscall_make_sec_audit_count(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result < 6) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_count_action(&frame, VIBEOS_SEC_AUDIT_PROCESS_SPAWN, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result < 2) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_count_success(&frame, 1, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result < 3) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_count_success(&frame, 0, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result < 1) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_summary(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.arg0 < frame.arg1 || frame.arg0 < frame.arg2) {
        return -1;
    }
    if ((frame.arg1 + frame.arg2) != frame.arg0) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_get(&frame, 0, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    if (frame.arg0 == 0) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_count(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result < 1) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_reset(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_reset(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_sec_audit_count(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    /* Threads: creation, state changes and per-process aggregation. */
    vibeos_syscall_make_thread_create(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    tid1 = (uint32_t)frame.result;
    vibeos_syscall_make_process_state_get(&frame, pid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != VIBEOS_PROCESS_STATE_RUNNING) {
        return -1;
    }
    vibeos_syscall_make_process_state_get(&frame, pid2, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_process_state_get(&frame, pid1, pid3);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_process_state_set(&frame, pid1, VIBEOS_PROCESS_STATE_BLOCKED, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_process_state_get(&frame, pid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != VIBEOS_PROCESS_STATE_BLOCKED) {
        return -1;
    }
    vibeos_syscall_make_process_state_set(&frame, pid1, VIBEOS_PROCESS_STATE_RUNNING, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_process_terminate(&frame, pid3, pid3);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_process_state_get(&frame, pid3, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != VIBEOS_PROCESS_STATE_TERMINATED) {
        return -1;
    }
    vibeos_syscall_make_process_count_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 4) {
        return -1;
    }
    vibeos_syscall_make_process_live_count_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 3) {
        return -1;
    }
    vibeos_syscall_make_process_terminated_count_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_process_state_count_get(&frame, VIBEOS_PROCESS_STATE_BLOCKED);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_process_state_summary_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 2 || frame.arg1 != 0 || frame.arg2 != 1 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_thread_state_get(&frame, tid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != VIBEOS_THREAD_STATE_RUNNABLE) {
        return -1;
    }
    vibeos_syscall_make_thread_state_get(&frame, tid1, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_thread_state_set(&frame, tid1, VIBEOS_THREAD_STATE_BLOCKED, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_thread_state_get(&frame, tid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != VIBEOS_THREAD_STATE_BLOCKED) {
        return -1;
    }
    vibeos_syscall_make_thread_state_count_get(&frame, VIBEOS_THREAD_STATE_BLOCKED);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_thread_state_summary_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 0 || frame.arg1 != 0 || frame.arg2 != 1 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_thread_state_set(&frame, tid1, 99, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_thread_exit(&frame, tid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_thread_state_get(&frame, tid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_thread_count_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_thread_state_count_get(&frame, VIBEOS_THREAD_STATE_EXITED);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_handle_alloc(&frame, VIBEOS_HANDLE_RIGHT_SIGNAL | VIBEOS_HANDLE_RIGHT_MANAGE, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    p1_handle = (uint32_t)frame.result;
    vibeos_syscall_make_handle_close(&frame, p1_handle, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_handle_close(&frame, p1_handle, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    /* Virtual memory: mapping and protection through the syscall path. */
    vibeos_syscall_make_vm_map(&frame, 0x800000, 0x300000, 0x1000);
    if (vibeos_vm_init(&kernel.kernel_aspace) != 0) {
        return -1;
    }
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_vm_protect(&frame, 0x800000, 0x1000, VIBEOS_VM_PERM_READ);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_vm_unmap(&frame, 0x800000, 0x1000);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_handle_alloc(&frame, VIBEOS_HANDLE_RIGHT_SIGNAL, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    waitset_event_handle = (uint32_t)frame.result;
    /* Waitsets: membership and the statistics that back the wake policies. */
    vibeos_syscall_make_waitset_add_event(&frame, waitset_event_handle, pid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_add_event(&frame, waitset_event_handle, 200, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_stats_get(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 1 || frame.arg1 != 0 || frame.arg2 != 0 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_stats_ext_get(&frame, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_stats_ext_get(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 0 || frame.arg1 != 1 || frame.arg2 != pid1 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_waitset_owner_get(&frame, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_owner_get(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.result != pid1 || frame.arg0 != 1 || frame.arg1 != 1) {
        return -1;
    }
    vibeos_syscall_make_waitset_snapshot_get(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.result != pid1 || frame.arg0 != 1 || frame.arg1 != VIBEOS_WAITSET_WAKE_FIFO || frame.arg2 != 1) {
        return -1;
    }
    vibeos_syscall_make_waitset_wake_policy_get(&frame, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_wake_policy_get(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != VIBEOS_WAITSET_WAKE_FIFO) {
        return -1;
    }
    vibeos_syscall_make_waitset_wake_policy_set(&frame, VIBEOS_WAITSET_WAKE_REVERSE, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_wake_policy_get(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != VIBEOS_WAITSET_WAKE_REVERSE) {
        return -1;
    }
    vibeos_syscall_make_waitset_stats_reset(&frame, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_stats_reset(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_stats_get(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 0 || frame.arg1 != 0 || frame.arg2 != 0 || frame.result != 0) {
        return -1;
    }
    if (vibeos_proc_handles(&kernel.proc_table, pid1, &pid1_handles) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(pid1_handles, VIBEOS_HANDLE_RIGHT_SIGNAL | VIBEOS_HANDLE_RIGHT_MANAGE, &revoke_root) != 0) {
        return -1;
    }
    if (vibeos_proc_duplicate_handle(&kernel.proc_table, pid1, pid2, revoke_root, VIBEOS_HANDLE_RIGHT_SIGNAL, &revoke_dup) != 0) {
        return -1;
    }
    if (revoke_dup == 0) {
        return -1;
    }
    if (vibeos_proc_revoke_handle_lineage(&kernel.proc_table, pid1, revoke_root) != 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_count(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result < 1) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_get(&frame, (uint32_t)(frame.result - 1), 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    if (vibeos_syscall_audit_event_action(&frame) != VIBEOS_PROC_AUDIT_REVOKE_LINEAGE) {
        return -1;
    }
    if (vibeos_syscall_audit_event_success(&frame) != 1 || vibeos_syscall_audit_event_revoked_count(&frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_count(&frame, pid2);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_count(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result < 1) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_get(&frame, 0, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_policy_set(&frame, VIBEOS_PROC_AUDIT_DROP_NEWEST, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_policy_set(&frame, VIBEOS_PROC_AUDIT_DROP_NEWEST, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_policy_get(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != VIBEOS_PROC_AUDIT_DROP_NEWEST) {
        return -1;
    }
    for (i = 0; i < VIBEOS_PROC_AUDIT_CAPACITY + 8; i++) {
        (void)vibeos_proc_revoke_handle_lineage(&kernel.proc_table, pid1, revoke_root);
    }
    vibeos_syscall_make_proc_audit_dropped(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_dropped(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_count_action(&frame, VIBEOS_PROC_AUDIT_REVOKE_LINEAGE, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_count_success(&frame, 1, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_summary(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.arg0 < frame.arg1 || frame.arg0 < frame.arg2) {
        return -1;
    }
    if ((frame.arg1 + frame.arg2) != frame.arg0) {
        return -1;
    }
    vibeos_syscall_make_proc_audit_summary(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.arg0 < 1) {
        return -1;
    }
    /* Scheduler: admission and the queue accounting it exposes. */
    vibeos_syscall_make_sched_cpu_count_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 2) {
        return -1;
    }
    vibeos_syscall_make_sched_runnable_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 2) {
        return -1;
    }
    vibeos_syscall_make_sched_runqueue_depth_get(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_runqueue_depth_get(&frame, 1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_runqueue_depth_get(&frame, 9);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_sched_preemptions_get(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_wait_timeouts_get(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_wait_wakes_get(&frame, 1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_preemptions_total_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_wait_timeouts_total_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_wait_wakes_total_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_counter_summary_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 1 || frame.arg1 != 1 || frame.arg2 != 1 || frame.result != 2) {
        return -1;
    }
    vibeos_syscall_make_sched_counters_reset(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_sched_counters_reset(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_counter_summary_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 0 || frame.arg1 != 0 || frame.arg2 != 0 || frame.result != 2) {
        return -1;
    }
    if (vibeos_sched_wait_begin(&kernel.scheduler, sthread1.id, 0) != 0) {
        return -1;
    }
    if (vibeos_sched_wait_end(&kernel.scheduler, sthread1.id, 1, 0) != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_tracked_threads_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 2) {
        return -1;
    }
    vibeos_syscall_make_sched_blocked_threads_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_wait_transition_summary_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 1 || frame.arg1 != 1 || frame.arg2 != 1 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_thread_runtime_get(&frame, sthread1.id);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != VIBEOS_SCHED_THREAD_RUNNABLE || frame.arg1 != 1 || frame.arg2 != 1 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_sched_thread_affinity_set(&frame, sthread2.id, (1u << 1), 0, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_thread_affinity_get(&frame, sthread2.id);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.arg0 != (1u << 1) || frame.arg1 != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_thread_nice_set(&frame, sthread2.id, -5, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_thread_nice_get(&frame, sthread2.id);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != -5) {
        return -1;
    }
    vibeos_syscall_make_sched_starvation_tick(&frame, 1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_starvation_tick(&frame, 1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_boost_starving(&frame, 2, 2, 8, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    vibeos_syscall_make_sched_rebalance(&frame, 4, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_sched_qos_summary_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 == 0 || frame.result == 0) {
        return -1;
    }
    vibeos_syscall_make_process_thread_state_count_get(&frame, pid1, VIBEOS_THREAD_STATE_BLOCKED, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_process_runnable_threads_get(&frame, pid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    vibeos_syscall_make_process_blocked_threads_get(&frame, pid1, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 0) {
        return -1;
    }
    if (vibeos_proc_transition_counters(&kernel.proc_table, &proc_transitions, &thread_transitions, &proc_terms, &thread_exits) != 0) {
        return -1;
    }
    if (proc_transitions != 3 || thread_transitions != 3 || proc_terms != 1 || thread_exits != 1) {
        return -1;
    }
    vibeos_syscall_make_proc_transition_counters_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 3 || frame.arg1 != 3 || frame.arg2 != 1 || frame.result != 1) {
        return -1;
    }
    vibeos_syscall_make_proc_transition_counters_reset(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
        return -1;
    }
    vibeos_syscall_make_proc_transition_counters_reset(&frame, 0);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_proc_transition_counters_get(&frame);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (frame.arg0 != 0 || frame.arg1 != 0 || frame.arg2 != 0 || frame.result != 0) {
        return -1;
    }
    return 0;
}

static int test_process_relationships(void) {
    vibeos_process_table_t pt;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    uint32_t p3 = 0;
    uint32_t count = 0;
    uint64_t proc_transitions = 0;
    uint64_t thread_transitions = 0;
    uint64_t proc_terms = 0;
    uint64_t thread_exits = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p1) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, p1, &p2) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p3) != 0) {
        return -1;
    }
    if (!vibeos_proc_are_related(&pt, p1, p2)) {
        return -1;
    }
    if (vibeos_proc_are_related(&pt, p2, p3)) {
        return -1;
    }
    if (vibeos_proc_live_count(&pt, &count) != 0 || count != 3) {
        return -1;
    }
    if (vibeos_proc_terminate(&pt, p2) != 0) {
        return -1;
    }
    if (vibeos_proc_live_count(&pt, &count) != 0 || count != 2) {
        return -1;
    }
    if (vibeos_proc_terminated_count(&pt, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_proc_count_in_state(&pt, VIBEOS_PROCESS_STATE_TERMINATED, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_proc_count_in_state(&pt, VIBEOS_PROCESS_STATE_NEW, &count) != 0 || count != 2) {
        return -1;
    }
    if (vibeos_proc_state_summary(&pt, &p1, &p2, &p3, &count) != 0) {
        return -1;
    }
    if (p1 != 2 || p2 != 0 || p3 != 0 || count != 1) {
        return -1;
    }
    if (vibeos_proc_transition_counters(&pt, &proc_transitions, &thread_transitions, &proc_terms, &thread_exits) != 0) {
        return -1;
    }
    if (proc_transitions != 1 || thread_transitions != 0 || proc_terms != 1 || thread_exits != 0) {
        return -1;
    }
    return 0;
}

static int test_services(void) {
    vibeos_init_state_t init_state;
    vibeos_devmgr_state_t devmgr_state;
    vibeos_vfs_state_t vfs_state;
    vibeos_net_state_t net_state;
    vibeos_init_node_t nodes[4];
    uint32_t started = 0;
    uint32_t failed = 0;
    uint32_t restart_allowed = 0;

    if (vibeos_init_start(&init_state) != 0) {
        return -1;
    }
    if (vibeos_devmgr_start(&devmgr_state) != 0) {
        return -1;
    }
    if (vibeos_vfs_start(&vfs_state) != 0) {
        return -1;
    }
    if (vibeos_net_start(&net_state) != 0) {
        return -1;
    }

    if (init_state.state != VIBEOS_SERVICE_RUNNING) {
        return -1;
    }
    if (devmgr_state.state != VIBEOS_SERVICE_RUNNING) {
        return -1;
    }
    if (vfs_state.state != VIBEOS_SERVICE_RUNNING) {
        return -1;
    }
    if (net_state.state != VIBEOS_SERVICE_RUNNING) {
        return -1;
    }
    nodes[0].service_id = 1;
    nodes[0].dependency_mask = 0;
    nodes[0].enabled = 1;
    nodes[0].restart_class = VIBEOS_INIT_SERVICE_CORE;
    nodes[1].service_id = 2;
    nodes[1].dependency_mask = 1u << 0;
    nodes[1].enabled = 1;
    nodes[1].restart_class = VIBEOS_INIT_SERVICE_CORE;
    nodes[2].service_id = 3;
    nodes[2].dependency_mask = 1u << 1;
    nodes[2].enabled = 1;
    nodes[2].restart_class = VIBEOS_INIT_SERVICE_OPTIONAL;
    nodes[3].service_id = 4;
    nodes[3].dependency_mask = 1u << 2;
    nodes[3].enabled = 1;
    nodes[3].restart_class = VIBEOS_INIT_SERVICE_OPTIONAL;
    if (vibeos_init_graph_start(&init_state, nodes, 4, &started, &failed) != 0 || started != 4 || failed != 0) {
        return -1;
    }
    nodes[3].enabled = 0;
    nodes[3].dependency_mask = 1u << 2;
    started = 0;
    failed = 0;
    if (vibeos_init_graph_start(&init_state, nodes, 4, &started, &failed) != 0 ||
        started != 3 || failed != 0) {
        return -1;
    }
    if (vibeos_init_restart_policy(&init_state, 2, 1) != 0) {
        return -1;
    }
    if (vibeos_init_restart_note(&init_state, VIBEOS_INIT_SERVICE_CORE) != 0) {
        return -1;
    }
    if (vibeos_init_restart_note(&init_state, VIBEOS_INIT_SERVICE_OPTIONAL) != 0) {
        return -1;
    }
    if (vibeos_init_restart_allowed(&init_state, VIBEOS_INIT_SERVICE_OPTIONAL, &restart_allowed) != 0 || restart_allowed != 0) {
        return -1;
    }
    if (vibeos_net_stop(&net_state) != 0 || net_state.state != VIBEOS_SERVICE_STOPPED) {
        return -1;
    }
    if (vibeos_vfs_stop(&vfs_state) != 0 || vfs_state.state != VIBEOS_SERVICE_STOPPED) {
        return -1;
    }
    if (vibeos_devmgr_stop(&devmgr_state) != 0 || devmgr_state.state != VIBEOS_SERVICE_STOPPED) {
        return -1;
    }
    if (vibeos_init_stop(&init_state) != 0 || init_state.state != VIBEOS_SERVICE_STOPPED) {
        return -1;
    }
    return 0;
}

static int test_servicemgr_and_drivers(void) {
    vibeos_servicemgr_state_t mgr;
    vibeos_init_state_t init_state;
    vibeos_devmgr_state_t devmgr_state;
    vibeos_vfs_state_t vfs_state;
    vibeos_net_state_t net_state;
    vibeos_driver_framework_t fw;
    vibeos_driver_state_t state;
    uint32_t running = 0;
    uint32_t can_restart = 0;
    uint32_t count_loaded = 0;
    uint32_t count_faulted = 0;
    if (vibeos_servicemgr_start(&mgr, &init_state, &devmgr_state, &vfs_state, &net_state) != 0) {
        return -1;
    }
    if (init_state.started_services != 4) {
        return -1;
    }
    if (mgr.supervised_count != 4 || mgr.state != VIBEOS_SERVICE_RUNNING) {
        return -1;
    }
    if (vibeos_servicemgr_set_restart_budget(&mgr, 2) != 0) {
        return -1;
    }
    if (vibeos_servicemgr_can_restart(&mgr, &can_restart) != 0 || can_restart != 1) {
        return -1;
    }
    if (vibeos_servicemgr_report_service_failure(&mgr) != 0) {
        return -1;
    }
    if (vibeos_servicemgr_report_service_failure(&mgr) != 0) {
        return -1;
    }
    if (vibeos_servicemgr_can_restart(&mgr, &can_restart) != 0 || can_restart != 0) {
        return -1;
    }
    if (vibeos_servicemgr_report_service_failure(&mgr) == 0) {
        return -1;
    }
    if (vibeos_driver_framework_init(&fw) != 0) {
        return -1;
    }
    if (vibeos_driver_framework_require_abi(&fw, 1) != 0) {
        return -1;
    }
    if (vibeos_driver_register(&fw, 100) != 0 || fw.count != 1) {
        return -1;
    }
    if (vibeos_driver_register_versioned(&fw, 200, 2, 0, 0x10) == 0) {
        return -1;
    }
    if (vibeos_driver_register_versioned(&fw, 101, 1, 2, 0x10) != 0 || fw.count != 2) {
        return -1;
    }
    if (vibeos_driver_register(&fw, 100) == 0) {
        return -1;
    }
    if (vibeos_driver_state(&fw, 100, &state) != 0 || state != VIBEOS_DRIVER_LOADED) {
        return -1;
    }
    if (vibeos_driver_mark_faulted(&fw, 101) != 0) {
        return -1;
    }
    if (vibeos_driver_state(&fw, 101, &state) != 0 || state != VIBEOS_DRIVER_FAULTED) {
        return -1;
    }
    if (vibeos_driver_count_state(&fw, VIBEOS_DRIVER_LOADED, &count_loaded) != 0 || count_loaded != 1) {
        return -1;
    }
    if (vibeos_driver_count_state(&fw, VIBEOS_DRIVER_FAULTED, &count_faulted) != 0 || count_faulted != 1) {
        return -1;
    }
    if (vibeos_servicemgr_health(&mgr, &init_state, &devmgr_state, &vfs_state, &net_state, &running) != 0 || running != 5) {
        return -1;
    }
    if (vibeos_driver_unregister(&fw, 100) != 0 || vibeos_driver_unregister(&fw, 101) != 0 || fw.count != 0) {
        return -1;
    }
    if (vibeos_servicemgr_stop(&mgr, &init_state, &devmgr_state, &vfs_state, &net_state) != 0) {
        return -1;
    }
    if (vibeos_servicemgr_health(&mgr, &init_state, &devmgr_state, &vfs_state, &net_state, &running) != 0 || running != 0) {
        return -1;
    }
    return 0;
}

/* Covers the user-facing API surface together with the bootloader contract:
 * boot_info construction, validation of malformed maps, memory summaries and
 * the region-type queries the kernel relies on at start-up.
 *
 * The test is intentionally end-to-end: it verifies that user-visible API
 * metadata and capability checks remain consistent with boot-time memory-map
 * handling assumptions used during kernel initialization. */
static int test_user_api_and_bootloader(void) {
    vibeos_kernel_t kernel;
    vibeos_user_context_t user_ctx;
    vibeos_user_api_caps_t caps;
    uint32_t api_major = 0;
    uint32_t api_minor = 0;
    uint32_t signal_handle = 0;
    uint32_t out_label = 0;
    uint32_t interact_allowed = 0;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    vibeos_memory_region_t regions[2];
    vibeos_boot_info_t boot_info;
    uint64_t total = 0;
    uint64_t usable = 0;
    uint64_t usable_regions = 0;
    uint32_t has_overlap = 0;

    /* Build a minimal but representative memory map:
     * - one usable RAM region
     * - one MMIO region
     * This allows us to validate usable-only accounting and type filtering. */
    memset(&kernel, 0, sizeof(kernel));
    regions[0].base = 0x200000;
    regions[0].length = 0x100000;
    regions[0].type = 1;
    regions[0].reserved = 0;
    regions[1].base = 0x500000;
    regions[1].length = 0x080000;
    regions[1].type = 2;
    regions[1].reserved = 0;

    if (vibeos_user_context_init(&user_ctx, 10, 1) != 0) {
        return -1;
    }
    if (vibeos_user_api_contract(&api_major, &api_minor) != 0 || api_major != 1 || api_minor == 0) {
        return -1;
    }
    if (vibeos_user_api_capabilities(&caps) != 0) {
        return -1;
    }
    if (caps.supports_boot_event_signal == 0 || caps.supports_process_security_label == 0 || caps.supports_process_interaction_check == 0 || caps.supports_policy_summary == 0) {
        return -1;
    }
    if (user_ctx.pid != 10 || user_ctx.tid != 1) {
        return -1;
    }
    if (vibeos_bootloader_build_boot_info(&boot_info, regions, 2) != 0) {
        return -1;
    }
    if (boot_info.version != VIBEOS_BOOTINFO_VERSION || boot_info.memory_map_entries != 2) {
        return -1;
    }
    if (vibeos_bootloader_validate_boot_info(&boot_info) != 0) {
        return -1;
    }
    if (vibeos_bootloader_memory_summary(&boot_info, &total, &usable) != 0) {
        return -1;
    }
    if (total != (regions[0].length + regions[1].length) || usable != regions[0].length) {
        return -1;
    }
    if (vibeos_bootloader_count_region_type(&boot_info, 1, &usable_regions) != 0 || usable_regions != 1) {
        return -1;
    }
    if (vibeos_bootloader_has_overlap(&boot_info, &has_overlap) != 0 || has_overlap != 0) {
        return -1;
    }
    if (vibeos_bootloader_max_physical_address(&boot_info, &total) != 0 || total != (regions[1].base + regions[1].length)) {
        return -1;
    }
    if (vibeos_handle_table_init(&kernel.handles) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(&kernel.handles, VIBEOS_HANDLE_RIGHT_SIGNAL, &signal_handle) != 0) {
        return -1;
    }
    if (vibeos_user_signal_boot_event(&kernel, signal_handle) != 0) {
        return -1;
    }
    if (!vibeos_event_is_signaled(&kernel.boot_event)) {
        return -1;
    }
    if (vibeos_proc_init(&kernel.proc_table) != 0 || vibeos_policy_init(&kernel.policy) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&kernel.proc_table, 0, &p1) != 0 || vibeos_proc_spawn(&kernel.proc_table, p1, &p2) != 0) {
        return -1;
    }
    if (vibeos_user_set_process_security_label(&kernel, 0, p1, 5) != 0) {
        return -1;
    }
    if (vibeos_user_get_process_security_label(&kernel, p1, p1, &out_label) != 0 || out_label != 5) {
        return -1;
    }
    if (vibeos_user_check_process_interaction(&kernel, p1, p2, &interact_allowed) != 0 || interact_allowed != 0) {
        return -1;
    }
    if (vibeos_user_set_process_security_label(&kernel, 0, p2, 5) != 0) {
        return -1;
    }
    if (vibeos_user_check_process_interaction(&kernel, p1, p2, &interact_allowed) != 0 || interact_allowed != 1) {
        return -1;
    }
    return 0;
}

static int test_native_userland_abi(void) {
    vibeos_process_start_info_t start = {0};
    vibeos_service_manifest_t manifest[2] = {0};
    vibeos_service_runtime_snapshot_t snapshot = {0};

    start.abi_major = VIBEOS_NATIVE_ABI_MAJOR;
    start.abi_minor = VIBEOS_NATIVE_ABI_MINOR;
    start.struct_size = sizeof(start);
    start.argc = 1;
    start.argv_ptr = 0x1000;
    strcpy(start.image_path, "/sbin/init");
    if (vibeos_process_start_info_validate(&start) != 0) {
        return -1;
    }
    start.abi_major++;
    if (vibeos_process_start_info_validate(&start) == 0) {
        return -1;
    }
    start.abi_major = VIBEOS_NATIVE_ABI_MAJOR;
    start.argv_ptr = 0;
    if (vibeos_process_start_info_validate(&start) == 0) {
        return -1;
    }

    manifest[0].abi_major = VIBEOS_NATIVE_ABI_MAJOR;
    manifest[0].struct_size = sizeof(manifest[0]);
    manifest[0].service_id = 1;
    manifest[0].restart_policy = VIBEOS_NATIVE_RESTART_ON_FAILURE;
    strcpy(manifest[0].name, "init");
    strcpy(manifest[0].image_path, "/sbin/init");
    manifest[1] = manifest[0];
    manifest[1].service_id = 2;
    manifest[1].dependency_mask = 1u;
    strcpy(manifest[1].name, "shell");
    strcpy(manifest[1].image_path, "/bin/sh");
    if (vibeos_service_manifest_validate(manifest, 2, 0) != 0 ||
        vibeos_service_manifest_validate(manifest, 2, 1) != 0) {
        return -1;
    }
    manifest[1].dependency_mask = 2u;
    if (vibeos_service_manifest_validate(manifest, 2, 1) == 0) {
        return -1;
    }

    snapshot.abi_major = VIBEOS_NATIVE_ABI_MAJOR;
    snapshot.struct_size = sizeof(snapshot);
    snapshot.service_id = 2;
    snapshot.state = VIBEOS_NATIVE_SERVICE_RUNNING;
    snapshot.last_exit_reason = VIBEOS_PROCESS_EXIT_NORMAL;
    if (vibeos_service_snapshot_validate(&snapshot) != 0) {
        return -1;
    }
    snapshot.state = 99;
    return vibeos_service_snapshot_validate(&snapshot) == 0 ? -1 : 0;
}

static int test_native_service_supervisor(void) {
    vibeos_service_supervisor_t supervisor;
    vibeos_service_manifest_t manifests[3] = {0};
    uint32_t running = 0, failed = 0;
    uint32_t i;
    for (i = 0; i < 3; i++) {
        manifests[i].abi_major = VIBEOS_NATIVE_ABI_MAJOR;
        manifests[i].struct_size = sizeof(manifests[i]);
        manifests[i].service_id = i + 1;
        manifests[i].restart_policy = VIBEOS_NATIVE_RESTART_ON_FAILURE;
        manifests[i].restart_limit = 2;
        strcpy(manifests[i].name, i == 0 ? "init" : (i == 1 ? "logd" : "shell"));
        strcpy(manifests[i].image_path, i == 0 ? "/sbin/init" : "/bin/service");
    }
    manifests[1].dependency_mask = 1u;
    manifests[2].dependency_mask = 3u;
    if (vibeos_service_supervisor_init(&supervisor) != 0 ||
        vibeos_service_supervisor_load(&supervisor, manifests, 3) != 0 ||
        vibeos_service_supervisor_start_ready(&supervisor) != 0 ||
        vibeos_service_supervisor_health(&supervisor, &running, &failed) != 0 ||
        running != 3 || failed != 0) {
        return -1;
    }
    if (vibeos_service_supervisor_bind_pid(&supervisor, 1, 100) != 0 ||
        vibeos_service_supervisor_bind_pid(&supervisor, 2, 100) == 0 ||
        vibeos_service_supervisor_service_for_pid(&supervisor, 100, &i) != 0 || i != 1 ||
        vibeos_service_supervisor_unbind_pid(&supervisor, 100) != 0 ||
        vibeos_service_supervisor_service_for_pid(&supervisor, 100, &i) == 0) {
        return -1;
    }
    if (vibeos_service_supervisor_bind_pid(&supervisor, 2, 200) != 0 ||
        vibeos_service_supervisor_report_exit_pid(&supervisor, 200, 7, VIBEOS_PROCESS_EXIT_FAULT) != 0 ||
        supervisor.runtime[1].state != VIBEOS_NATIVE_SERVICE_STARTING ||
        vibeos_service_supervisor_service_for_pid(&supervisor, 200, &i) == 0) {
        return -1;
    }
    manifests[2].service_id = manifests[1].service_id;
    if (vibeos_service_supervisor_load(&supervisor, manifests, 3) == 0) {
        return -1;
    }
    manifests[2].service_id = 3;
    if (vibeos_service_supervisor_report_exit(&supervisor, 2, 9, VIBEOS_PROCESS_EXIT_FAULT) != 0 ||
        vibeos_service_supervisor_tick(&supervisor, 16) != 0 ||
        supervisor.runtime[1].state != VIBEOS_NATIVE_SERVICE_RUNNING ||
        supervisor.runtime[1].restart_count != 2) {
        return -1;
    }
    if (vibeos_service_supervisor_report_exit(&supervisor, 2, 9, VIBEOS_PROCESS_EXIT_NORMAL) != 0 ||
        supervisor.runtime[1].state != VIBEOS_NATIVE_SERVICE_STOPPED) {
        return -1;
    }
    if (vibeos_service_supervisor_health(&supervisor, &running, &failed) != 0 || failed != 0) {
        return -1;
    }
    if (vibeos_service_supervisor_report_exit(&supervisor, 2, 11, VIBEOS_PROCESS_EXIT_FAULT) != 0 ||
        vibeos_service_supervisor_tick(&supervisor, 16) != 0 ||
        vibeos_service_supervisor_report_exit(&supervisor, 2, 12, VIBEOS_PROCESS_EXIT_FAULT) != 0 ||
        supervisor.runtime[1].state != VIBEOS_NATIVE_SERVICE_FAILED) {
        return -1;
    }
    manifests[1].service_id = 2;
    manifests[1].dependency_mask = 4u;
    manifests[2].dependency_mask = 2u;
    {
        int load_result = vibeos_service_supervisor_load(&supervisor, manifests, 3);
        int start_result = vibeos_service_supervisor_start_ready(&supervisor);
        if (load_result != 0 || start_result == 0) {
            return -1;
        }
    }
    return vibeos_service_supervisor_health(&supervisor, &running, &failed) == 0 &&
           running == 1 && failed == 0 ? 0 : -1;
}

static int native_supervisor_spawn(const vibeos_service_manifest_t *manifest, void *context, uint32_t *out_pid) {
    uint32_t *next_pid = (uint32_t *)context;
    if (!manifest || !out_pid || !next_pid || manifest->service_id == 0) {
        return -1;
    }
    *out_pid = (*next_pid)++;
    return 0;
}

static int test_native_service_spawn_hook(void) {
    vibeos_service_supervisor_t supervisor;
    vibeos_service_manifest_t manifest = {0};
    uint32_t next_pid = 401;
    if (vibeos_service_supervisor_init(&supervisor) != 0) {
        return -1;
    }
    manifest.abi_major = VIBEOS_NATIVE_ABI_MAJOR;
    manifest.struct_size = sizeof(manifest);
    manifest.service_id = 1;
    manifest.restart_policy = VIBEOS_NATIVE_RESTART_ON_FAILURE;
    manifest.restart_limit = 1;
    strcpy(manifest.name, "init");
    strcpy(manifest.image_path, "/sbin/init");
    if (vibeos_service_supervisor_load(&supervisor, &manifest, 1) != 0 ||
        vibeos_service_supervisor_set_hooks(&supervisor, native_supervisor_spawn, 0, &next_pid) != 0 ||
        vibeos_service_supervisor_start_ready(&supervisor) != 0 ||
        vibeos_service_supervisor_bind_pid(&supervisor, 1, 400) != 0) {
        return -1;
    }
    /* Initial activation is already running; after a fault, the hook owns the
     * transition and supplies the replacement PID. */
    if (vibeos_service_supervisor_report_exit_pid(&supervisor, 400, 1, VIBEOS_PROCESS_EXIT_FAULT) != 0 ||
        vibeos_service_supervisor_tick(&supervisor, 16) != 0 ||
        supervisor.runtime[0].pid != 401 ||
        supervisor.runtime[0].state != VIBEOS_NATIVE_SERVICE_RUNNING) {
        return -1;
    }
    return 0;
}

static int test_process_groups_and_sessions(void) {
    vibeos_process_table_t table;
    uint32_t leader, child, session;
    if (vibeos_proc_init(&table) != 0 ||
        vibeos_proc_spawn(&table, 0, &leader) != 0 ||
        vibeos_proc_spawn(&table, leader, &child) != 0 ||
        vibeos_proc_get_session(&table, child, &session) != 0 || session != leader ||
        vibeos_proc_set_process_group(&table, child, leader) != 0) {
        return -1;
    }
    if (vibeos_proc_set_process_group(&table, child, 9999) == 0 ||
        vibeos_proc_create_session(&table, child, &session) == 0 ||
        vibeos_proc_create_session(&table, leader, &session) != 0 || session != leader) {
        return -1;
    }
    return 0;
}

static int test_process_orphan_adoption(void) {
    vibeos_process_table_t table;
    uint32_t init_pid, parent_pid, child_pid;
    if (vibeos_proc_init(&table) != 0 ||
        vibeos_proc_spawn(&table, 0, &init_pid) != 0 || init_pid != 1 ||
        vibeos_proc_spawn(&table, init_pid, &parent_pid) != 0 ||
        vibeos_proc_spawn(&table, parent_pid, &child_pid) != 0 ||
        vibeos_proc_terminate(&table, parent_pid) != 0) {
        return -1;
    }
    if (table.entries[2].pid != child_pid || table.entries[2].parent_pid != init_pid) {
        return -1;
    }
    if (vibeos_proc_terminate(&table, init_pid) != 0 || table.entries[2].parent_pid != 0) {
        return -1;
    }
    return 0;
}

static int test_bootloader_sanitized_map(void) {
    vibeos_memory_region_t input[6];
    vibeos_memory_region_t scratch[6];
    vibeos_boot_info_t boot_info;
    uint64_t sanitized_count = 0;
    uint64_t count_usable = 0;
    uint32_t has_overlap = 0;

    memset(&boot_info, 0, sizeof(boot_info));
    memset(input, 0, sizeof(input));
    memset(scratch, 0, sizeof(scratch));

    input[0].base = 0x400000;
    input[0].length = 0x1000;
    input[0].type = VIBEOS_MEMORY_REGION_RESERVED;

    input[1].base = 0x100000;
    input[1].length = 0x2000;
    input[1].type = VIBEOS_MEMORY_REGION_USABLE;

    input[2].base = 0x102000;
    input[2].length = 0x1000;
    input[2].type = VIBEOS_MEMORY_REGION_USABLE;

    input[3].base = 0x800000;
    input[3].length = 0x1000;
    input[3].type = 99u;

    input[4].base = 0x500000;
    input[4].length = 0x3000;
    input[4].type = VIBEOS_MEMORY_REGION_MMIO;

    input[5].base = 0x200000;
    input[5].length = 0x1000;
    input[5].type = VIBEOS_MEMORY_REGION_ACPI_RECLAIMABLE;

    if (vibeos_bootloader_build_boot_info_sanitized(&boot_info, input, 6, scratch, 6, &sanitized_count) != 0) {
        return -1;
    }
    if (sanitized_count != 4 || boot_info.memory_map_entries != 4) {
        return -1;
    }
    if (boot_info.memory_map[0].base != 0x100000 || boot_info.memory_map[0].length != 0x3000 || boot_info.memory_map[0].type != VIBEOS_MEMORY_REGION_USABLE) {
        return -1;
    }
    if (boot_info.memory_map[1].base != 0x200000 || boot_info.memory_map[1].type != VIBEOS_MEMORY_REGION_ACPI_RECLAIMABLE) {
        return -1;
    }
    if (vibeos_bootloader_count_region_type(&boot_info, VIBEOS_MEMORY_REGION_USABLE, &count_usable) != 0 || count_usable != 1) {
        return -1;
    }
    if (vibeos_bootloader_has_overlap(&boot_info, &has_overlap) != 0 || has_overlap != 0) {
        return -1;
    }
    return 0;
}

static int test_bootloader_handoff_metadata(void) {
    vibeos_memory_region_t regions[3];
    vibeos_boot_info_t boot_info;
    uint32_t region_type = 0;
    regions[0].base = 0x100000;
    regions[0].length = 0x400000;
    regions[0].type = VIBEOS_MEMORY_REGION_USABLE;
    regions[0].reserved = 0;
    regions[1].base = 0x90000000ull;
    regions[1].length = 0x100000;
    regions[1].type = VIBEOS_MEMORY_REGION_MMIO;
    regions[1].reserved = 0;
    regions[2].base = 0xA0000000ull;
    regions[2].length = 0x100000;
    regions[2].type = VIBEOS_MEMORY_REGION_RESERVED;
    regions[2].reserved = 0;
    if (vibeos_bootloader_build_boot_info(&boot_info, regions, 3) != 0) {
        return -1;
    }
    if (vibeos_bootloader_set_firmware_tables(&boot_info, 0x101000ull, 0x102000ull) != 0) {
        return -1;
    }
    if (vibeos_bootloader_set_initrd(&boot_info, 0x200000ull, 0x100000ull) != 0) {
        return -1;
    }
    if (vibeos_bootloader_set_framebuffer(&boot_info, 0x90000000ull, 1024, 768) != 0) {
        return -1;
    }
    if (vibeos_bootloader_validate_boot_info(&boot_info) != 0) {
        return -1;
    }
    if (vibeos_bootloader_set_firmware_tables(&boot_info, 0x12345000ull, 0) == 0) {
        return -1;
    }
    if (vibeos_bootloader_set_initrd(&boot_info, 0, 0x1000) == 0) {
        return -1;
    }
    if (vibeos_bootloader_set_framebuffer(&boot_info, 0x90000000ull, 0, 768) == 0) {
        return -1;
    }
    if (vibeos_bootloader_find_region_type_for_range(&boot_info, 0x90000000ull, 0x1000, &region_type) != 0 || region_type != VIBEOS_MEMORY_REGION_MMIO) {
        return -1;
    }
    if (vibeos_bootloader_set_initrd(&boot_info, 0x90001000ull, 0x2000) != 0) {
        return -1;
    }
    if (vibeos_bootloader_validate_boot_info(&boot_info) == 0) {
        return -1;
    }
    return 0;
}

/* Covers firmware tag extraction (ACPI, SMBIOS, secure and measured boot)
 * and the kernel image load plan for both PE32+ and ELF64 inputs, including
 * the rejection of malformed headers. */
static int test_bootloader_firmware_tags_and_pe_plan(void) {
    vibeos_memory_region_t regions[2];
    vibeos_boot_info_t boot_info;
    vibeos_firmware_tag_t tags[4];
    vibeos_boot_image_plan_t plan;
    uint8_t image[1024];
    uint8_t elf_image[512];
    uint8_t elf_invalid[512];
    memset(&boot_info, 0, sizeof(boot_info));
    memset(tags, 0, sizeof(tags));
    memset(&plan, 0, sizeof(plan));
    memset(image, 0, sizeof(image));
    memset(elf_image, 0, sizeof(elf_image));
    memset(elf_invalid, 0, sizeof(elf_invalid));

    /* A usable region plus an MMIO region above it: firmware pointers that
     * land in MMIO must be rejected, which is what the tag checks below
     * depend on. */
    regions[0].base = 0x100000;
    regions[0].length = 0x800000;
    regions[0].type = VIBEOS_MEMORY_REGION_USABLE;
    regions[0].reserved = 0;
    regions[1].base = 0x90000000ull;
    regions[1].length = 0x200000;
    regions[1].type = VIBEOS_MEMORY_REGION_MMIO;
    regions[1].reserved = 0;
    if (vibeos_bootloader_build_boot_info(&boot_info, regions, 2) != 0) {
        return -1;
    }

    tags[0].type = VIBEOS_FIRMWARE_TAG_ACPI_RSDP;
    tags[0].value = 0x101000;
    tags[1].type = VIBEOS_FIRMWARE_TAG_SMBIOS;
    tags[1].value = 0x102000;
    tags[2].type = VIBEOS_FIRMWARE_TAG_SECURE_BOOT;
    tags[2].value = 1;
    tags[3].type = VIBEOS_FIRMWARE_TAG_MEASURED_BOOT;
    tags[3].value = 1;
    if (vibeos_bootloader_apply_firmware_tags(&boot_info, tags, 4) != 0) {
        return -1;
    }
    if ((boot_info.flags & VIBEOS_BOOT_FLAG_SECURE_BOOT) == 0 || (boot_info.flags & VIBEOS_BOOT_FLAG_MEASURED_BOOT) == 0) {
        return -1;
    }

    image[0] = 'M';
    image[1] = 'Z';
    image[0x3c] = 0x80;
    image[0x80] = 'P';
    image[0x81] = 'E';
    image[0x82] = 0;
    image[0x83] = 0;
    image[0x84] = 0x64;
    image[0x85] = 0x86;
    image[0x86] = 0x01;
    image[0x87] = 0x00;
    image[0x94] = 0xF0;
    image[0x95] = 0x00;
    image[0x98] = 0x0B;
    image[0x99] = 0x02;
    image[0xA8] = 0x00;
    image[0xA9] = 0x10;
    image[0xB0] = 0x00;
    image[0xB1] = 0x00;
    image[0xB2] = 0x40;
    image[0xB3] = 0x00;
    image[0xB4] = 0x00;
    image[0xB5] = 0x00;
    image[0xB6] = 0x00;
    image[0xB7] = 0x00;
    image[0x190] = 0x00;
    image[0x191] = 0x20;
    image[0x194] = 0x00;
    image[0x195] = 0x10;
    image[0x198] = 0x00;
    image[0x199] = 0x02;
    image[0x19C] = 0x00;
    image[0x19D] = 0x02;
    image[0x1AC] = 0x20;
    image[0x1AD] = 0x00;
    image[0x1AE] = 0x00;
    image[0x1AF] = 0x60;

    if (vibeos_bootloader_plan_pe_image(image, sizeof(image), &plan) != 0) {
        return -1;
    }
    if (plan.segment_count != 1 || plan.entry_point != 0x401000ull) {
        return -1;
    }
    if (plan.segments[0].image_address != 0x401000ull || plan.segments[0].file_offset != 0x200ull) {
        return -1;
    }
    if ((plan.segments[0].flags & VIBEOS_BOOT_IMAGE_SEGMENT_EXEC) == 0 || (plan.segments[0].flags & VIBEOS_BOOT_IMAGE_SEGMENT_READ) == 0) {
        return -1;
    }

    elf_image[0] = 0x7f;
    elf_image[1] = 'E';
    elf_image[2] = 'L';
    elf_image[3] = 'F';
    elf_image[4] = 2;
    elf_image[5] = 1;
    elf_image[6] = 1;
    elf_image[16] = 0x02;
    elf_image[18] = 0x3e;
    elf_image[20] = 0x01;
    elf_image[24] = 0x00;
    elf_image[25] = 0x10;
    elf_image[26] = 0x40;
    elf_image[32] = 0x40;
    elf_image[54] = 0x38;
    elf_image[56] = 0x01;
    elf_image[64] = 0x01;
    elf_image[68] = 0x05;
    elf_image[72] = 0x00;
    elf_image[73] = 0x01;
    elf_image[80] = 0x00;
    elf_image[81] = 0x10;
    elf_image[82] = 0x40;
    elf_image[88] = 0x00;
    elf_image[89] = 0x10;
    elf_image[90] = 0x40;
    elf_image[96] = 0x20;
    elf_image[104] = 0x40;

    if (vibeos_bootloader_plan_elf_image(elf_image, sizeof(elf_image), &plan) != 0) {
        return -1;
    }
    if (plan.segment_count != 1 || plan.entry_point != 0x401000ull || plan.image_base != 0x401000ull) {
        return -1;
    }
    if (plan.segments[0].file_offset != 0x100ull || plan.segments[0].file_size != 0x20ull || plan.segments[0].mem_size != 0x40ull) {
        return -1;
    }
    if ((plan.segments[0].flags & VIBEOS_BOOT_IMAGE_SEGMENT_EXEC) == 0 || (plan.segments[0].flags & VIBEOS_BOOT_IMAGE_SEGMENT_READ) == 0) {
        return -1;
    }

    memcpy(elf_invalid, elf_image, sizeof(elf_invalid));
    elf_invalid[104] = 0x10;
    elf_invalid[105] = 0x00;
    if (vibeos_bootloader_plan_elf_image(elf_invalid, sizeof(elf_invalid), &plan) == 0) {
        return -1;
    }
    return 0;
}

static int test_timer_and_idt(void) {
    vibeos_timer_t timer;
    vibeos_interrupt_controller_t intc;
    vibeos_x86_64_idt_t idt;
    vibeos_timer_backend_t backend;
    uint32_t irq_vector = 0;
    uint32_t irq_divider = 0;
    int timer_vec;
    if (vibeos_timer_init(&timer, 1000) != 0) {
        return -1;
    }
    vibeos_timer_tick(&timer);
    vibeos_timer_tick(&timer);
    if (vibeos_timer_ticks(&timer) != 2) {
        return -1;
    }
    if (vibeos_timer_ticks_to_ms(&timer, 2) != 2 || vibeos_timer_ticks_to_ns(&timer, 1) != 1000000ull) {
        return -1;
    }
    if (vibeos_timer_arm_deadline(&timer, 3) != 0) {
        return -1;
    }
    if (vibeos_timer_deadline_expired(&timer, 2) != 0) {
        return -1;
    }
    if (vibeos_timer_deadline_expired(&timer, 3) != 1) {
        return -1;
    }
    if (vibeos_x86_64_idt_init(&idt) != 0) {
        return -1;
    }
    timer_vec = vibeos_x86_64_timer_vector();
    if (vibeos_x86_64_idt_set(&idt, (uint32_t)timer_vec) != 0) {
        return -1;
    }
    if (!idt.present[timer_vec]) {
        return -1;
    }
    vibeos_intc_init(&intc);
    if (vibeos_intc_bind_timer_irq(&intc, &timer, (uint32_t)timer_vec) != 0) {
        return -1;
    }
    if (vibeos_timer_backend_info(&timer, &backend, &irq_vector, &irq_divider) != 0) {
        return -1;
    }
    if (backend != VIBEOS_TIMER_BACKEND_IRQ || irq_vector != (uint32_t)timer_vec || irq_divider != 1) {
        return -1;
    }
    if (vibeos_timer_bind_backend(&timer, VIBEOS_TIMER_BACKEND_IRQ, (uint32_t)timer_vec, 2) != 0) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, (uint32_t)timer_vec) != 0) {
        return -1;
    }
    if (vibeos_timer_ticks(&timer) != 2) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, (uint32_t)timer_vec) != 0) {
        return -1;
    }
    if (vibeos_timer_ticks(&timer) != 3) {
        return -1;
    }
    if (vibeos_x86_64_validate_boot_environment(VIBEOS_X86_64_FEATURE_SSE2 | VIBEOS_X86_64_FEATURE_NX) != 0) {
        return -1;
    }
    if (vibeos_x86_64_validate_boot_environment(VIBEOS_X86_64_FEATURE_SSE2) == 0) {
        return -1;
    }
    return 0;
}

static int test_compat_runtime(void) {
    vibeos_compat_runtime_t rt;
    uint32_t native_id = 0;
    uint64_t translated = 0;
    uint64_t denied = 0;
    if (vibeos_compat_init(&rt) != 0) {
        return -1;
    }
    if (vibeos_compat_translate_syscall(&rt, VIBEOS_COMPAT_TARGET_LINUX, 39, &native_id) == 0) {
        return -1;
    }
    if (vibeos_compat_enable(&rt, VIBEOS_COMPAT_TARGET_LINUX, 1) != 0) {
        return -1;
    }
    if (vibeos_compat_translate_syscall(&rt, VIBEOS_COMPAT_TARGET_LINUX, 39, &native_id) != 0 || native_id == 0) {
        return -1;
    }
    if (vibeos_compat_enable(&rt, VIBEOS_COMPAT_TARGET_WINDOWS, 1) != 0) {
        return -1;
    }
    if (vibeos_compat_translate_syscall(&rt, VIBEOS_COMPAT_TARGET_WINDOWS, 0xC0u, &native_id) != 0 || native_id != 10u) {
        return -1;
    }
    if (vibeos_compat_enable(&rt, VIBEOS_COMPAT_TARGET_MACOS, 1) != 0) {
        return -1;
    }
    if (vibeos_compat_translate_syscall(&rt, VIBEOS_COMPAT_TARGET_MACOS, 2u, &native_id) != 0 || native_id != 10u) {
        return -1;
    }
    if (vibeos_compat_stats(&rt, &translated, &denied) != 0) {
        return -1;
    }
    if (translated != 3 || denied < 1) {
        return -1;
    }
    return 0;
}

static int test_waitset(void) {
    vibeos_waitset_t waitset;
    vibeos_scheduler_t sched;
    vibeos_event_t ev;
    size_t count = 0;
    uint32_t registered = 0;
    uint32_t enabled = 0;
    uint32_t signaled = 0;
    vibeos_event_init(&ev);
    if (vibeos_sched_init(&sched, 1) != 0) {
        return -1;
    }
    if (vibeos_waitset_init(&waitset) != 0) {
        return -1;
    }
    if (vibeos_waitset_add(&waitset, &ev) != 0) {
        return -1;
    }
    if (vibeos_waitset_count(&waitset, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_waitset_contention_snapshot(&waitset, &registered, &enabled, &signaled) != 0) {
        return -1;
    }
    if (registered != 1 || enabled != 1 || signaled != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&waitset, 1, &count, &sched, 0) == 0) {
        return -1;
    }
    if (vibeos_sched_wait_timeouts(&sched, 0) != 1) {
        return -1;
    }
    vibeos_event_signal(&ev);
    if (vibeos_waitset_wait_ex(&waitset, 1, &count, &sched, 0) != 0 || count != 0) {
        return -1;
    }
    if (vibeos_waitset_contention_snapshot(&waitset, &registered, &enabled, &signaled) != 0) {
        return -1;
    }
    if (registered != 1 || enabled != 1 || signaled != 1) {
        return -1;
    }
    if (vibeos_sched_wait_wakes(&sched, 0) != 1) {
        return -1;
    }
    return 0;
}

static int test_waitset_timed(void) {
    vibeos_waitset_t waitset;
    vibeos_scheduler_t sched;
    vibeos_timer_t timer;
    vibeos_event_t ev;
    size_t idx = 0;
    if (vibeos_sched_init(&sched, 1) != 0) {
        return -1;
    }
    if (vibeos_timer_init(&timer, 1000) != 0) {
        return -1;
    }
    vibeos_event_init(&ev);
    if (vibeos_waitset_init(&waitset) != 0) {
        return -1;
    }
    if (vibeos_waitset_add(&waitset, &ev) != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_timed(&waitset, &timer, 3, &idx, &sched, 0) == 0) {
        return -1;
    }
    if (vibeos_timer_ticks(&timer) != 3 || vibeos_sched_wait_timeouts(&sched, 0) != 1) {
        return -1;
    }
    vibeos_event_signal(&ev);
    if (vibeos_waitset_wait_timed(&waitset, &timer, 3, &idx, &sched, 0) != 0) {
        return -1;
    }
    if (idx != 0 || vibeos_sched_wait_wakes(&sched, 0) != 1) {
        return -1;
    }
    return 0;
}

static int test_waitset_thread_integration(void) {
    vibeos_waitset_t waitset;
    vibeos_scheduler_t sched;
    vibeos_timer_t timer;
    vibeos_process_table_t pt;
    vibeos_event_t ev;
    vibeos_thread_t sched_thread = { .id = 0, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 0 };
    vibeos_process_state_t proc_state;
    vibeos_thread_state_t state;
    uint32_t wake_cpu = 0;
    uint64_t wait_begin = 0;
    uint64_t wait_end = 0;
    uint64_t requeues = 0;
    uint64_t requeue_failures = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    size_t idx = 0;
    if (vibeos_sched_init(&sched, 1) != 0) {
        return -1;
    }
    if (vibeos_timer_init(&timer, 1000) != 0) {
        return -1;
    }
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &pid) != 0 || vibeos_thread_create(&pt, pid, &tid) != 0) {
        return -1;
    }
    sched_thread.id = tid;
    if (vibeos_sched_enqueue(&sched, &sched_thread) != 0) {
        return -1;
    }
    if (vibeos_thread_state(&pt, tid, &state) != 0 || state != VIBEOS_THREAD_STATE_RUNNABLE) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &proc_state) != 0 || proc_state != VIBEOS_PROCESS_STATE_RUNNING) {
        return -1;
    }
    vibeos_event_init(&ev);
    if (vibeos_waitset_init(&waitset) != 0 || vibeos_waitset_add(&waitset, &ev) != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_for_thread(&waitset, 0, &idx, &sched, 0, &pt, tid) == 0) {
        return -1;
    }
    if (vibeos_thread_state(&pt, tid, &state) != 0 || state != VIBEOS_THREAD_STATE_RUNNABLE) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &proc_state) != 0 || proc_state != VIBEOS_PROCESS_STATE_RUNNING) {
        return -1;
    }
    vibeos_event_signal(&ev);
    if (vibeos_waitset_wait_timed_for_thread_on_cpu(&waitset, &timer, 1, &idx, &sched, 0, &pt, tid, 0, &wake_cpu) != 0 || idx != 0) {
        return -1;
    }
    if (wake_cpu != 0) {
        return -1;
    }
    if (vibeos_thread_state(&pt, tid, &state) != 0 || state != VIBEOS_THREAD_STATE_RUNNABLE) {
        return -1;
    }
    if (vibeos_sched_blocked_threads(&sched) != 0) {
        return -1;
    }
    if (vibeos_sched_wait_transition_summary(&sched, &wait_begin, &wait_end, &requeues, &requeue_failures) != 0) {
        return -1;
    }
    if (wait_begin != 2 || wait_end != 2 || requeues != 2 || requeue_failures != 0) {
        return -1;
    }
    return 0;
}

static int test_waitset_ownership(void) {
    vibeos_waitset_t waitset;
    vibeos_event_t ev;
    uint32_t owner = 0;
    uint32_t enforced = 0;
    vibeos_event_init(&ev);
    if (vibeos_waitset_init_owned(&waitset, 42) != 0) {
        return -1;
    }
    if (vibeos_waitset_owner(&waitset, &owner, &enforced) != 0) {
        return -1;
    }
    if (owner != 42 || enforced != 1) {
        return -1;
    }
    if (vibeos_waitset_add_owned(&waitset, &ev, 7) == 0) {
        return -1;
    }
    if (vibeos_waitset_add_owned(&waitset, &ev, 42) != 0) {
        return -1;
    }
    return 0;
}

static int test_waitset_lifecycle(void) {
    vibeos_waitset_t waitset;
    vibeos_event_t ev1;
    vibeos_event_t ev2;
    size_t count = 0;
    vibeos_event_init(&ev1);
    vibeos_event_init(&ev2);
    if (vibeos_waitset_init(&waitset) != 0) {
        return -1;
    }
    if (vibeos_waitset_add(&waitset, &ev1) != 0 || vibeos_waitset_add(&waitset, &ev2) != 0) {
        return -1;
    }
    if (vibeos_waitset_remove(&waitset, 0) != 0) {
        return -1;
    }
    if (vibeos_waitset_count(&waitset, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_waitset_reset(&waitset) != 0) {
        return -1;
    }
    if (vibeos_waitset_count(&waitset, &count) != 0 || count != 0) {
        return -1;
    }
    if (vibeos_waitset_destroy(&waitset) != 0) {
        return -1;
    }
    if (vibeos_waitset_add(&waitset, &ev1) == 0) {
        return -1;
    }
    if (vibeos_waitset_count(&waitset, &count) == 0) {
        return -1;
    }
    if (vibeos_waitset_init(&waitset) != 0) {
        return -1;
    }
    if (vibeos_waitset_add(&waitset, &ev1) != 0) {
        return -1;
    }
    return 0;
}

static int test_waitset_wake_policy(void) {
    vibeos_waitset_t waitset;
    vibeos_scheduler_t sched;
    vibeos_event_t ev1;
    vibeos_event_t ev2;
    vibeos_waitset_wake_policy_t policy;
    size_t idx = 0;
    if (vibeos_sched_init(&sched, 1) != 0) {
        return -1;
    }
    vibeos_event_init(&ev1);
    vibeos_event_init(&ev2);
    vibeos_event_signal(&ev1);
    vibeos_event_signal(&ev2);
    if (vibeos_waitset_init(&waitset) != 0) {
        return -1;
    }
    if (vibeos_waitset_get_wake_policy(&waitset, &policy) != 0 || policy != VIBEOS_WAITSET_WAKE_FIFO) {
        return -1;
    }
    if (vibeos_waitset_add(&waitset, &ev1) != 0 || vibeos_waitset_add(&waitset, &ev2) != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&waitset, 0, &idx, &sched, 0) != 0 || idx != 0) {
        return -1;
    }
    if (vibeos_waitset_set_wake_policy(&waitset, VIBEOS_WAITSET_WAKE_REVERSE) != 0) {
        return -1;
    }
    if (vibeos_waitset_get_wake_policy(&waitset, &policy) != 0 || policy != VIBEOS_WAITSET_WAKE_REVERSE) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&waitset, 0, &idx, &sched, 0) != 0 || idx != 1) {
        return -1;
    }
    if (vibeos_waitset_reset(&waitset) != 0) {
        return -1;
    }
    vibeos_event_clear(&ev1);
    vibeos_event_clear(&ev2);
    vibeos_event_signal(&ev1);
    vibeos_event_signal(&ev2);
    if (vibeos_waitset_add(&waitset, &ev1) != 0 || vibeos_waitset_add(&waitset, &ev2) != 0) {
        return -1;
    }
    if (vibeos_waitset_set_wake_policy(&waitset, VIBEOS_WAITSET_WAKE_ROUND_ROBIN) != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&waitset, 0, &idx, &sched, 0) != 0 || idx != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&waitset, 0, &idx, &sched, 0) != 0 || idx != 1) {
        return -1;
    }
    if (vibeos_waitset_set_wake_policy(&waitset, (vibeos_waitset_wake_policy_t)99) == 0) {
        return -1;
    }
    return 0;
}

static int test_waitset_stats(void) {
    vibeos_waitset_t ws;
    vibeos_waitset_t owned;
    vibeos_scheduler_t sched;
    vibeos_event_t ev;
    size_t idx = 0;
    uint64_t added = 0;
    uint64_t removed = 0;
    uint64_t waits = 0;
    uint64_t wakes = 0;
    uint64_t timeouts = 0;
    uint64_t denials = 0;
    if (vibeos_sched_init(&sched, 1) != 0) {
        return -1;
    }
    vibeos_event_init(&ev);
    if (vibeos_waitset_init(&ws) != 0) {
        return -1;
    }
    if (vibeos_waitset_add(&ws, &ev) != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&ws, 0, &idx, &sched, 0) == 0) {
        return -1;
    }
    vibeos_event_signal(&ev);
    if (vibeos_waitset_wait_ex(&ws, 0, &idx, &sched, 0) != 0 || idx != 0) {
        return -1;
    }
    if (vibeos_waitset_remove(&ws, 0) != 0) {
        return -1;
    }
    if (vibeos_waitset_stats(&ws, &added, &removed, &waits, &wakes, &timeouts, &denials) != 0) {
        return -1;
    }
    if (added != 1 || removed != 1 || waits != 2 || wakes != 1 || timeouts != 1 || denials != 0) {
        return -1;
    }
    if (vibeos_waitset_init_owned(&owned, 77) != 0) {
        return -1;
    }
    if (vibeos_waitset_add_owned(&owned, &ev, 88) == 0) {
        return -1;
    }
    if (vibeos_waitset_stats(&owned, &added, &removed, &waits, &wakes, &timeouts, &denials) != 0) {
        return -1;
    }
    if (denials != 1) {
        return -1;
    }
    if (vibeos_waitset_stats_reset(&ws) != 0) {
        return -1;
    }
    if (vibeos_waitset_stats(&ws, &added, &removed, &waits, &wakes, &timeouts, &denials) != 0) {
        return -1;
    }
    if (added != 0 || removed != 0 || waits != 0 || wakes != 0 || timeouts != 0 || denials != 0) {
        return -1;
    }
    if (vibeos_waitset_add(&ws, &ev) != 0) {
        return -1;
    }
    if (vibeos_waitset_reset(&ws) != 0) {
        return -1;
    }
    if (vibeos_waitset_stats(&ws, &added, &removed, &waits, &wakes, &timeouts, &denials) != 0) {
        return -1;
    }
    if (added != 1 || removed != 1) {
        return -1;
    }
    return 0;
}

static int test_waitset_priority_and_batch(void) {
    vibeos_waitset_t ws;
    vibeos_event_t ev1;
    vibeos_event_t ev2;
    vibeos_event_t ev3;
    size_t idx = 0;
    size_t indices[3];
    size_t out_count = 0;
    uint8_t prio = 0;
    uint32_t enabled = 0;
    if (vibeos_waitset_init(&ws) != 0) {
        return -1;
    }
    vibeos_event_init(&ev1);
    vibeos_event_init(&ev2);
    vibeos_event_init(&ev3);
    if (vibeos_waitset_add_with_priority(&ws, &ev1, 2) != 0 ||
        vibeos_waitset_add_with_priority(&ws, &ev2, 9) != 0 ||
        vibeos_waitset_add_with_priority(&ws, &ev3, 5) != 0) {
        return -1;
    }
    if (vibeos_waitset_set_wake_policy(&ws, VIBEOS_WAITSET_WAKE_PRIORITY) != 0) {
        return -1;
    }
    vibeos_event_signal(&ev1);
    vibeos_event_signal(&ev2);
    vibeos_event_signal(&ev3);
    if (vibeos_waitset_wait_ex(&ws, 0, &idx, 0, 0) != 0 || idx != 1) {
        return -1;
    }
    if (vibeos_waitset_peek_signaled(&ws, indices, 3, &out_count) != 0 || out_count != 3) {
        return -1;
    }
    if (indices[0] != 1 || indices[1] != 2 || indices[2] != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_batch(&ws, 0, indices, 3, &out_count) != 0 || out_count != 3) {
        return -1;
    }
    if (indices[0] != 1 || indices[1] != 2 || indices[2] != 0) {
        return -1;
    }
    if (vibeos_waitset_set_event_enabled(&ws, 1, 0) != 0) {
        return -1;
    }
    if (vibeos_waitset_get_event_enabled(&ws, 1, &enabled) != 0 || enabled != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&ws, 0, &idx, 0, 0) != 0 || idx != 2) {
        return -1;
    }
    if (vibeos_waitset_set_event_priority(&ws, 0, 12) != 0) {
        return -1;
    }
    if (vibeos_waitset_get_event_priority(&ws, 0, &prio) != 0 || prio != 12) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&ws, 0, &idx, 0, 0) != 0 || idx != 0) {
        return -1;
    }
    if (vibeos_waitset_remove_event(&ws, &ev2) != 0) {
        return -1;
    }
    if (vibeos_waitset_count(&ws, &out_count) != 0 || out_count != 2) {
        return -1;
    }
    return 0;
}

static int test_waitset_wait_all_and_ext_stats(void) {
    vibeos_waitset_t ws;
    vibeos_event_t ev1;
    vibeos_event_t ev2;
    vibeos_waitset_ext_stats_t stats;
    size_t idx = 0;
    if (vibeos_waitset_init(&ws) != 0) {
        return -1;
    }
    vibeos_event_init(&ev1);
    vibeos_event_init(&ev2);
    if (vibeos_waitset_add(&ws, &ev1) != 0 || vibeos_waitset_add(&ws, &ev2) != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_all(&ws, 0, 0) == 0) {
        return -1;
    }
    vibeos_event_signal(&ev1);
    if (vibeos_waitset_wait_all(&ws, 0, 0) == 0) {
        return -1;
    }
    vibeos_event_signal(&ev2);
    if (vibeos_waitset_wait_all(&ws, 0, 1) != 0) {
        return -1;
    }
    if (vibeos_waitset_wait_ex(&ws, 0, &idx, 0, 0) == 0) {
        return -1;
    }
    if (vibeos_waitset_stats_ext(&ws, &stats) != 0) {
        return -1;
    }
    if (stats.wait_all_calls < 3 || stats.wait_all_success < 1 || stats.wait_all_timeouts < 2) {
        return -1;
    }
    if (stats.wait_timeouts < 1) {
        return -1;
    }
    if (vibeos_waitset_stats_reset(&ws) != 0) {
        return -1;
    }
    if (vibeos_waitset_stats_ext(&ws, &stats) != 0) {
        return -1;
    }
    if (stats.wait_all_calls != 0 || stats.wait_batch_calls != 0 || stats.peek_calls != 0) {
        return -1;
    }
    return 0;
}

static int test_waitset_large_fan_in(void) {
    vibeos_waitset_t ws;
    vibeos_event_t events[VIBEOS_WAITSET_MAX_EVENTS];
    size_t indices[VIBEOS_WAITSET_MAX_EVENTS];
    size_t count = 0;
    uint32_t registered = 0;
    uint32_t enabled = 0;
    uint32_t signaled = 0;
    uint32_t i;

    if (vibeos_waitset_init(&ws) != 0 ||
        vibeos_waitset_set_wake_policy(&ws, VIBEOS_WAITSET_WAKE_ROUND_ROBIN) != 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_WAITSET_MAX_EVENTS; i++) {
        vibeos_event_init(&events[i]);
        if (vibeos_waitset_add(&ws, &events[i]) != 0) {
            return -1;
        }
        if ((i & 1u) == 0u) {
            vibeos_event_signal(&events[i]);
        }
    }
    if (vibeos_waitset_contention_snapshot(&ws, &registered, &enabled, &signaled) != 0 ||
        registered != VIBEOS_WAITSET_MAX_EVENTS || enabled != VIBEOS_WAITSET_MAX_EVENTS ||
        signaled != VIBEOS_WAITSET_MAX_EVENTS / 2u) {
        return -1;
    }
    if (vibeos_waitset_wait_batch(&ws, 0, indices, VIBEOS_WAITSET_MAX_EVENTS, &count) != 0 ||
        count != VIBEOS_WAITSET_MAX_EVENTS / 2u) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if ((indices[i] & 1u) != 0u) {
            return -1;
        }
    }
    return vibeos_waitset_destroy(&ws);
}

static int test_filesystem_runtime(void) {
    vibeos_vfs_runtime_t rt;
    vibeos_policy_state_t policy;
    vibeos_security_token_t token;
    uint8_t persist_in[16];
    uint8_t persist_out[16];
    size_t persist_len = 0;
    uint32_t persist_count = 0;
    uint32_t i;
    uint32_t mount_id;
    uint32_t fd;
    uint32_t active_mounts = 0;
    if (vibeos_vfs_runtime_init(&rt) != 0) {
        return -1;
    }
    if (vibeos_vfs_mount(&rt, &mount_id) != 0 || mount_id == 0) {
        return -1;
    }
    if (vibeos_vfs_active_mounts(&rt, &active_mounts) != 0 || active_mounts != 1) {
        return -1;
    }
    if (vibeos_vfs_open(&rt, mount_id, &fd) != 0 || fd < 3) {
        return -1;
    }
    if (vibeos_vfs_close(&rt, fd) != 0) {
        return -1;
    }
    if (vibeos_policy_init(&policy) != 0) {
        return -1;
    }
    if (vibeos_sec_token_init(&token, 1000, 1000, (1u << 0)) != 0) {
        return -1;
    }
    if (vibeos_vfs_open_secure(&rt, mount_id, &policy, &token, &fd) != 0) {
        return -1;
    }
    if (vibeos_vfs_close(&rt, fd) != 0) {
        return -1;
    }
    for (i = 0; i < sizeof(persist_in); i++) {
        persist_in[i] = (uint8_t)(i + 1u);
    }
    if (vibeos_vfs_persist_write(&rt, mount_id, 7, persist_in, sizeof(persist_in)) != 0) {
        return -1;
    }
    if (vibeos_vfs_persist_count(&rt, mount_id, &persist_count) != 0 || persist_count != 1) {
        return -1;
    }
    if (vibeos_vfs_persist_read(&rt, mount_id, 7, persist_out, sizeof(persist_out), &persist_len) != 0 || persist_len != sizeof(persist_in)) {
        return -1;
    }
    if (memcmp(persist_in, persist_out, sizeof(persist_in)) != 0) {
        return -1;
    }
    if (vibeos_vfs_persist_delete(&rt, mount_id, 7) != 0) {
        return -1;
    }
    if (vibeos_vfs_persist_count(&rt, mount_id, &persist_count) != 0 || persist_count != 0) {
        return -1;
    }
    if (vibeos_vfs_unmount(&rt, mount_id) != 0) {
        return -1;
    }
    if (vibeos_vfs_active_mounts(&rt, &active_mounts) != 0 || active_mounts != 0) {
        return -1;
    }
    if (vibeos_vfs_open(&rt, mount_id, &fd) == 0) {
        return -1;
    }
    return 0;
}

static int test_network_runtime(void) {
    vibeos_net_runtime_t rt;
    vibeos_policy_state_t policy;
    vibeos_security_token_t denied;
    vibeos_security_token_t allowed;
    uint32_t sock;
    uint32_t sock2;
    uint64_t total_tx = 0;
    uint64_t total_rx = 0;
    uint64_t sim_ticks = 0;
    uint64_t sim_drops = 0;
    uint64_t latency_ticks = 0;
    uint32_t delivered = 0;
    uint32_t open_sockets = 0;
    char recv_buf[8];
    size_t recv_len = 0;
    const char payload[] = "ping";
    if (vibeos_net_runtime_init(&rt) != 0) {
        return -1;
    }
    if (vibeos_policy_init(&policy) != 0) {
        return -1;
    }
    if (vibeos_sec_token_init(&denied, 1000, 1000, 0) != 0) {
        return -1;
    }
    if (vibeos_sec_token_init(&allowed, 1000, 1000, (1u << 1)) != 0) {
        return -1;
    }
    if (vibeos_socket_create(&rt, &sock) != 0 || sock == 0) {
        return -1;
    }
    if (vibeos_socket_bind_secure(&rt, sock, 1234, &policy, &denied) == 0) {
        return -1;
    }
    if (vibeos_socket_bind_secure(&rt, sock, 1234, &policy, &allowed) != 0) {
        return -1;
    }
    if (vibeos_socket_create(&rt, &sock2) != 0) {
        return -1;
    }
    if (vibeos_socket_bind(&rt, sock2, 1234) == 0) {
        return -1;
    }
    if (vibeos_socket_bind(&rt, sock2, 1235) != 0) {
        return -1;
    }
    if (vibeos_socket_send(&rt, sock, payload, sizeof(payload)) != 0) {
        return -1;
    }
    if (vibeos_socket_receive(&rt, sock, recv_buf, sizeof(recv_buf), &recv_len) != 0 || recv_len != sizeof(recv_buf)) {
        return -1;
    }
    if (vibeos_net_simulate_path(&rt, sock, 5, 2, 3, &delivered, &latency_ticks) != 0) {
        return -1;
    }
    if (delivered != 4 || latency_ticks != 8) {
        return -1;
    }
    if (vibeos_net_stats(&rt, &total_tx, &total_rx, &open_sockets) != 0) {
        return -1;
    }
    if (total_tx != 5 || total_rx != 5 || open_sockets != 2) {
        return -1;
    }
    if (vibeos_net_stats_ext(&rt, &total_tx, &total_rx, &open_sockets, &sim_ticks, &sim_drops) != 0) {
        return -1;
    }
    if (sim_ticks != 8 || sim_drops != 1) {
        return -1;
    }
    if (vibeos_socket_close(&rt, sock2) != 0) {
        return -1;
    }
    if (vibeos_socket_close(&rt, sock) != 0) {
        return -1;
    }
    return 0;
}

static int test_security_token(void) {
    vibeos_security_token_t token;
    vibeos_policy_state_t policy;
    uint32_t mac_enabled = 0;
    if (vibeos_sec_token_init(&token, 1000, 1000, (1u << 1) | (1u << 3)) != 0) {
        return -1;
    }
    if (!vibeos_sec_token_can(&token, 1) || vibeos_sec_token_can(&token, 2)) {
        return -1;
    }
    if (vibeos_policy_init(&policy) != 0) {
        return -1;
    }
    if (vibeos_policy_can_net_bind(&policy, token.capability_mask) != VIBEOS_POLICY_ALLOW) {
        return -1;
    }
    if (vibeos_policy_set_mac_enforced(&policy, 1) != 0) {
        return -1;
    }
    if (vibeos_policy_get_mac_enforced(&policy, &mac_enabled) != 0 || mac_enabled != 1) {
        return -1;
    }
    if (vibeos_policy_can_process_interact_mac(&policy, 7, 4, (1u << 1)) != VIBEOS_POLICY_DENY) {
        return -1;
    }
    if (vibeos_policy_can_process_interact_mac(&policy, 4, 7, token.capability_mask) != VIBEOS_POLICY_ALLOW) {
        return -1;
    }
    return 0;
}

static int test_security_audit_log(void) {
    vibeos_security_audit_log_t log;
    vibeos_sec_audit_event_t event;
    uint32_t count = 0;
    uint32_t total = 0;
    uint32_t success = 0;
    uint32_t fail = 0;
    if (vibeos_sec_audit_init(&log) != 0) {
        return -1;
    }
    if (vibeos_sec_audit_record(&log, VIBEOS_SEC_AUDIT_PROCESS_SPAWN, 1, 2, 0, 1) != 0) {
        return -1;
    }
    if (vibeos_sec_audit_record(&log, VIBEOS_SEC_AUDIT_POLICY_CAPABILITY_SET, 0, 0, 0, 0) != 0) {
        return -1;
    }
    if (vibeos_sec_audit_count(&log, &count) != 0 || count != 2) {
        return -1;
    }
    if (vibeos_sec_audit_count_action(&log, VIBEOS_SEC_AUDIT_PROCESS_SPAWN, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_sec_audit_count_success(&log, 1, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_sec_audit_count_success(&log, 0, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_sec_audit_summary(&log, &total, &success, &fail) != 0) {
        return -1;
    }
    if (total != 2 || success != 1 || fail != 1) {
        return -1;
    }
    if (vibeos_sec_audit_count_for_pid(&log, 1, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_sec_audit_get_for_pid(&log, 1, 0, &event) != 0) {
        return -1;
    }
    if (event.action != VIBEOS_SEC_AUDIT_PROCESS_SPAWN || event.target_pid != 2 || event.success != 1) {
        return -1;
    }
    if (vibeos_sec_audit_reset(&log) != 0) {
        return -1;
    }
    if (vibeos_sec_audit_count(&log, &count) != 0 || count != 0) {
        return -1;
    }
    return 0;
}

static int test_driver_host(void) {
    vibeos_driver_framework_t fw;
    vibeos_devmgr_state_t devmgr;
    vibeos_driver_state_t state;
    if (vibeos_driver_framework_init(&fw) != 0) {
        return -1;
    }
    if (vibeos_devmgr_start(&devmgr) != 0) {
        return -1;
    }
    if (vibeos_driver_host_probe(&fw, &devmgr, 55) != 0) {
        return -1;
    }
    if (fw.count != 1 || devmgr.discovered_devices != 1) {
        return -1;
    }
    if (vibeos_driver_state(&fw, 55, &state) != 0 || state != VIBEOS_DRIVER_LOADED) {
        return -1;
    }
    if (vibeos_driver_unregister(&fw, 55) != 0 || fw.count != 0) {
        return -1;
    }
    return 0;
}

static int test_service_ipc_contract(void) {
    vibeos_service_msg_t msg;
    if (vibeos_service_msg_build(&msg, VIBEOS_SERVICE_SERVICEMGR, VIBEOS_SERVICE_DEVMGR, VIBEOS_SERVICE_MSG_START, 42) != 0) {
        return -1;
    }
    if (vibeos_service_msg_validate(&msg) != 0) {
        return -1;
    }
    msg.msg_type = VIBEOS_SERVICE_MSG_ACK;
    if (vibeos_service_msg_set_reply(&msg, 9, 0) != 0) {
        return -1;
    }
    if (vibeos_service_msg_validate(&msg) != 0) {
        return -1;
    }
    msg.src_service = 99;
    if (vibeos_service_msg_validate(&msg) == 0) {
        return -1;
    }
    return 0;
}

static int test_trap_dispatch(void) {
    vibeos_trap_state_t state;
    vibeos_trap_frame_t frame;
    if (vibeos_trap_state_init(&state) != 0) {
        return -1;
    }
    frame.rip = 0x1000;
    frame.rsp = 0x2000;
    frame.rflags = 0x202;
    frame.error_code = 0;
    frame.cs = 0;
    frame.fault_address = 0;
    frame.vector = 14;
    if (vibeos_trap_dispatch(&state, &frame) != 0) {
        return -1;
    }
    if (state.last_vector != 14 || state.trap_count != 1 || state.class_counts[VIBEOS_TRAP_CLASS_FAULT] != 1) {
        return -1;
    }
    frame.vector = 0x80;
    if (vibeos_trap_dispatch(&state, &frame) != 0) {
        return -1;
    }
    if (vibeos_trap_classify(0x80) != VIBEOS_TRAP_CLASS_SYSCALL) {
        return -1;
    }
    return 0;
}

static int test_trap_fault_decisions(void) {
    vibeos_trap_state_t state;
    vibeos_trap_frame_t frame;
    vibeos_trap_decision_t decision;
    vibeos_log_t log;
    vibeos_log_event_t latest;

    if (vibeos_trap_state_init(&state) != 0 || vibeos_log_init(&log) != 0) {
        return -1;
    }

    memset(&frame, 0, sizeof(frame));
    frame.rip = 0x401000;
    frame.rsp = 0x7fff0000;
    frame.rflags = 0x202;
    frame.cs = VIBEOS_TRAP_X86_USER_CPL;
    frame.fault_address = 0xdeadbeef;
    frame.vector = VIBEOS_TRAP_VECTOR_PAGE_FAULT;

    if (vibeos_trap_dispatch_ex(&state, &frame, &log, 42, &decision) != 0) {
        return -1;
    }
    if (decision.action != VIBEOS_TRAP_ACTION_KILL_CURRENT || decision.user_mode != 1 || decision.current_pid != 42) {
        return -1;
    }
    if (state.last_action != VIBEOS_TRAP_ACTION_KILL_CURRENT || state.action_counts[VIBEOS_TRAP_ACTION_KILL_CURRENT] != 1) {
        return -1;
    }
    if (vibeos_log_latest(&log, &latest) != 0 || latest.level != VIBEOS_LOG_ERROR || strcmp(latest.message, "user_fault_kill_process") != 0) {
        return -1;
    }

    frame.cs = 0;
    frame.vector = VIBEOS_TRAP_VECTOR_GP_FAULT;
    frame.fault_address = 0;
    if (vibeos_trap_dispatch_ex(&state, &frame, &log, 0, &decision) != 0) {
        return -1;
    }
    if (decision.action != VIBEOS_TRAP_ACTION_PANIC || decision.user_mode != 0) {
        return -1;
    }
    if (state.last_action != VIBEOS_TRAP_ACTION_PANIC || state.action_counts[VIBEOS_TRAP_ACTION_PANIC] != 1) {
        return -1;
    }
    if (vibeos_log_latest(&log, &latest) != 0 || latest.level != VIBEOS_LOG_FATAL || strcmp(latest.message, "kernel_fault_panic") != 0) {
        return -1;
    }

    frame.vector = VIBEOS_TRAP_VECTOR_SYSCALL;
    frame.cs = VIBEOS_TRAP_X86_USER_CPL;
    if (vibeos_trap_dispatch_ex(&state, &frame, &log, 42, &decision) != 0) {
        return -1;
    }
    if (decision.action != VIBEOS_TRAP_ACTION_CONTINUE || state.action_counts[VIBEOS_TRAP_ACTION_CONTINUE] != 1) {
        return -1;
    }
    return 0;
}

static int test_trap_debug_resumable(void) {
    vibeos_trap_state_t state;
    vibeos_trap_frame_t frame;
    vibeos_trap_decision_t decision;
    /* #DB (1), #BP (3) and #OF (4) are resumable debug traps: even in kernel
     * mode (cs=0, pid=0) they must yield CONTINUE, not PANIC. They are still
     * FAULT-class for statistics. */
    uint32_t debug_vectors[3] = {1u, 3u, 4u};
    uint32_t i;

    if (vibeos_trap_state_init(&state) != 0) {
        return -1;
    }
    for (i = 0; i < 3u; i++) {
        memset(&frame, 0, sizeof(frame));
        frame.rip = 0x4000500;
        frame.cs = 0; /* kernel CPL */
        frame.vector = debug_vectors[i];
        if (vibeos_trap_dispatch_ex(&state, &frame, 0, 0, &decision) != 0) {
            return -1;
        }
        if (decision.action != VIBEOS_TRAP_ACTION_CONTINUE || decision.user_mode != 0) {
            return -1;
        }
        if (decision.trap_class != VIBEOS_TRAP_CLASS_FAULT) {
            return -1;
        }
    }
    if (state.trap_count != 3 || state.action_counts[VIBEOS_TRAP_ACTION_PANIC] != 0) {
        return -1;
    }
    if (state.action_counts[VIBEOS_TRAP_ACTION_CONTINUE] != 3) {
        return -1;
    }
    return 0;
}

static int test_kernel_trap_fault_handling(void) {
    vibeos_kernel_t kernel;
    vibeos_trap_frame_t frame;
    vibeos_trap_decision_t decision;
    vibeos_process_state_t proc_state;
    vibeos_log_event_t latest;
    uint32_t pid = 0;
    uint32_t tid = 0;

    memset(&kernel, 0, sizeof(kernel));
    if (vibeos_log_init(&kernel.log) != 0 || vibeos_trap_state_init(&kernel.trap_state) != 0 || vibeos_proc_init(&kernel.proc_table) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&kernel.proc_table, 0, &pid) != 0 || vibeos_thread_create(&kernel.proc_table, pid, &tid) != 0) {
        return -1;
    }

    memset(&frame, 0, sizeof(frame));
    frame.rip = 0x401000;
    frame.rsp = 0x7fff0000;
    frame.rflags = 0x202;
    frame.cs = VIBEOS_TRAP_X86_USER_CPL;
    frame.fault_address = 0xcafebabe;
    frame.vector = VIBEOS_TRAP_VECTOR_PAGE_FAULT;

    if (vibeos_kernel_dispatch_trap(&kernel, &frame, pid, &decision) != 0) {
        return -1;
    }
    if (decision.action != VIBEOS_TRAP_ACTION_KILL_CURRENT || kernel.boot_failure_fatal != 0) {
        return -1;
    }
    if (vibeos_proc_state(&kernel.proc_table, pid, &proc_state) != 0 || proc_state != VIBEOS_PROCESS_STATE_TERMINATED) {
        return -1;
    }
    if (vibeos_log_latest(&kernel.log, &latest) != 0 || latest.level != VIBEOS_LOG_WARN || strcmp(latest.message, "trap_user_process_terminated") != 0) {
        return -1;
    }

    frame.cs = 0;
    frame.vector = VIBEOS_TRAP_VECTOR_GP_FAULT;
    frame.fault_address = 0;
    if (vibeos_kernel_dispatch_trap(&kernel, &frame, 0, &decision) != 0) {
        return -1;
    }
    if (decision.action != VIBEOS_TRAP_ACTION_PANIC || kernel.boot_failure_fatal != 1 || kernel.boot_state.last_error_code != 1201) {
        return -1;
    }
    if (vibeos_log_latest(&kernel.log, &latest) != 0 || latest.level != VIBEOS_LOG_FATAL || strcmp(latest.message, "trap_kernel_panic") != 0) {
        return -1;
    }
    return 0;
}

static int test_ipc_handle_transfer(void) {
    vibeos_handle_table_t sender;
    vibeos_handle_table_t receiver;
    uint32_t src_handle = 0;
    uint32_t dst_handle = 0;
    uint32_t rights = 0;
    uint32_t active = 0;
    uint32_t quota = 0;
    uint64_t failures = 0;
    if (vibeos_handle_table_init(&sender) != 0 || vibeos_handle_table_init(&receiver) != 0) {
        return -1;
    }
    if (vibeos_handle_set_quota(&sender, 1) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(&sender, VIBEOS_HANDLE_RIGHT_READ | VIBEOS_HANDLE_RIGHT_SIGNAL, &src_handle) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(&sender, VIBEOS_HANDLE_RIGHT_READ, &dst_handle) == 0) {
        return -1;
    }
    if (vibeos_handle_stats(&sender, &active, &quota, &failures) != 0 || active != 1 || quota != 1 || failures < 1) {
        return -1;
    }
    if (vibeos_ipc_transfer_handle(&sender, &receiver, src_handle, VIBEOS_HANDLE_RIGHT_SIGNAL, &dst_handle) != 0) {
        return -1;
    }
    if (vibeos_handle_rights(&receiver, dst_handle, &rights) != 0 || rights != VIBEOS_HANDLE_RIGHT_SIGNAL) {
        return -1;
    }
    if (vibeos_ipc_transfer_handle(&sender, &receiver, src_handle, VIBEOS_HANDLE_RIGHT_WRITE, &dst_handle) == 0) {
        return -1;
    }
    if (vibeos_ipc_transfer_handle(&sender, &receiver, 0xFFFFFFFFu,
                                   VIBEOS_HANDLE_RIGHT_READ, &dst_handle) == 0) {
        return -1;
    }
    return 0;
}

static int test_handle_lifecycle_hooks(void) {
    vibeos_handle_table_t table;
    handle_hook_stats_t stats;
    uint32_t h1 = 0;
    uint32_t h2 = 0;
    uint32_t revoked = 0;
    memset(&stats, 0, sizeof(stats));
    if (vibeos_handle_table_init(&table) != 0) {
        return -1;
    }
    if (vibeos_handle_set_lifecycle_hook(&table, handle_lifecycle_hook, &stats) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc_object(&table, VIBEOS_HANDLE_RIGHT_READ, VIBEOS_OBJECT_PROCESS, 11, &h1) != 0) {
        return -1;
    }
    if (vibeos_handle_set_provenance(&table, h1, 1, h1) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc_object(&table, VIBEOS_HANDLE_RIGHT_SIGNAL, VIBEOS_OBJECT_THREAD, 12, &h2) != 0) {
        return -1;
    }
    if (vibeos_handle_set_provenance(&table, h2, 1, h1) != 0) {
        return -1;
    }
    if (stats.alloc_events != 2 || stats.close_events != 0 || stats.revoke_events != 0) {
        return -1;
    }
    if (vibeos_handle_close(&table, h1) != 0) {
        return -1;
    }
    if (stats.close_events != 1) {
        return -1;
    }
    if (vibeos_handle_revoke_origin(&table, 2, 1, h1, VIBEOS_OBJECT_NONE, 0, &revoked) != 0) {
        return -1;
    }
    if (revoked != 1 || stats.revoke_events != 1) {
        return -1;
    }
    return 0;
}

static int test_cross_process_handle_dup_policy(void) {
    vibeos_process_table_t pt;
    vibeos_handle_table_t *p1_handles = 0;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    uint32_t p3 = 0;
    uint32_t src_manage_handle = 0;
    uint32_t src_nomgr_handle = 0;
    uint32_t dup_handle = 0;
    uint32_t rights = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p1) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, p1, &p2) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p3) != 0) {
        return -1;
    }
    if (vibeos_proc_handles(&pt, p1, &p1_handles) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(p1_handles, VIBEOS_HANDLE_RIGHT_SIGNAL | VIBEOS_HANDLE_RIGHT_MANAGE, &src_manage_handle) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(p1_handles, VIBEOS_HANDLE_RIGHT_SIGNAL, &src_nomgr_handle) != 0) {
        return -1;
    }
    if (vibeos_proc_duplicate_handle(&pt, p1, p2, src_manage_handle, VIBEOS_HANDLE_RIGHT_SIGNAL, &dup_handle) != 0) {
        return -1;
    }
    if (vibeos_proc_handles(&pt, p2, &p1_handles) != 0) {
        return -1;
    }
    if (vibeos_handle_rights(p1_handles, dup_handle, &rights) != 0 || rights != VIBEOS_HANDLE_RIGHT_SIGNAL) {
        return -1;
    }
    if (vibeos_proc_duplicate_handle(&pt, p1, p3, src_manage_handle, VIBEOS_HANDLE_RIGHT_SIGNAL, &dup_handle) == 0) {
        return -1;
    }
    if (vibeos_proc_duplicate_handle(&pt, p1, p2, src_nomgr_handle, VIBEOS_HANDLE_RIGHT_SIGNAL, &dup_handle) == 0) {
        return -1;
    }
    return 0;
}

static int test_handle_revocation_propagation(void) {
    vibeos_process_table_t pt;
    vibeos_handle_table_t *p1_handles = 0;
    vibeos_handle_table_t *p2_handles = 0;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    uint32_t src = 0;
    uint32_t dup = 0;
    uint32_t unrelated = 0;
    uint32_t rights = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p1) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, p1, &p2) != 0) {
        return -1;
    }
    if (vibeos_proc_handles(&pt, p1, &p1_handles) != 0 || vibeos_proc_handles(&pt, p2, &p2_handles) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(p1_handles, VIBEOS_HANDLE_RIGHT_SIGNAL | VIBEOS_HANDLE_RIGHT_MANAGE, &src) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(p1_handles, VIBEOS_HANDLE_RIGHT_SIGNAL, &unrelated) != 0) {
        return -1;
    }
    if (vibeos_proc_duplicate_handle(&pt, p1, p2, src, VIBEOS_HANDLE_RIGHT_SIGNAL, &dup) != 0) {
        return -1;
    }
    if (vibeos_handle_rights(p2_handles, dup, &rights) != 0 || rights != VIBEOS_HANDLE_RIGHT_SIGNAL) {
        return -1;
    }
    if (vibeos_proc_revoke_handle_lineage(&pt, p1, src) != 0) {
        return -1;
    }
    if (vibeos_handle_rights(p1_handles, src, &rights) == 0) {
        return -1;
    }
    if (vibeos_handle_rights(p2_handles, dup, &rights) == 0) {
        return -1;
    }
    if (vibeos_handle_rights(p1_handles, unrelated, &rights) != 0) {
        return -1;
    }
    return 0;
}

static int test_handle_revocation_scoped(void) {
    vibeos_process_table_t pt;
    vibeos_handle_table_t *p1_handles = 0;
    vibeos_handle_table_t *p2_handles = 0;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    uint32_t t2 = 0;
    uint32_t src_proc = 0;
    uint32_t src_thread = 0;
    uint32_t dup_proc = 0;
    uint32_t dup_thread = 0;
    uint32_t src_rights = 0;
    uint32_t dup_rights = 0;

    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p1) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, p1, &p2) != 0) {
        return -1;
    }
    if (vibeos_thread_create(&pt, p2, &t2) != 0) {
        return -1;
    }
    if (vibeos_proc_handles(&pt, p1, &p1_handles) != 0 || vibeos_proc_handles(&pt, p2, &p2_handles) != 0) {
        return -1;
    }
    if (vibeos_proc_bind_process_handle(&pt, p1, p2, VIBEOS_HANDLE_RIGHT_READ | VIBEOS_HANDLE_RIGHT_MANAGE, &src_proc) != 0) {
        return -1;
    }
    if (vibeos_proc_bind_thread_handle(&pt, p1, t2, VIBEOS_HANDLE_RIGHT_READ | VIBEOS_HANDLE_RIGHT_MANAGE, &src_thread) != 0) {
        return -1;
    }
    if (vibeos_proc_duplicate_handle(&pt, p1, p2, src_proc, VIBEOS_HANDLE_RIGHT_READ, &dup_proc) != 0) {
        return -1;
    }
    if (vibeos_proc_duplicate_handle(&pt, p1, p2, src_thread, VIBEOS_HANDLE_RIGHT_READ, &dup_thread) != 0) {
        return -1;
    }
    if (vibeos_proc_revoke_handle_lineage_scoped(&pt, p1, src_proc, VIBEOS_OBJECT_PROCESS, VIBEOS_HANDLE_RIGHT_READ) != 0) {
        return -1;
    }
    if (vibeos_handle_rights(p1_handles, src_proc, &src_rights) == 0) {
        return -1;
    }
    if (vibeos_handle_rights(p2_handles, dup_proc, &dup_rights) == 0) {
        return -1;
    }
    if (vibeos_handle_rights(p1_handles, src_thread, &src_rights) != 0) {
        return -1;
    }
    if (vibeos_handle_rights(p2_handles, dup_thread, &dup_rights) != 0) {
        return -1;
    }
    return 0;
}

static int test_handle_revocation_audit(void) {
    vibeos_process_table_t pt;
    vibeos_handle_table_t *p1_handles = 0;
    vibeos_handle_table_t *p2_handles = 0;
    vibeos_proc_audit_event_t ev0;
    vibeos_proc_audit_event_t ev1;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    uint32_t src = 0;
    uint32_t dup = 0;
    uint32_t count = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p1) != 0 || vibeos_proc_spawn(&pt, p1, &p2) != 0) {
        return -1;
    }
    if (vibeos_proc_handles(&pt, p1, &p1_handles) != 0 || vibeos_proc_handles(&pt, p2, &p2_handles) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(p1_handles, VIBEOS_HANDLE_RIGHT_SIGNAL | VIBEOS_HANDLE_RIGHT_MANAGE, &src) != 0) {
        return -1;
    }
    if (vibeos_proc_duplicate_handle(&pt, p1, p2, src, VIBEOS_HANDLE_RIGHT_SIGNAL, &dup) != 0) {
        return -1;
    }
    if (vibeos_proc_revoke_handle_lineage(&pt, p1, src) != 0) {
        return -1;
    }
    if (vibeos_proc_revoke_handle_lineage_scoped(&pt, p1, src, VIBEOS_OBJECT_PROCESS, VIBEOS_HANDLE_RIGHT_READ) == 0) {
        return -1;
    }
    if (vibeos_proc_audit_count(&pt, &count) != 0 || count != 2) {
        return -1;
    }
    if (vibeos_proc_audit_get(&pt, 0, &ev0) != 0 || vibeos_proc_audit_get(&pt, 1, &ev1) != 0) {
        return -1;
    }
    if (ev0.action != VIBEOS_PROC_AUDIT_REVOKE_LINEAGE || ev0.success != 1 || ev0.revoked_count == 0) {
        return -1;
    }
    if (ev1.action != VIBEOS_PROC_AUDIT_REVOKE_LINEAGE_SCOPED || ev1.success != 0 || ev1.revoked_count != 0) {
        return -1;
    }
    return 0;
}

static int test_process_lifecycle(void) {
    vibeos_process_table_t pt;
    vibeos_process_state_t state;
    vibeos_handle_table_t *handles = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    uint32_t h = 0;
    uint32_t rights = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &pid) != 0) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &state) != 0 || state != VIBEOS_PROCESS_STATE_NEW) {
        return -1;
    }
    if (vibeos_thread_create(&pt, pid, &tid) != 0 || tid == 0) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &state) != 0 || state != VIBEOS_PROCESS_STATE_RUNNING) {
        return -1;
    }
    if (vibeos_proc_handles(&pt, pid, &handles) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(handles, VIBEOS_HANDLE_RIGHT_SIGNAL, &h) != 0) {
        return -1;
    }
    if (vibeos_proc_terminate(&pt, pid) != 0) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &state) != 0 || state != VIBEOS_PROCESS_STATE_TERMINATED) {
        return -1;
    }
    if (vibeos_handle_rights(handles, h, &rights) == 0) {
        return -1;
    }
    return 0;
}

static int test_process_thread_object_handles(void) {
    vibeos_process_table_t pt;
    vibeos_object_type_t type = VIBEOS_OBJECT_NONE;
    uint32_t object_id = 0;
    uint32_t p1 = 0;
    uint32_t p2 = 0;
    uint32_t p3 = 0;
    uint32_t tid2 = 0;
    uint32_t h_proc = 0;
    uint32_t h_thread = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p1) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, p1, &p2) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p3) != 0) {
        return -1;
    }
    if (vibeos_thread_create(&pt, p2, &tid2) != 0) {
        return -1;
    }
    if (vibeos_proc_bind_process_handle(&pt, p1, p2, VIBEOS_HANDLE_RIGHT_READ, &h_proc) != 0) {
        return -1;
    }
    if (vibeos_proc_resolve_object_handle(&pt, p1, h_proc, &type, &object_id) != 0) {
        return -1;
    }
    if (type != VIBEOS_OBJECT_PROCESS || object_id != p2) {
        return -1;
    }
    if (vibeos_proc_bind_thread_handle(&pt, p1, tid2, VIBEOS_HANDLE_RIGHT_READ, &h_thread) != 0) {
        return -1;
    }
    if (vibeos_proc_resolve_object_handle(&pt, p1, h_thread, &type, &object_id) != 0) {
        return -1;
    }
    if (type != VIBEOS_OBJECT_THREAD || object_id != tid2) {
        return -1;
    }
    if (vibeos_proc_bind_process_handle(&pt, p1, p3, VIBEOS_HANDLE_RIGHT_READ, &h_proc) == 0) {
        return -1;
    }
    return 0;
}

static int test_process_security_tokens(void) {
    vibeos_process_table_t pt;
    vibeos_security_token_t parent_token;
    vibeos_security_token_t child_token;
    vibeos_security_token_t explicit_token;
    vibeos_security_token_t thread_token;
    uint32_t label = 0;
    uint32_t parent = 0;
    uint32_t child = 0;
    uint32_t sibling = 0;
    uint32_t tid = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &parent) != 0) {
        return -1;
    }
    if (vibeos_proc_token_get(&pt, parent, &parent_token) != 0) {
        return -1;
    }
    parent_token.capability_mask = (1u << 4) | (1u << 6);
    if (vibeos_proc_token_set(&pt, parent, &parent_token) != 0) {
        return -1;
    }
    if (vibeos_proc_security_label_set(&pt, parent, 77) != 0) {
        return -1;
    }
    if (vibeos_thread_create(&pt, parent, &tid) != 0) {
        return -1;
    }
    if (vibeos_thread_token_get(&pt, tid, &thread_token) != 0 || thread_token.capability_mask != parent_token.capability_mask) {
        return -1;
    }
    parent_token.capability_mask |= (1u << 9);
    if (vibeos_proc_token_set(&pt, parent, &parent_token) != 0) {
        return -1;
    }
    if (vibeos_thread_token_get(&pt, tid, &thread_token) != 0 || thread_token.capability_mask != parent_token.capability_mask) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, parent, &child) != 0) {
        return -1;
    }
    if (vibeos_proc_token_get(&pt, child, &child_token) != 0) {
        return -1;
    }
    if (child_token.capability_mask != parent_token.capability_mask) {
        return -1;
    }
    if (vibeos_proc_security_label_get(&pt, child, &label) != 0 || label != 77) {
        return -1;
    }
    if (vibeos_sec_token_init(&explicit_token, 42, 43, (1u << 10)) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn_with_token(&pt, 0, &explicit_token, &sibling) != 0) {
        return -1;
    }
    if (vibeos_proc_token_get(&pt, sibling, &child_token) != 0) {
        return -1;
    }
    if (child_token.uid != 42 || child_token.gid != 43 || child_token.capability_mask != (1u << 10)) {
        return -1;
    }
    return 0;
}

static int test_thread_lifecycle_controls(void) {
    vibeos_process_table_t pt;
    vibeos_thread_state_t state;
    uint32_t owner_pid = 0;
    uint64_t proc_transitions = 0;
    uint64_t thread_transitions = 0;
    uint64_t proc_terms = 0;
    uint64_t thread_exits = 0;
    uint32_t pid = 0;
    uint32_t tid = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &pid) != 0) {
        return -1;
    }
    if (vibeos_thread_create(&pt, pid, &tid) != 0) {
        return -1;
    }
    if (vibeos_thread_state(&pt, tid, &state) != 0 || state != VIBEOS_THREAD_STATE_RUNNABLE) {
        return -1;
    }
    if (vibeos_thread_owner(&pt, tid, &owner_pid) != 0 || owner_pid != pid) {
        return -1;
    }
    if (vibeos_thread_set_state(&pt, tid, VIBEOS_THREAD_STATE_BLOCKED) != 0) {
        return -1;
    }
    if (vibeos_thread_state(&pt, tid, &state) != 0 || state != VIBEOS_THREAD_STATE_BLOCKED) {
        return -1;
    }
    if (vibeos_thread_exit(&pt, tid) != 0) {
        return -1;
    }
    if (vibeos_thread_state(&pt, tid, &state) == 0) {
        return -1;
    }
    if (pt.thread_count != 0) {
        return -1;
    }
    if (vibeos_proc_transition_counters(&pt, &proc_transitions, &thread_transitions, &proc_terms, &thread_exits) != 0) {
        return -1;
    }
    if (proc_transitions != 1 || thread_transitions != 3 || proc_terms != 0 || thread_exits != 1) {
        return -1;
    }
    return 0;
}

static int test_process_wait_state_aggregation(void) {
    vibeos_process_table_t pt;
    vibeos_process_state_t proc_state;
    uint32_t pid = 0;
    uint32_t tid1 = 0;
    uint32_t tid2 = 0;
    uint32_t count = 0;
    uint32_t has_runnable = 0;
    uint32_t fully_blocked = 0;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &pid) != 0) {
        return -1;
    }
    if (vibeos_thread_create(&pt, pid, &tid1) != 0 || vibeos_thread_create(&pt, pid, &tid2) != 0) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &proc_state) != 0 || proc_state != VIBEOS_PROCESS_STATE_RUNNING) {
        return -1;
    }
    if (vibeos_proc_thread_wait_begin(&pt, tid1) != 0) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &proc_state) != 0 || proc_state != VIBEOS_PROCESS_STATE_RUNNING) {
        return -1;
    }
    if (vibeos_proc_thread_wait_begin(&pt, tid2) != 0) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &proc_state) != 0 || proc_state != VIBEOS_PROCESS_STATE_BLOCKED) {
        return -1;
    }
    if (vibeos_proc_thread_count_for_process_in_state(&pt, pid, VIBEOS_THREAD_STATE_BLOCKED, &count) != 0 || count != 2) {
        return -1;
    }
    if (vibeos_proc_process_has_runnable_threads(&pt, pid, &has_runnable) != 0 || has_runnable != 0) {
        return -1;
    }
    if (vibeos_proc_process_fully_blocked(&pt, pid, &fully_blocked) != 0 || fully_blocked != 1) {
        return -1;
    }
    if (vibeos_proc_thread_wait_end(&pt, tid1) != 0) {
        return -1;
    }
    if (vibeos_proc_state(&pt, pid, &proc_state) != 0 || proc_state != VIBEOS_PROCESS_STATE_RUNNING) {
        return -1;
    }
    if (vibeos_proc_thread_count_for_process_in_state(&pt, pid, VIBEOS_THREAD_STATE_RUNNABLE, &count) != 0 || count != 1) {
        return -1;
    }
    if (vibeos_proc_process_has_runnable_threads(&pt, pid, &has_runnable) != 0 || has_runnable != 1) {
        return -1;
    }
    if (vibeos_proc_process_fully_blocked(&pt, pid, &fully_blocked) != 0 || fully_blocked != 0) {
        return -1;
    }
    if (vibeos_proc_thread_wait_end(&pt, tid2) != 0) {
        return -1;
    }
    if (vibeos_proc_thread_count_for_process_in_state(&pt, pid, VIBEOS_THREAD_STATE_RUNNABLE, &count) != 0 || count != 2) {
        return -1;
    }
    return 0;
}

static int test_proc_audit_retention_policy(void) {
    vibeos_process_table_t pt;
    vibeos_handle_table_t *p1_handles = 0;
    vibeos_proc_audit_event_t ev;
    uint32_t p1 = 0;
    uint32_t h = 0;
    uint32_t count = 0;
    uint32_t dropped = 0;
    uint32_t i;
    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p1) != 0) {
        return -1;
    }
    if (vibeos_proc_handles(&pt, p1, &p1_handles) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(p1_handles, VIBEOS_HANDLE_RIGHT_SIGNAL | VIBEOS_HANDLE_RIGHT_MANAGE, &h) != 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_PROC_AUDIT_CAPACITY + 5; i++) {
        (void)vibeos_proc_revoke_handle_lineage(&pt, p1, h);
    }
    if (vibeos_proc_audit_count(&pt, &count) != 0 || count != VIBEOS_PROC_AUDIT_CAPACITY) {
        return -1;
    }
    if (vibeos_proc_audit_get_dropped(&pt, &dropped) != 0 || dropped != 0) {
        return -1;
    }
    if (vibeos_proc_audit_get(&pt, 0, &ev) != 0 || ev.seq != 6) {
        return -1;
    }

    if (vibeos_proc_init(&pt) != 0) {
        return -1;
    }
    if (vibeos_proc_spawn(&pt, 0, &p1) != 0) {
        return -1;
    }
    if (vibeos_proc_handles(&pt, p1, &p1_handles) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(p1_handles, VIBEOS_HANDLE_RIGHT_SIGNAL | VIBEOS_HANDLE_RIGHT_MANAGE, &h) != 0) {
        return -1;
    }
    if (vibeos_proc_audit_set_policy(&pt, VIBEOS_PROC_AUDIT_DROP_NEWEST) != 0) {
        return -1;
    }
    for (i = 0; i < VIBEOS_PROC_AUDIT_CAPACITY + 5; i++) {
        (void)vibeos_proc_revoke_handle_lineage(&pt, p1, h);
    }
    if (vibeos_proc_audit_count(&pt, &count) != 0 || count != VIBEOS_PROC_AUDIT_CAPACITY) {
        return -1;
    }
    if (vibeos_proc_audit_get_dropped(&pt, &dropped) != 0 || dropped != 5) {
        return -1;
    }
    if (vibeos_proc_audit_get(&pt, VIBEOS_PROC_AUDIT_CAPACITY - 1, &ev) != 0 || ev.seq != VIBEOS_PROC_AUDIT_CAPACITY) {
        return -1;
    }
    return 0;
}


/* ---- TCP/IP stack (kernel/net/inet.c) ------------------------------------
 *
 * The stack never touches hardware: it is driven entirely through
 * vibeos_inet_input(), the transmit callback and vibeos_inet_poll(). That makes
 * the whole protocol path - checksums, ARP, the TCP state machine - exercisable
 * here, on the same code that runs behind virtio-net on metal.
 */

#define INET_TEST_CAPTURE 16

typedef struct {
    uint8_t frame[INET_TEST_CAPTURE][1600];
    uint32_t len[INET_TEST_CAPTURE];
    uint32_t count;
} inet_capture_t;

static int inet_capture_tx(void *ctx, const void *frame, uint32_t len) {
    inet_capture_t *cap = (inet_capture_t *)ctx;
    if (cap->count < INET_TEST_CAPTURE && len <= sizeof(cap->frame[0])) {
        memcpy(cap->frame[cap->count], frame, len);
        cap->len[cap->count] = len;
        cap->count++;
    }
    return 0;
}

static const uint8_t inet_test_local_mac[6] = {0x52, 0x54, 0x00, 0x11, 0x22, 0x33};
static const uint8_t inet_test_peer_mac[6] = {0x52, 0x54, 0x00, 0xAA, 0xBB, 0xCC};

static void inet_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void inet_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t inet_rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t inet_rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Match the TCP/UDP pseudo-header checksum used by the portable stack. */
static uint16_t inet_l4_checksum(uint32_t src, uint32_t dst, uint8_t proto,
                                 const uint8_t *data, uint32_t len) {
    uint32_t sum = 0;
    uint32_t i;
    uint8_t pseudo[12];

    inet_wr32(pseudo + 0, src);
    inet_wr32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = proto;
    inet_wr16(pseudo + 10, (uint16_t)len);
    for (i = 0; i < sizeof(pseudo); i += 2) {
        sum += ((uint32_t)pseudo[i] << 8) | pseudo[i + 1];
    }
    for (i = 0; i + 1 < len; i += 2) {
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
    }
    if (len & 1u) {
        sum += (uint32_t)data[len - 1u] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/* Build an Ethernet + IPv4 frame carrying `payload` and hand it to the stack. */
static void inet_deliver(vibeos_inet_t *net, uint32_t src, uint32_t dst, uint8_t proto,
                         const uint8_t *payload, uint32_t plen) {
    uint8_t f[1600];
    uint8_t *ip = f + 14;
    uint32_t i;

    memcpy(f, inet_test_local_mac, 6);
    memcpy(f + 6, inet_test_peer_mac, 6);
    inet_wr16(f + 12, 0x0800);

    ip[0] = 0x45;
    ip[1] = 0;
    inet_wr16(ip + 2, (uint16_t)(20 + plen));
    inet_wr16(ip + 4, 1);
    inet_wr16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = proto;
    inet_wr16(ip + 10, 0);
    inet_wr32(ip + 12, src);
    inet_wr32(ip + 16, dst);
    inet_wr16(ip + 10, vibeos_inet_checksum(ip, 20));
    for (i = 0; i < plen; i++) {
        ip[20 + i] = payload[i];
    }
    (void)vibeos_inet_input(net, f, 14 + 20 + plen);
}

/* Build a TCP segment towards us and deliver it. */
static void inet_deliver_tcp(vibeos_inet_t *net, uint32_t src, uint16_t sport, uint16_t dport,
                             uint32_t seq, uint32_t ack, uint8_t flags,
                             const uint8_t *data, uint32_t dlen) {
    uint8_t seg[600];
    uint32_t i;

    inet_wr16(seg + 0, sport);
    inet_wr16(seg + 2, dport);
    inet_wr32(seg + 4, seq);
    inet_wr32(seg + 8, ack);
    seg[12] = 0x50;
    seg[13] = flags;
    inet_wr16(seg + 14, 4096);
    inet_wr16(seg + 16, 0);
    inet_wr16(seg + 18, 0);
    for (i = 0; i < dlen; i++) {
        seg[20 + i] = data[i];
    }
    inet_wr16(seg + 16, inet_l4_checksum(src, net->ip, 6, seg, 20 + dlen));
    inet_deliver(net, src, net->ip, 6, seg, 20 + dlen);
}

static void inet_deliver_udp(vibeos_inet_t *net, uint32_t src, uint16_t sport, uint16_t dport,
                             const uint8_t *data, uint32_t dlen) {
    uint8_t seg[600];

    inet_wr16(seg + 0, sport);
    inet_wr16(seg + 2, dport);
    inet_wr16(seg + 4, (uint16_t)(8u + dlen));
    inet_wr16(seg + 6, 0);
    memcpy(seg + 8, data, dlen);
    inet_deliver(net, src, net->ip, 17, seg, 8u + dlen);
}

static void inet_seed_arp(vibeos_inet_t *net);

static int test_inet_checksum(void) {
    /* A header whose checksum field is already correct must sum to zero. */
    uint8_t ip[20];
    memset(ip, 0, sizeof(ip));
    ip[0] = 0x45;
    inet_wr16(ip + 2, 40);
    ip[8] = 64;
    ip[9] = 6;
    inet_wr32(ip + 12, 0x0A00020Fu);
    inet_wr32(ip + 16, 0x0A000202u);
    inet_wr16(ip + 10, vibeos_inet_checksum(ip, 20));
    if (vibeos_inet_checksum(ip, 20) != 0) {
        return -1;
    }
    if (vibeos_inet_parse_ip("10.0.2.15") != 0x0A00020Fu) {
        return -1;
    }
    if (vibeos_inet_parse_ip("10.0.2") != 0 || vibeos_inet_parse_ip("10.0.2.300") != 0) {
        return -1;
    }
    return 0;
}

/* The adapter must answer consistently in both build configurations, so this
 * test is meaningful whether or not the audited dependency is checked out. */
static int test_tls_dependency(void) {
#if defined(VIBEOS_TLS_MBEDTLS)
    /* Built against the dependency: it must be present and new enough. */
    if (!vibeos_tls_runtime_available()) {
        return -1;
    }
    return (vibeos_tls_library_version() >= VIBEOS_TLS_MIN_VERSION) ? 0 : -1;
#else
    /* Built without it: the adapter must say so plainly rather than claim a
     * version it does not have. */
    if (vibeos_tls_runtime_available()) {
        return -1;
    }
    return (vibeos_tls_library_version() == 0u) ? 0 : -1;
#endif
}

static int test_inet_arp_and_icmp(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    uint8_t arp[28];

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);

    /* An ARP request for our address must produce a reply carrying our MAC. */
    inet_wr16(arp + 0, 1);
    inet_wr16(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    inet_wr16(arp + 6, 1);
    memcpy(arp + 8, inet_test_peer_mac, 6);
    inet_wr32(arp + 14, 0x0A000202u);
    memset(arp + 18, 0, 6);
    inet_wr32(arp + 24, 0x0A00020Fu);
    {
        uint8_t f[64];
        memcpy(f, inet_test_local_mac, 6);
        memcpy(f + 6, inet_test_peer_mac, 6);
        inet_wr16(f + 12, 0x0806);
        memcpy(f + 14, arp, 28);
        if (vibeos_inet_input(&net, f, 14 + 28) != 0) {
            return -1;
        }
    }
    if (cap.count != 1 || inet_rd16(cap.frame[0] + 12) != 0x0806) {
        return -1;
    }
    if (inet_rd16(cap.frame[0] + 14 + 6) != 2) {          /* opcode = reply */
        return -1;
    }
    if (memcmp(cap.frame[0] + 14 + 8, inet_test_local_mac, 6) != 0) {
        return -1;
    }

    /* An ICMP echo request must be answered, using the ARP entry just learned
     * (so no further ARP traffic is generated). */
    cap.count = 0;
    {
        uint8_t icmp[16];
        memset(icmp, 0, sizeof(icmp));
        icmp[0] = 8;
        inet_wr16(icmp + 4, 0x1234);
        inet_wr16(icmp + 6, 1);
        inet_wr16(icmp + 2, vibeos_inet_checksum(icmp, sizeof(icmp)));
        inet_deliver(&net, 0x0A000202u, 0x0A00020Fu, 1, icmp, sizeof(icmp));
    }
    if (cap.count != 1) {
        return -1;
    }
    if (cap.frame[0][14 + 9] != 1 || cap.frame[0][14 + 20] != 0) {   /* ICMP echo reply */
        return -1;
    }
    if (vibeos_inet_checksum(cap.frame[0] + 14, 20) != 0) {          /* valid IP header */
        return -1;
    }
    return 0;
}

static int test_inet_udp(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    int s;
    uint8_t buf[64];
    uint32_t from_ip = 0;
    uint16_t from_port = 0;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);

    s = vibeos_inet_socket(&net, VIBEOS_INET_SOCK_UDP);
    if (s < 0 || vibeos_inet_bind(&net, s, 4242) != 0) {
        return -1;
    }

    /* Nothing received yet. */
    if (vibeos_inet_recvfrom(&net, s, buf, sizeof(buf), &from_ip, &from_port) !=
        -VIBEOS_INET_EAGAIN) {
        return -1;
    }

    {
        uint8_t udp[8 + 5];
        inet_wr16(udp + 0, 9999);
        inet_wr16(udp + 2, 4242);
        inet_wr16(udp + 4, 8 + 5);
        inet_wr16(udp + 6, 0);
        memcpy(udp + 8, "hello", 5);
        inet_deliver(&net, 0x0A000202u, 0x0A00020Fu, 17, udp, sizeof(udp));
    }
    if (vibeos_inet_recvfrom(&net, s, buf, sizeof(buf), &from_ip, &from_port) != 5) {
        return -1;
    }
    if (memcmp(buf, "hello", 5) != 0 || from_ip != 0x0A000202u || from_port != 9999) {
        return -1;
    }

    /* Sending needs ARP first: the datagram is deferred behind a request. */
    cap.count = 0;
    if (vibeos_inet_sendto(&net, s, "pong", 4, 0x0A000202u, 9999) !=
        -VIBEOS_INET_EAGAIN) {
        return -1;
    }
    if (cap.count != 1 || inet_rd16(cap.frame[0] + 12) != 0x0806) {
        return -1;   /* an ARP request, not the datagram */
    }
    return 0;
}

static int test_inet_dhcp_and_dns(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    uint8_t offer[300];
    uint8_t ack[300];
    uint8_t dns[96];
    uint32_t o;
    uint32_t answer_ip = 0xC0000201u;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0 ||
        vibeos_inet_dhcp_start(&net) != 0 || cap.count != 1u) {
        return -1;
    }

    memset(offer, 0, sizeof(offer));
    offer[0] = 2; offer[1] = 1; offer[2] = 6;
    inet_wr32(offer + 4, net.dhcp_xid);
    inet_wr32(offer + 16, 0x0A00020Fu);
    inet_wr32(offer + 236, 0x63825363u);
    o = 240;
    offer[o++] = 53; offer[o++] = 1; offer[o++] = 2;
    offer[o++] = 54; offer[o++] = 4; inet_wr32(offer + o, 0x0A000202u); o += 4;
    offer[o++] = 255;
    inet_deliver_udp(&net, 0x0A000202u, 67, 68, offer, o);
    if (net.dhcp_state != 2u || cap.count < 2u) {
        return -1;
    }

    memset(ack, 0, sizeof(ack));
    ack[0] = 2; ack[1] = 1; ack[2] = 6;
    inet_wr32(ack + 4, net.dhcp_xid);
    inet_wr32(ack + 16, 0x0A00020Fu);
    inet_wr32(ack + 236, 0x63825363u);
    o = 240;
    ack[o++] = 53; ack[o++] = 1; ack[o++] = 5;
    ack[o++] = 1; ack[o++] = 4; inet_wr32(ack + o, 0xFFFFFF00u); o += 4;
    ack[o++] = 3; ack[o++] = 4; inet_wr32(ack + o, 0x0A000202u); o += 4;
    ack[o++] = 6; ack[o++] = 4; inet_wr32(ack + o, 0x0A000203u); o += 4;
    ack[o++] = 255;
    inet_deliver_udp(&net, 0x0A000202u, 67, 68, ack, o);
    if (!vibeos_inet_dhcp_bound(&net) || net.ip != 0x0A00020Fu || net.dns != 0x0A000203u) {
        return -1;
    }

    inet_seed_arp(&net);
    if (vibeos_inet_resolve(&net, "example.test") != 0) {
        return -1;
    }
    memset(dns, 0, sizeof(dns));
    inet_wr16(dns + 0, net.dns_id);
    inet_wr16(dns + 2, 0x8180u);
    inet_wr16(dns + 4, 1);
    inet_wr16(dns + 6, 1);
    o = 12;
    dns[o++] = 7; memcpy(dns + o, "example", 7); o += 7;
    dns[o++] = 4; memcpy(dns + o, "test", 4); o += 4;
    dns[o++] = 0;
    inet_wr16(dns + o, 1); o += 2;
    inet_wr16(dns + o, 1); o += 2;
    dns[o++] = 0xC0; dns[o++] = 0x0C;
    inet_wr16(dns + o, 1); o += 2;
    inet_wr16(dns + o, 1); o += 2;
    inet_wr32(dns + o, 60); o += 4;
    inet_wr16(dns + o, 4); o += 2;
    inet_wr32(dns + o, answer_ip); o += 4;
    inet_deliver_udp(&net, net.dns, 53, 0xC353u, dns, o);
    if (vibeos_inet_resolve_result(&net, &answer_ip) != 0 || answer_ip != 0xC0000201u) {
        return -1;
    }
    {
        uint32_t frames_before = cap.count;
        if (vibeos_inet_resolve(&net, "example.test") != 0 || cap.count != frames_before ||
            vibeos_inet_resolve_result(&net, &answer_ip) != 0 || answer_ip != 0xC0000201u) {
            return -1;
        }
    }
    return 0;
}

static int test_inet_l4_checksum_rejection(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    uint8_t tcp[20];
    uint8_t udp[8];
    uint8_t out[8];
    uint64_t dropped;
    int sock;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    sock = vibeos_inet_socket(&net, VIBEOS_INET_SOCK_UDP);
    if (sock < 0 || vibeos_inet_bind(&net, sock, 4242) != 0) {
        return -1;
    }

    dropped = net.rx_dropped;
    inet_wr16(udp + 0, 9999);
    inet_wr16(udp + 2, 4242);
    inet_wr16(udp + 4, sizeof(udp));
    inet_wr16(udp + 6, 0x1234); /* Invalid non-zero checksum. */
    inet_deliver(&net, 0x0A000202u, net.ip, 17, udp, sizeof(udp));
    if (net.rx_dropped != dropped + 1u ||
        vibeos_inet_recvfrom(&net, sock, out, sizeof(out), 0, 0) != -VIBEOS_INET_EAGAIN) {
        return -1;
    }

    memset(tcp, 0, sizeof(tcp));
    inet_wr16(tcp + 0, 80);
    inet_wr16(tcp + 2, 4243);
    tcp[12] = 0x50;
    tcp[13] = 0x10;
    inet_wr16(tcp + 14, 4096);
    inet_wr16(tcp + 16, 0x1234); /* Invalid mandatory TCP checksum. */
    inet_deliver(&net, 0x0A000202u, net.ip, 6, tcp, sizeof(tcp));
    if (net.rx_dropped != dropped + 2u) {
        return -1;
    }
    return 0;
}

static int test_network_route_and_firewall_policy(void) {
    vibeos_net_policy_t policy;
    vibeos_net_route_t route;

    if (vibeos_net_policy_init(&policy) != 0) {
        return -1;
    }
    if (vibeos_net_route_add(&policy, 0u, 0u, 0x0A000202u, 1u, 100u) != 0 ||
        vibeos_net_route_add(&policy, 0x0A000000u, 8u, 0u, 1u, 50u) != 0 ||
        vibeos_net_route_add(&policy, 0x0A000200u, 24u, 0u, 1u, 10u) != 0) {
        return -1;
    }
    if (vibeos_net_route_lookup(&policy, 0x0A00020Fu, &route) != 0 || route.prefix_len != 24u ||
        vibeos_net_route_lookup(&policy, 0xC0000201u, &route) != 0 || route.prefix_len != 0u) {
        return -1;
    }

    /* Ingress is deny-by-default; an allowed egress flow permits its reply. */
    if (vibeos_net_policy_check(&policy, VIBEOS_NET_DIR_INGRESS, VIBEOS_NET_PROTO_TCP,
                                0x0A00020Fu, 8080u, 0x0A000202u, 40000u, 1u) != 0 ||
        policy.denied_packets != 1u) {
        return -1;
    }
    if (vibeos_net_rule_add(&policy, 1u, VIBEOS_NET_DIR_EGRESS, VIBEOS_NET_PROTO_TCP,
                            40000u, 8080u, 0x0A000200u, 24u) != 0 ||
        vibeos_net_policy_check(&policy, VIBEOS_NET_DIR_EGRESS, VIBEOS_NET_PROTO_TCP,
                                0x0A00020Fu, 40000u, 0x0A000202u, 8080u, 2u) != 1 ||
        vibeos_net_policy_check(&policy, VIBEOS_NET_DIR_INGRESS, VIBEOS_NET_PROTO_TCP,
                                0x0A00020Fu, 40000u, 0x0A000202u, 8080u, 3u) != 1) {
        return -1;
    }
    if (vibeos_net_policy_check(&policy, VIBEOS_NET_DIR_EGRESS, VIBEOS_NET_PROTO_UDP,
                                0x0A00020Fu, 40000u, 0xC0000201u, 53u, 4u) != 0) {
        return -1;
    }
    if (vibeos_net_policy_expire_flows(&policy, 60003u, 60000u) != 1 ||
        vibeos_net_policy_check(&policy, VIBEOS_NET_DIR_INGRESS, VIBEOS_NET_PROTO_TCP,
                                0x0A00020Fu, 40000u, 0x0A000202u, 8080u, 60004u) != 0) {
        return -1;
    }
    if (vibeos_net_rule_remove(&policy, 0u) != 0 || policy.rule_count != 0u ||
        vibeos_net_route_remove(&policy, 2u) != 0 || policy.route_count != 2u ||
        vibeos_net_route_lookup(&policy, 0x0A00020Fu, &route) != 0 || route.prefix_len != 8u) {
        return -1;
    }
    return 0;
}

static void inet_seed_arp(vibeos_inet_t *net);

static int test_network_policy_data_path(void) {
    vibeos_inet_t net;
    vibeos_net_policy_t policy;
    vibeos_inet_t allowed_net;
    vibeos_net_policy_t allowed_policy;
    inet_capture_t cap;
    inet_capture_t allowed_cap;
    uint8_t udp[8 + 2];
    int sock;
    int second_sock;
    uint32_t owner_pid = 0;
    uint64_t denied;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_net_policy_init(&policy) != 0 ||
        vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    vibeos_inet_set_policy(&net, &policy);
    if (vibeos_net_route_add(&policy, 0x0A000200u, 24u, 0u, 1u, 1u) != 0 ||
        vibeos_net_rule_add(&policy, 1u, VIBEOS_NET_DIR_EGRESS, VIBEOS_NET_PROTO_UDP,
                             0u, 0u, 0x0A000200u, 24u) != 0) {
        return -1;
    }
    denied = policy.denied_packets;
    memset(udp, 0, sizeof(udp));
    inet_wr16(udp + 0, 9999u);
    inet_wr16(udp + 2, 4242u);
    inet_wr16(udp + 4, sizeof(udp));
    inet_deliver(&net, 0x0A000202u, net.ip, 17, udp, sizeof(udp));
    if (policy.denied_packets <= denied) {
        return -1;
    }

    memset(&allowed_cap, 0, sizeof(allowed_cap));
    if (vibeos_net_policy_init(&allowed_policy) != 0 ||
        vibeos_inet_init(&allowed_net, inet_test_local_mac, inet_capture_tx, &allowed_cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&allowed_net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    vibeos_inet_set_policy(&allowed_net, &allowed_policy);
    if (vibeos_net_route_add(&allowed_policy, 0x0A000200u, 24u, 0u, 1u, 1u) != 0 ||
        vibeos_net_rule_add(&allowed_policy, 1u, VIBEOS_NET_DIR_EGRESS, VIBEOS_NET_PROTO_UDP,
                            4242u, 9999u, 0x0A000200u, 24u) != 0) {
        return -1;
    }
    inet_seed_arp(&allowed_net);
    sock = vibeos_inet_socket(&allowed_net, VIBEOS_INET_SOCK_UDP);
    if (sock < 0 || vibeos_inet_bind(&allowed_net, sock, 4242u) != 0 ||
        vibeos_inet_socket_set_owner(&allowed_net, sock, 42u) != 0 ||
        vibeos_inet_socket_owner(&allowed_net, sock, &owner_pid) != 0 || owner_pid != 42u ||
        vibeos_inet_sendto(&allowed_net, sock, "ok", 2u, 0x0A000202u, 9999u) != 2) {
        return -1;
    }
    if (vibeos_inet_close_owned(&allowed_net, sock, 43u) == 0 ||
        vibeos_inet_close_owned(&allowed_net, sock, 42u) != 0) {
        return -1;
    }
    second_sock = vibeos_inet_socket(&allowed_net, VIBEOS_INET_SOCK_UDP);
    if (second_sock < 0 || vibeos_inet_socket_set_owner(&allowed_net, second_sock, 42u) != 0 ||
        vibeos_inet_release_owner_sockets(&allowed_net, 42u) != 1 ||
        vibeos_inet_socket_owner(&allowed_net, second_sock, &owner_pid) == 0) {
        return -1;
    }
    return 0;
}

/* Teach a stack the peer's MAC, so a test is not held up by ARP. */
static void inet_seed_arp(vibeos_inet_t *net) {
    uint8_t arp[28];
    uint8_t f[64];
    inet_wr16(arp + 0, 1);
    inet_wr16(arp + 2, 0x0800);
    arp[4] = 6;
    arp[5] = 4;
    inet_wr16(arp + 6, 2);                    /* reply */
    memcpy(arp + 8, inet_test_peer_mac, 6);
    inet_wr32(arp + 14, 0x0A000202u);
    memcpy(arp + 18, inet_test_local_mac, 6);
    inet_wr32(arp + 24, 0x0A00020Fu);
    memcpy(f, inet_test_local_mac, 6);
    memcpy(f + 6, inet_test_peer_mac, 6);
    inet_wr16(f + 12, 0x0806);
    memcpy(f + 14, arp, 28);
    (void)vibeos_inet_input(net, f, 14 + 28);
}


/* Deliver a BOOTP reply (DHCP OFFER/ACK/NAK) carrying the stack's own xid. */
static void inet_deliver_dhcp(vibeos_inet_t *net, uint8_t msg_type,
                              uint32_t yiaddr, uint32_t lease_secs) {
    uint8_t body[300];
    uint32_t o;
    uint32_t i;

    for (i = 0; i < sizeof(body); i++) {
        body[i] = 0;
    }
    body[0] = 2;                       /* BOOTREPLY                       */
    body[1] = 1;
    body[2] = 6;
    inet_wr32(body + 4, net->dhcp_xid);
    inet_wr32(body + 16, yiaddr);      /* yiaddr                          */
    inet_wr32(body + 236, 0x63825363u);/* magic cookie                    */

    o = 240u;
    body[o++] = 53; body[o++] = 1; body[o++] = msg_type;
    body[o++] = 1;  body[o++] = 4; inet_wr32(body + o, 0xFFFFFF00u); o += 4u;
    body[o++] = 3;  body[o++] = 4; inet_wr32(body + o, 0x0A000202u); o += 4u;
    body[o++] = 6;  body[o++] = 4; inet_wr32(body + o, 0x0A000203u); o += 4u;
    body[o++] = 54; body[o++] = 4; inet_wr32(body + o, 0x0A000202u); o += 4u;
    if (lease_secs) {
        body[o++] = 51; body[o++] = 4; inet_wr32(body + o, lease_secs); o += 4u;
    }
    body[o++] = 255;

    {
        uint8_t udp[600];
        uint32_t k;
        inet_wr16(udp + 0, 67);
        inet_wr16(udp + 2, 68);
        inet_wr16(udp + 4, (uint16_t)(8u + o));
        inet_wr16(udp + 6, 0);
        for (k = 0; k < o; k++) {
            udp[8 + k] = body[k];
        }
        inet_deliver(net, 0x0A000202u, 0xFFFFFFFFu, 17, udp, 8u + o);
    }
}

static int test_inet_tcp_connection(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    int s;
    uint32_t our_isn;
    uint32_t peer_isn = 0x50000000u;
    uint16_t our_port;
    uint8_t buf[64];

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    inet_seed_arp(&net);

    s = vibeos_inet_socket(&net, VIBEOS_INET_SOCK_TCP);
    if (s < 0) {
        return -1;
    }

    /* connect() emits a SYN. */
    cap.count = 0;
    if (vibeos_inet_connect(&net, s, 0x0A000202u, 80) != 0) {
        return -1;
    }
    if (cap.count != 1) {
        return -1;
    }
    if ((cap.frame[0][14 + 20 + 13] & 0x02) == 0) {         /* SYN set */
        return -1;
    }
    our_isn = inet_rd32(cap.frame[0] + 14 + 20 + 4);
    our_port = inet_rd16(cap.frame[0] + 14 + 20 + 0);
    if (vibeos_inet_socket_state(&net, s) != VIBEOS_TCP_SYN_SENT) {
        return -1;
    }

    /* SYN|ACK completes the handshake and must be acknowledged. */
    cap.count = 0;
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn, our_isn + 1, 0x12, 0, 0);
    if (vibeos_inet_socket_state(&net, s) != VIBEOS_TCP_ESTABLISHED) {
        return -1;
    }
    if (cap.count < 1 || (cap.frame[0][14 + 20 + 13] & 0x10) == 0) {
        return -1;   /* the ACK that finishes the three-way handshake */
    }

    /* Sending queues data and puts it on the wire with the right sequence. */
    cap.count = 0;
    if (vibeos_inet_send(&net, s, "GET /\n", 6) != 6) {
        return -1;
    }
    if (cap.count != 1) {
        return -1;
    }
    if (inet_rd32(cap.frame[0] + 14 + 20 + 4) != our_isn + 1) {
        return -1;
    }
    if (memcmp(cap.frame[0] + 14 + 20 + 20, "GET /\n", 6) != 0) {
        return -1;
    }

    /* Unacknowledged data must be retransmitted once the timer expires. */
    cap.count = 0;
    vibeos_inet_poll(&net, 10000);
    if (cap.count == 0 || net.tcp_retransmits == 0) {
        return -1;
    }

    /* The peer acknowledges, then sends data of its own. */
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn + 1, our_isn + 7, 0x10, 0, 0);
    cap.count = 0;
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn + 1, our_isn + 7, 0x18,
                     (const uint8_t *)"OK\n", 3);
    if (vibeos_inet_recv(&net, s, buf, sizeof(buf)) != 3 || memcmp(buf, "OK\n", 3) != 0) {
        return -1;
    }
    if (cap.count == 0) {
        return -1;   /* received data must be acknowledged */
    }

    /* A FIN from the peer moves us to CLOSE_WAIT and ends the stream. */
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn + 4, our_isn + 7, 0x11, 0, 0);
    if (vibeos_inet_socket_state(&net, s) != VIBEOS_TCP_CLOSE_WAIT) {
        return -1;
    }
    if (vibeos_inet_recv(&net, s, buf, sizeof(buf)) != 0) {
        return -1;   /* orderly shutdown reads as end of stream */
    }

    /* Closing sends our FIN and waits for the last acknowledgement. */
    cap.count = 0;
    if (vibeos_inet_close(&net, s) != 0) {
        return -1;
    }
    if (cap.count != 1 || (cap.frame[0][14 + 20 + 13] & 0x01) == 0) {
        return -1;
    }
    if (vibeos_inet_socket_state(&net, s) != VIBEOS_TCP_LAST_ACK) {
        return -1;
    }
    return 0;
}

static int test_inet_tcp_listen_accept(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    int srv, conn;
    uint32_t child_isn;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    inet_seed_arp(&net);

    srv = vibeos_inet_socket(&net, VIBEOS_INET_SOCK_TCP);
    if (srv < 0 || vibeos_inet_bind(&net, srv, 8080) != 0) {
        return -1;
    }
    if (vibeos_inet_listen(&net, srv) != 0) {
        return -1;
    }
    if (vibeos_inet_accept(&net, srv) != -VIBEOS_INET_EAGAIN) {
        return -1;
    }

    /* An incoming SYN creates a child connection and answers SYN|ACK. */
    cap.count = 0;
    inet_deliver_tcp(&net, 0x0A000202u, 40000, 8080, 0x900u, 0, 0x02, 0, 0);
    if (cap.count != 1 || (cap.frame[0][14 + 20 + 13] & 0x12) != 0x12) {
        return -1;
    }
    child_isn = inet_rd32(cap.frame[0] + 14 + 20 + 4);
    if (vibeos_inet_accept(&net, srv) != -VIBEOS_INET_EAGAIN) {
        return -1;   /* not connected until the handshake completes */
    }

    /* The final ACK makes it acceptable. */
    inet_deliver_tcp(&net, 0x0A000202u, 40000, 8080, 0x901u, child_isn + 1, 0x10, 0, 0);
    conn = vibeos_inet_accept(&net, srv);
    if (conn < 0) {
        return -1;
    }
    if (vibeos_inet_socket_state(&net, conn) != VIBEOS_TCP_ESTABLISHED) {
        return -1;
    }
    if (vibeos_inet_accept(&net, srv) != -VIBEOS_INET_EAGAIN) {
        return -1;   /* the backlog held exactly one connection */
    }
    return 0;
}


/* ---- Wave 1 reliability ---------------------------------------------------
 * Each of these covers a limit the stack used to have: one datagram per socket,
 * out-of-order segments dropped, closing connections never reclaimed, and a
 * DHCP lease that was never renewed or given up.
 */

static int test_inet_udp_datagram_queue(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    int s;
    uint8_t buf[64];
    uint32_t from_ip = 0;
    uint16_t from_port = 0;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    s = vibeos_inet_socket(&net, VIBEOS_INET_SOCK_UDP);
    if (s < 0 || vibeos_inet_bind(&net, s, 4242) != 0) {
        return -1;
    }

    /* Three datagrams from two different senders, delivered back to back. */
    inet_deliver_udp(&net, 0x0A000202u, 1111, 4242, (const uint8_t *)"one", 3);
    inet_deliver_udp(&net, 0x0A000203u, 2222, 4242, (const uint8_t *)"two", 3);
    inet_deliver_udp(&net, 0x0A000202u, 1111, 4242, (const uint8_t *)"three", 5);

    /* Each recvfrom must return exactly one datagram, in order, with the
     * sender that actually sent it. */
    if (vibeos_inet_recvfrom(&net, s, buf, sizeof(buf), &from_ip, &from_port) != 3 ||
        memcmp(buf, (const uint8_t *)"one", 3) != 0 || from_ip != 0x0A000202u || from_port != 1111) {
        return -1;
    }
    if (vibeos_inet_recvfrom(&net, s, buf, sizeof(buf), &from_ip, &from_port) != 3 ||
        memcmp(buf, (const uint8_t *)"two", 3) != 0 || from_ip != 0x0A000203u || from_port != 2222) {
        return -1;
    }
    if (vibeos_inet_recvfrom(&net, s, buf, sizeof(buf), &from_ip, &from_port) != 5 ||
        memcmp(buf, (const uint8_t *)"three", 5) != 0) {
        return -1;
    }
    if (vibeos_inet_recvfrom(&net, s, buf, sizeof(buf), &from_ip, &from_port) !=
        -VIBEOS_INET_EAGAIN) {
        return -1;
    }

    /* A short buffer truncates the datagram and discards the rest; it must not
     * turn into a second, partial delivery. */
    inet_deliver_udp(&net, 0x0A000202u, 1111, 4242, (const uint8_t *)"abcdefgh", 8);
    if (vibeos_inet_recvfrom(&net, s, buf, 4, &from_ip, &from_port) != 4 ||
        memcmp(buf, "abcd", 4) != 0) {
        return -1;
    }
    if (vibeos_inet_recvfrom(&net, s, buf, sizeof(buf), &from_ip, &from_port) !=
        -VIBEOS_INET_EAGAIN) {
        return -1;
    }
    return 0;
}

static int test_inet_tcp_out_of_order(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    int s;
    uint32_t our_isn, peer_isn = 0x70000000u;
    uint16_t our_port;
    uint8_t buf[64];

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    inet_seed_arp(&net);

    s = vibeos_inet_socket(&net, VIBEOS_INET_SOCK_TCP);
    cap.count = 0;
    if (s < 0 || vibeos_inet_connect(&net, s, 0x0A000202u, 80) != 0) {
        return -1;
    }
    our_isn = inet_rd32(cap.frame[0] + 14 + 20 + 4);
    our_port = inet_rd16(cap.frame[0] + 14 + 20 + 0);
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn, our_isn + 1, 0x12, 0, 0);
    if (vibeos_inet_socket_state(&net, s) != VIBEOS_TCP_ESTABLISHED) {
        return -1;
    }

    /* Send the third chunk first, then the second: both are ahead of the gap
     * and must be held, not dropped and not delivered early. */
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn + 7, our_isn + 1, 0x18,
                     (const uint8_t *)"GHI", 3);
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn + 4, our_isn + 1, 0x18,
                     (const uint8_t *)"DEF", 3);
    if (vibeos_inet_recv(&net, s, buf, sizeof(buf)) != -VIBEOS_INET_EAGAIN) {
        return -1;   /* nothing is deliverable while the first chunk is missing */
    }

    /* The missing chunk arrives: everything must now be readable in order. */
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn + 1, our_isn + 1, 0x18,
                     (const uint8_t *)"ABC", 3);
    if (vibeos_inet_recv(&net, s, buf, sizeof(buf)) != 9 ||
        memcmp(buf, "ABCDEFGHI", 9) != 0) {
        return -1;
    }
    return 0;
}

static int test_inet_tcp_close_reclaims_socket(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    int s;
    uint32_t our_isn, peer_isn = 0x80000000u;
    uint16_t our_port;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    inet_seed_arp(&net);

    s = vibeos_inet_socket(&net, VIBEOS_INET_SOCK_TCP);
    cap.count = 0;
    if (s < 0 || vibeos_inet_connect(&net, s, 0x0A000202u, 80) != 0) {
        return -1;
    }
    our_isn = inet_rd32(cap.frame[0] + 14 + 20 + 4);
    our_port = inet_rd16(cap.frame[0] + 14 + 20 + 0);
    inet_deliver_tcp(&net, 0x0A000202u, 80, our_port, peer_isn, our_isn + 1, 0x12, 0, 0);

    /* Close, then let the peer go silent: the slot must still come back. */
    if (vibeos_inet_close(&net, s) != 0) {
        return -1;
    }
    if (vibeos_inet_socket_state(&net, s) != VIBEOS_TCP_FIN_WAIT_1) {
        return -1;
    }
    vibeos_inet_poll(&net, VIBEOS_INET_TIME_WAIT_MS + 1000ull);
    if (vibeos_inet_socket_state(&net, s) != -VIBEOS_INET_EINVAL) {
        return -1;   /* the socket is gone, so querying it is invalid */
    }
    /* And the freed slot is reusable. */
    if (vibeos_inet_socket(&net, VIBEOS_INET_SOCK_TCP) < 0) {
        return -1;
    }
    return 0;
}

static int test_inet_dhcp_lease_lifecycle(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    if (vibeos_inet_dhcp_start(&net) != 0) {
        return -1;
    }
    inet_deliver_dhcp(&net, 2 /* OFFER */, 0x0A00020Fu, 100u);
    inet_deliver_dhcp(&net, 5 /* ACK */, 0x0A00020Fu, 100u);
    if (!vibeos_inet_dhcp_bound(&net) || net.ip != 0x0A00020Fu) {
        return -1;
    }
    if (net.dhcp_lease_secs != 100u) {
        return -1;
    }

    /* Before T1 nothing is sent. */
    cap.count = 0;
    vibeos_inet_poll(&net, 10000);
    if (cap.count != 0) {
        return -1;
    }

    /* At T1 (half the lease) the client renews, and the ACK extends it. */
    vibeos_inet_poll(&net, 51000);
    if (cap.count == 0) {
        return -1;
    }
    inet_deliver_dhcp(&net, 5, 0x0A00020Fu, 100u);
    if (net.dhcp_renewals != 1u || !vibeos_inet_dhcp_bound(&net)) {
        return -1;
    }
    if (net.ip != 0x0A00020Fu) {
        return -1;
    }

    /* If nothing answers past the expiry, the address is given up and the
     * client goes back to discovering rather than using a dead lease. */
    vibeos_inet_poll(&net, 51000 + 200000);
    if (net.ip != 0u || vibeos_inet_dhcp_bound(&net)) {
        return -1;
    }
    return 0;
}

static int test_inet_dns_timeout_and_negative_cache(void) {
    static vibeos_inet_t net;
    static inet_capture_t cap;
    uint32_t ip = 0;

    memset(&cap, 0, sizeof(cap));
    if (vibeos_inet_init(&net, inet_test_local_mac, inet_capture_tx, &cap) != 0) {
        return -1;
    }
    vibeos_inet_set_addr(&net, 0x0A00020Fu, 0xFFFFFF00u, 0x0A000202u, 0x0A000203u);
    inet_seed_arp(&net);

    cap.count = 0;
    if (vibeos_inet_resolve(&net, "example.test") != 0) {
        return -1;
    }
    if (vibeos_inet_resolve_result(&net, &ip) != -VIBEOS_INET_EAGAIN) {
        return -1;
    }

    /* Nothing answers: the query is retried a bounded number of times and then
     * fails, instead of leaving the caller waiting forever. */
    {
        uint64_t t;
        int sends = (int)cap.count;
        for (t = 1000; t <= 6000; t += 1000) {
            vibeos_inet_poll(&net, t);
        }
        if ((int)cap.count <= sends) {
            return -1;   /* it must have retried at least once */
        }
    }
    if (vibeos_inet_resolve_result(&net, &ip) == -VIBEOS_INET_EAGAIN) {
        return -1;       /* it must have stopped waiting */
    }
    if (net.dns_timeouts == 0u) {
        return -1;
    }
    return 0;
}


/* ---- ELF program-image parser --------------------------------------------
 *
 * These build ELF headers by hand so the malformed cases can be expressed at
 * all: a compiler will not emit a segment whose filesz exceeds its memsz, or
 * a program header that runs off the end of the file, and those are exactly
 * the inputs the parser has to survive.
 */

#define ELFT_BASE 0x8000000000ull
#define ELFT_LIMIT 0x8000400000ull

static void elft_w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void elft_w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void elft_w64(uint8_t *p, uint64_t v) {
    elft_w32(p, (uint32_t)v); elft_w32(p + 4, (uint32_t)(v >> 32));
}

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t off, vaddr, filesz, memsz;
} elft_seg_t;

/* Assemble a minimal but structurally valid ELF64 executable. */
static uint64_t elft_build(uint8_t *buf, uint64_t cap, uint64_t entry,
                           const elft_seg_t *segs, uint32_t nseg, uint16_t etype) {
    uint64_t phoff = 64;
    uint32_t i;
    uint64_t used = phoff + (uint64_t)nseg * 56u;

    for (i = 0; i < cap; i++) {
        buf[i] = 0;
    }
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 2;   /* ELFCLASS64  */
    buf[5] = 1;   /* little endian */
    buf[6] = 1;   /* version */
    elft_w16(buf + 16, etype);
    elft_w16(buf + 18, 62);          /* EM_X86_64 */
    elft_w32(buf + 20, 1);
    elft_w64(buf + 24, entry);
    elft_w64(buf + 32, phoff);
    elft_w16(buf + 52, 64);          /* e_ehsize    */
    elft_w16(buf + 54, 56);          /* e_phentsize */
    elft_w16(buf + 56, (uint16_t)nseg);

    for (i = 0; i < nseg; i++) {
        uint8_t *ph = buf + phoff + (uint64_t)i * 56u;
        elft_w32(ph + 0, segs[i].type);
        elft_w32(ph + 4, segs[i].flags);
        elft_w64(ph + 8, segs[i].off);
        elft_w64(ph + 16, segs[i].vaddr);
        elft_w64(ph + 24, segs[i].vaddr);
        elft_w64(ph + 32, segs[i].filesz);
        elft_w64(ph + 40, segs[i].memsz);
        elft_w64(ph + 48, 0x1000);
        if (segs[i].off + segs[i].filesz > used) {
            used = segs[i].off + segs[i].filesz;
        }
    }
    return used;
}

/* ---- FAT cluster-chain reader -------------------------------------------
 *
 * A fabricated volume, so the run-coalescing reader can be driven over chains
 * that are contiguous, fragmented, short, and failing - none of which can be
 * arranged on a real disk on demand. Sector contents are a function of the
 * sector number, so a request aimed at the wrong cluster shows up as wrong
 * bytes rather than as bytes that happen to match.
 */

#define FATT_MAX_CLUSTERS 64u
#define FATT_EOC 0x0FFFFFFFu
#define FATT_BASE_LBA 1000u

static uint32_t fatt_table[FATT_MAX_CLUSTERS];
static uint8_t fatt_owned[FATT_MAX_CLUSTERS];
static uint32_t fatt_spc;
static uint32_t fatt_requests;
static uint32_t fatt_largest_request;
static int fatt_stray_request;      /* a read touched a cluster the file does not own */
static long fatt_fail_after;        /* -1 never; otherwise fail once this many reads have run */

static uint8_t fatt_byte(uint32_t lba, uint32_t off) {
    return (uint8_t)(lba * 7u + off * 3u + 1u);
}

static uint32_t fatt_lba_of(void *ctx, uint32_t cluster) {
    (void)ctx;
    return FATT_BASE_LBA + (cluster - 2u) * fatt_spc;
}

static uint32_t fatt_next(void *ctx, uint32_t cluster) {
    (void)ctx;
    return (cluster < FATT_MAX_CLUSTERS) ? fatt_table[cluster] : FATT_EOC;
}

static int fatt_end(void *ctx, uint32_t cluster) {
    (void)ctx;
    return cluster >= 0x0FFFFFF8u;
}

static int fatt_serve(uint32_t lba, uint8_t *dst, uint32_t sectors, uint32_t bytes) {
    uint32_t s, i;

    fatt_requests++;
    if (sectors > fatt_largest_request) {
        fatt_largest_request = sectors;
    }
    if (fatt_fail_after >= 0) {
        if (fatt_fail_after == 0) {
            return -1;
        }
        fatt_fail_after--;
    }
    for (s = 0; s < sectors; s++) {
        uint32_t cluster = 2u + (lba + s - FATT_BASE_LBA) / fatt_spc;
        uint32_t n = (bytes < 512u) ? bytes : 512u;
        if (cluster >= FATT_MAX_CLUSTERS || !fatt_owned[cluster]) {
            fatt_stray_request = 1;
            return -1;
        }
        for (i = 0; i < n; i++) {
            dst[s * 512u + i] = fatt_byte(lba + s, i);
        }
    }
    return 0;
}

static int fatt_read_sectors(void *ctx, uint32_t lba, void *dst, uint32_t sectors) {
    (void)ctx;
    return fatt_serve(lba, (uint8_t *)dst, sectors, 512u);
}

static int fatt_read_partial(void *ctx, uint32_t lba, void *dst, uint32_t bytes) {
    (void)ctx;
    if (bytes == 0u || bytes >= 512u) {
        return -1;
    }
    return fatt_serve(lba, (uint8_t *)dst, 1u, bytes);
}

static void fatt_build(const uint32_t *chain, uint32_t n, uint32_t spc) {
    uint32_t i;
    fatt_spc = spc;
    for (i = 0; i < FATT_MAX_CLUSTERS; i++) {
        fatt_table[i] = FATT_EOC;
        fatt_owned[i] = 0;
    }
    for (i = 0; i < n; i++) {
        fatt_owned[chain[i]] = 1;
        fatt_table[chain[i]] = (i + 1u < n) ? chain[i + 1u] : FATT_EOC;
    }
    fatt_requests = 0;
    fatt_largest_request = 0;
    fatt_stray_request = 0;
    fatt_fail_after = -1;
}

/* What the reader replaced: one sector at a time, straight down the chain. */
static uint32_t fatt_reference(const uint32_t *chain, uint32_t n, uint32_t size, uint8_t *out) {
    uint32_t copied = 0, c, s, i;
    for (c = 0; c < n && copied < size; c++) {
        for (s = 0; s < fatt_spc && copied < size; s++) {
            uint32_t lba = FATT_BASE_LBA + (chain[c] - 2u) * fatt_spc + s;
            uint32_t take = size - copied;
            if (take > 512u) {
                take = 512u;
            }
            for (i = 0; i < take; i++) {
                out[copied + i] = fatt_byte(lba, i);
            }
            copied += take;
        }
    }
    return copied;
}

static void fatt_io(vibeos_fat_chain_io_t *io) {
    io->ctx = NULL;
    io->cluster_bytes = fatt_spc * 512u;
    io->next_cluster = fatt_next;
    io->chain_end = fatt_end;
    io->cluster_lba = fatt_lba_of;
    io->read_sectors = fatt_read_sectors;
    io->read_partial = fatt_read_partial;
}

static uint8_t fatt_got[FATT_MAX_CLUSTERS * 512u * 8u];
static uint8_t fatt_want[FATT_MAX_CLUSTERS * 512u * 8u];

static int fatt_case(const uint32_t *chain, uint32_t n, uint32_t spc, uint32_t size) {
    vibeos_fat_chain_io_t io;
    long got;

    fatt_build(chain, n, spc);
    fatt_io(&io);
    memset(fatt_got, 0xAA, size + 1u);
    memset(fatt_want, 0xBB, size + 1u);
    got = vibeos_fat_chain_read(&io, chain[0], size, fatt_got);
    if (got != (long)size || fatt_stray_request) {
        return -1;
    }
    if (fatt_reference(chain, n, size, fatt_want) != size) {
        return -1;
    }
    if (memcmp(fatt_got, fatt_want, size) != 0) {
        return -1;
    }
    /* One byte past the end must be untouched: the last sector of a file is
     * rarely full, and the run reader writes device sectors straight into the
     * caller's buffer. */
    if (fatt_got[size] != 0xAAu) {
        return -1;
    }
    return 0;
}

static int test_fat_chain_layouts(void) {
    uint32_t chain[8];
    uint32_t spcs[3] = {1u, 2u, 8u};
    uint32_t si, n, pat, k, size;

    for (si = 0; si < 3u; si++) {
        uint32_t spc = spcs[si];
        uint32_t cb = spc * 512u;
        for (n = 1u; n <= 5u; n++) {
            for (pat = 0; pat < (1u << (n - 1u)); pat++) {
                uint32_t total = n * cb;
                uint32_t cur = 3u;
                chain[0] = cur;
                for (k = 1u; k < n; k++) {
                    /* Bit set: a gap in the chain, so the run cannot coalesce
                     * across it. Every arrangement of gaps is covered. */
                    cur += (pat & (1u << (k - 1u))) ? 7u : 1u;
                    chain[k] = cur;
                }
                for (size = 1u; size <= total; size += (cb <= 512u) ? 1u : 173u) {
                    if (fatt_case(chain, n, spc, size) != 0) {
                        return -1;
                    }
                }
                /* Every cluster and sector boundary, which is where a
                 * miscounted run shows up first. */
                for (k = 1u; k <= n; k++) {
                    if (fatt_case(chain, n, spc, k * cb) != 0 ||
                        fatt_case(chain, n, spc, k * cb - 1u) != 0) {
                        return -1;
                    }
                }
                if (fatt_case(chain, n, spc, total) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int test_fat_chain_coalescing(void) {
    uint32_t contiguous[4] = {3u, 4u, 5u, 6u};
    uint32_t fragmented[4] = {3u, 9u, 4u, 20u};
    vibeos_fat_chain_io_t io;

    /* A contiguous file is the whole point of the run reader: four clusters
     * must cost one request, not four. This is what turned a two-megabyte
     * program from four thousand round trips into tens. */
    fatt_build(contiguous, 4u, 2u);
    fatt_io(&io);
    if (vibeos_fat_chain_read(&io, 3u, 4u * 1024u, fatt_got) != (long)(4u * 1024u)) {
        return -1;
    }
    if (fatt_requests != 1u || fatt_largest_request != 8u || fatt_stray_request) {
        return -1;
    }

    /* A fragmented chain must fall back to one request per cluster and must
     * not coalesce across the gap - the sectors after cluster 3 belong to
     * something else. */
    fatt_build(fragmented, 4u, 2u);
    fatt_io(&io);
    if (vibeos_fat_chain_read(&io, 3u, 4u * 1024u, fatt_got) != (long)(4u * 1024u)) {
        return -1;
    }
    if (fatt_requests != 4u || fatt_largest_request != 2u || fatt_stray_request) {
        return -1;
    }

    /* A run stops at the last cluster the file needs even when the chain runs
     * on contiguously past it: a file may hold fewer bytes than its chain. */
    fatt_build(contiguous, 4u, 2u);
    fatt_io(&io);
    if (vibeos_fat_chain_read(&io, 3u, 2048u, fatt_got) != 2048L) {
        return -1;
    }
    if (fatt_requests != 1u || fatt_largest_request != 4u || fatt_stray_request) {
        return -1;
    }

    /* A file that ends part way through a cluster still needs that cluster in
     * the run. Counting the whole clusters and stopping there leaves the
     * remainder to a request of its own - correct, but it is the shape of the
     * mistake this bound exists to prevent, so it is asserted against: 2560
     * bytes of an eight-sector chain is five sectors in one request. */
    fatt_build(contiguous, 4u, 2u);
    fatt_io(&io);
    if (vibeos_fat_chain_read(&io, 3u, 2560u, fatt_got) != 2560L) {
        return -1;
    }
    if (fatt_requests != 1u || fatt_largest_request != 5u || fatt_stray_request) {
        return -1;
    }
    return 0;
}

static int test_fat_chain_short_and_failed(void) {
    uint32_t chain[2] = {3u, 4u};
    vibeos_fat_chain_io_t io;
    long got;

    /* A chain shorter than the size the directory claims must report the
     * short count. Returning the claimed size here is what let execve parse a
     * buffer whose tail still held the previous program. */
    fatt_build(chain, 2u, 1u);
    fatt_io(&io);
    got = vibeos_fat_chain_read(&io, 3u, 4u * 512u, fatt_got);
    if (got != 2L * 512L || fatt_stray_request) {
        return -1;
    }

    /* A device error is an error, not a short file. */
    fatt_build(chain, 2u, 1u);
    fatt_io(&io);
    fatt_fail_after = 0;
    if (vibeos_fat_chain_read(&io, 3u, 2u * 512u, fatt_got) != -1L) {
        return -1;
    }

    /* Nothing to read is not a failure. */
    fatt_build(chain, 2u, 1u);
    fatt_io(&io);
    if (vibeos_fat_chain_read(&io, 3u, 0u, fatt_got) != 0L || fatt_requests != 0u) {
        return -1;
    }

    /* A cluster number below the first data cluster ends the walk rather than
     * indexing behind the data area. */
    fatt_build(chain, 2u, 1u);
    fatt_io(&io);
    if (vibeos_fat_chain_read(&io, 1u, 512u, fatt_got) != 0L) {
        return -1;
    }
    return 0;
}

static int test_elf_parse_valid(void) {
    static uint8_t img[8192];
    vibeos_elf_image_t out;
    elft_seg_t segs[2] = {
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0x400, ELFT_BASE, 0x100, 0x100},
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_W, 0x600, ELFT_BASE + 0x1000, 0x40, 0x200},
    };
    uint64_t len = elft_build(img, sizeof(img), ELFT_BASE + 0x10, segs, 2, 2);

    if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_OK) {
        return -1;
    }
    if (out.count != 2 || out.entry != ELFT_BASE + 0x10) {
        return -1;
    }
    if (out.min_vaddr != ELFT_BASE || out.end_vaddr != ELFT_BASE + 0x2000) {
        return -1;
    }
    /* The program headers live inside the first segment, so AT_PHDR resolves. */
    if (out.phdr_vaddr != ELFT_BASE + (64 - 0x400)) {
        /* phoff 64 is before the segment's file offset, so it is not covered */
        if (out.phdr_vaddr != 0) {
            return -1;
        }
    }
    /* Permissions come from the segment covering each page. */
    if (vibeos_elf_page_flags(&out, ELFT_BASE) != (VIBEOS_ELF_R | VIBEOS_ELF_X)) {
        return -1;
    }
    if (vibeos_elf_page_flags(&out, ELFT_BASE + 0x1000) != (VIBEOS_ELF_R | VIBEOS_ELF_W)) {
        return -1;
    }
    if (vibeos_elf_page_flags(&out, ELFT_BASE + 0x8000) != 0) {
        return -1;   /* nothing there */
    }
    return 0;
}

/* The case the old loader got wrong: .text ending part way through the page
 * where .data begins. Mapping segment by segment allocated that page twice and
 * lost the first one's bytes. */
static int test_elf_shared_page(void) {
    static uint8_t img[8192];
    static uint8_t page[VIBEOS_ELF_PAGE_SIZE];
    vibeos_elf_image_t out;
    uint64_t len;
    uint32_t i;
    elft_seg_t segs[2] = {
        /* text: 0x000..0x800 of the page */
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0x400, ELFT_BASE, 0x800, 0x800},
        /* data: 0x800..0xa00 of the SAME page, plus bss to 0xc00 */
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_W, 0xC00, ELFT_BASE + 0x800, 0x200, 0x400},
    };

    len = elft_build(img, sizeof(img), ELFT_BASE, segs, 2, 2);
    for (i = 0; i < 0x800; i++) {
        img[0x400 + i] = 0xAA;          /* text bytes  */
    }
    for (i = 0; i < 0x200; i++) {
        img[0xC00 + i] = 0xBB;          /* data bytes  */
    }
    if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_OK) {
        return -1;
    }
    /* One page, and it must carry the permissions of both segments. */
    if (out.min_vaddr != ELFT_BASE || out.end_vaddr != ELFT_BASE + 0x1000) {
        return -1;
    }
    if (vibeos_elf_page_flags(&out, ELFT_BASE) !=
        (VIBEOS_ELF_R | VIBEOS_ELF_W | VIBEOS_ELF_X)) {
        return -1;
    }
    /* And both segments' bytes have to survive in it. */
    vibeos_elf_fill_page(&out, img, ELFT_BASE, page);
    for (i = 0; i < 0x800; i++) {
        if (page[i] != 0xAA) {
            return -1;
        }
    }
    for (i = 0x800; i < 0xA00; i++) {
        if (page[i] != 0xBB) {
            return -1;
        }
    }
    for (i = 0xA00; i < VIBEOS_ELF_PAGE_SIZE; i++) {
        if (page[i] != 0x00) {
            return -1;   /* .bss and padding must be zero */
        }
    }
    return 0;
}

static int test_elf_rejects_malformed(void) {
    static uint8_t img[8192];
    vibeos_elf_image_t out;
    uint64_t len;
    elft_seg_t ok = {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0x400, ELFT_BASE, 0x100, 0x100};

    /* Not an ELF at all. */
    len = elft_build(img, sizeof(img), ELFT_BASE, &ok, 1, 2);
    img[1] = 'X';
    if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_ENOTELF) {
        return -1;
    }

    /* Wrong machine. */
    len = elft_build(img, sizeof(img), ELFT_BASE, &ok, 1, 2);
    elft_w16(img + 18, 40);
    if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_EMACHINE) {
        return -1;
    }

    /* Dynamic executable: refused explicitly, not loaded half way. */
    len = elft_build(img, sizeof(img), ELFT_BASE, &ok, 1, 3 /* ET_DYN */);
    if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_EDYNAMIC) {
        return -1;
    }

    /* Needs an interpreter. */
    {
        elft_seg_t segs[2] = {ok, {3u /* PT_INTERP */, 4u, 0x600, 0, 0x10, 0x10}};
        len = elft_build(img, sizeof(img), ELFT_BASE, segs, 2, 2);
        if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_EDYNAMIC) {
            return -1;
        }
    }

    /* filesz beyond memsz. */
    {
        elft_seg_t bad = ok;
        bad.filesz = 0x200;
        bad.memsz = 0x100;
        len = elft_build(img, sizeof(img), ELFT_BASE, &bad, 1, 2);
        if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_EMALFORMED) {
            return -1;
        }
    }

    /* Segment contents past the end of the file. */
    {
        elft_seg_t bad = ok;
        bad.off = 0x400;
        bad.filesz = 0x4000;
        bad.memsz = 0x4000;
        len = elft_build(img, sizeof(img), ELFT_BASE, &bad, 1, 2);
        if (vibeos_elf_parse(img, 0x500, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_ETRUNCATED) {
            return -1;
        }
        (void)len;
    }

    /* Arithmetic that would wrap: vaddr + memsz overflows 64 bits. */
    {
        elft_seg_t bad = ok;
        bad.vaddr = 0xFFFFFFFFFFFFF000ull;
        bad.memsz = 0x2000;
        len = elft_build(img, sizeof(img), ELFT_BASE, &bad, 1, 2);
        if (vibeos_elf_parse(img, len, 0, 0xFFFFFFFFFFFFFFFFull, &out) !=
            VIBEOS_ELF_EMALFORMED) {
            return -1;
        }
    }

    /* Asking to be placed outside the region the caller permits. */
    {
        elft_seg_t bad = ok;
        bad.vaddr = 0x1000;         /* far below the user base */
        len = elft_build(img, sizeof(img), 0x1000, &bad, 1, 2);
        if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_ERANGE) {
            return -1;
        }
    }

    /* An entry point outside every loaded segment. */
    {
        len = elft_build(img, sizeof(img), ELFT_BASE + 0x9000, &ok, 1, 2);
        if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) !=
            VIBEOS_ELF_EMALFORMED) {
            return -1;
        }
    }
    return 0;
}

static int test_elf_bss_is_zeroed(void) {
    static uint8_t img[8192];
    static uint8_t page[VIBEOS_ELF_PAGE_SIZE];
    vibeos_elf_image_t out;
    uint32_t i;
    /* 0x40 bytes in the file, 0x1000 in memory: the rest is .bss and spills
     * into a second page that has no file backing at all. */
    elft_seg_t seg = {1u, VIBEOS_ELF_R | VIBEOS_ELF_W, 0x400, ELFT_BASE, 0x40, 0x1800};
    uint64_t len = elft_build(img, sizeof(img), ELFT_BASE, &seg, 1, 2);

    for (i = 0; i < 0x40; i++) {
        img[0x400 + i] = 0xCD;
    }
    if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_OK) {
        return -1;
    }
    if (out.end_vaddr != ELFT_BASE + 0x2000) {
        return -1;
    }
    vibeos_elf_fill_page(&out, img, ELFT_BASE, page);
    for (i = 0; i < 0x40; i++) {
        if (page[i] != 0xCD) {
            return -1;
        }
    }
    for (i = 0x40; i < VIBEOS_ELF_PAGE_SIZE; i++) {
        if (page[i] != 0) {
            return -1;
        }
    }
    /* The second page is pure .bss: mapped, writable, and entirely zero. */
    if (vibeos_elf_page_flags(&out, ELFT_BASE + 0x1000) !=
        (VIBEOS_ELF_R | VIBEOS_ELF_W)) {
        return -1;
    }
    for (i = 0; i < VIBEOS_ELF_PAGE_SIZE; i++) {
        page[i] = 0xEE;
    }
    vibeos_elf_fill_page(&out, img, ELFT_BASE + 0x1000, page);
    for (i = 0; i < VIBEOS_ELF_PAGE_SIZE; i++) {
        if (page[i] != 0) {
            return -1;
        }
    }
    return 0;
}


/* The startup stack is what a real libc reads before it runs any of the
 * program, so these check the exact layout the System V ABI mandates rather
 * than merely that the builder returned something. */
#define STACKT_TOP 0x0000700000000000ull
#define STACKT_LEN 4096u

static uint8_t stackt_buf[STACKT_LEN];

/* Read the 64-bit word the program would see at virtual address `va`. */
static uint64_t stackt_word(uint64_t va) {
    uint64_t v = 0;
    uint32_t i;
    uint64_t off = va - (STACKT_TOP - STACKT_LEN);
    for (i = 0; i < 8u; i++) {
        v |= (uint64_t)stackt_buf[off + i] << (8u * i);
    }
    return v;
}

static const char *stackt_str(uint64_t va) {
    return (const char *)&stackt_buf[va - (STACKT_TOP - STACKT_LEN)];
}

static int test_elf_stack_layout(void) {
    static const char *const argv[] = {"/bin/sh", "-c", "echo", NULL};
    static const char *const envp[] = {"PATH=/bin", "HOME=/", NULL};
    static const uint8_t rnd[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
    vibeos_elf_stack_desc_t d;
    uint64_t sp, p;
    int i;
    int seen_phdr = 0, seen_phent = 0, seen_phnum = 0;
    int seen_entry = 0, seen_pagesz = 0, seen_random = 0, seen_null = 0;

    memset(stackt_buf, 0xA5, sizeof(stackt_buf));
    memset(&d, 0, sizeof(d));
    d.argv = argv;
    d.envp = envp;
    d.entry = 0x400123ull;
    d.phdr_vaddr = 0x400040ull;
    d.phnum = 3;
    d.phentsize = 56;
    d.random16 = rnd;

    sp = vibeos_elf_build_stack(stackt_buf, STACKT_LEN, STACKT_TOP, &d);
    if (sp == 0) {
        return -1;
    }
    /* Misalignment here is not cosmetic: the first movaps in a real _start
     * faults on it. */
    if ((sp & 0xFu) != 0u) {
        return -1;
    }
    if (sp < STACKT_TOP - STACKT_LEN || sp >= STACKT_TOP) {
        return -1;
    }
    if (stackt_word(sp) != 3u) {
        return -1;
    }
    p = sp + 8u;
    for (i = 0; i < 3; i++) {
        uint64_t ptr = stackt_word(p + (uint64_t)i * 8u);
        if (ptr == 0 || strcmp(stackt_str(ptr), argv[i]) != 0) {
            return -1;
        }
    }
    if (stackt_word(p + 24u) != 0u) {   /* argv NULL terminator */
        return -1;
    }
    p += 32u;
    for (i = 0; i < 2; i++) {
        uint64_t ptr = stackt_word(p + (uint64_t)i * 8u);
        if (ptr == 0 || strcmp(stackt_str(ptr), envp[i]) != 0) {
            return -1;
        }
    }
    if (stackt_word(p + 16u) != 0u) {   /* envp NULL terminator */
        return -1;
    }
    p += 24u;

    for (i = 0; i < 32; i++) {
        uint64_t key = stackt_word(p);
        uint64_t val = stackt_word(p + 8u);
        p += 16u;
        if (key == VIBEOS_AT_PHDR)   { seen_phdr   = (val == 0x400040ull); }
        if (key == VIBEOS_AT_PHENT)  { seen_phent  = (val == 56u); }
        if (key == VIBEOS_AT_PHNUM)  { seen_phnum  = (val == 3u); }
        if (key == VIBEOS_AT_ENTRY)  { seen_entry  = (val == 0x400123ull); }
        if (key == VIBEOS_AT_PAGESZ) { seen_pagesz = (val == VIBEOS_ELF_PAGE_SIZE); }
        if (key == VIBEOS_AT_RANDOM) {
            const uint8_t *r = (const uint8_t *)stackt_str(val);
            int k, ok = 1;
            for (k = 0; k < 16; k++) {
                if (r[k] != rnd[k]) { ok = 0; }
            }
            seen_random = ok;
        }
        if (key == VIBEOS_AT_NULL) { seen_null = 1; break; }
    }
    if (!seen_phdr || !seen_phent || !seen_phnum || !seen_entry ||
        !seen_pagesz || !seen_random || !seen_null) {
        return -1;
    }
    return 0;
}

static int test_elf_stack_edges(void) {
    static const char *const argv[] = {"a", NULL};
    vibeos_elf_stack_desc_t d;
    static char big[3000];
    static const char *bigv[2];
    uint64_t sp;

    memset(&d, 0, sizeof(d));
    /* No argv, no envp, no auxv extras: still a valid stack with argc == 0. */
    sp = vibeos_elf_build_stack(stackt_buf, STACKT_LEN, STACKT_TOP, &d);
    if (sp == 0 || (sp & 0xFu) != 0u || stackt_word(sp) != 0u) {
        return -1;
    }
    if (stackt_word(sp + 8u) != 0u || stackt_word(sp + 16u) != 0u) {
        return -1;   /* empty argv and envp terminators */
    }

    /* A stack top that is not 16-byte aligned cannot produce an aligned sp,
     * so it is refused rather than silently rounded. */
    d.argv = argv;
    if (vibeos_elf_build_stack(stackt_buf, STACKT_LEN, STACKT_TOP + 8u, &d) != 0) {
        return -1;
    }
    if (vibeos_elf_build_stack(NULL, STACKT_LEN, STACKT_TOP, &d) != 0) {
        return -1;
    }
    if (vibeos_elf_build_stack(stackt_buf, 0, STACKT_TOP, &d) != 0) {
        return -1;
    }

    /* Arguments larger than the stack are refused, not written past the end. */
    memset(big, 'x', sizeof(big) - 1u);
    big[sizeof(big) - 1u] = 0;
    bigv[0] = big;
    bigv[1] = NULL;
    d.argv = (const char *const *)bigv;
    if (vibeos_elf_build_stack(stackt_buf, 2048u, STACKT_TOP, &d) != 0) {
        return -1;
    }
    return 0;
}

static int test_elf_stack_carries_phdr(void) {
    static uint8_t img[8192];
    vibeos_elf_image_t out;
    vibeos_elf_stack_desc_t d;
    /* A segment starting at file offset 0 maps the ELF header and the program
     * headers along with the code, which is what makes AT_PHDR answerable. */
    elft_seg_t seg = {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0, ELFT_BASE, 0x800, 0x800};
    uint64_t len = elft_build(img, sizeof(img), ELFT_BASE, &seg, 1, 2);
    uint64_t sp, p, key, val;
    uint64_t got_phdr = 0;
    int i;

    if (vibeos_elf_parse(img, len, ELFT_BASE, ELFT_LIMIT, &out) != VIBEOS_ELF_OK) {
        return -1;
    }
    if (out.phdr_vaddr == 0u || out.phnum == 0u) {
        return -1;   /* headers are inside the segment; they must be reported */
    }
    memset(&d, 0, sizeof(d));
    d.entry = out.entry;
    d.phdr_vaddr = out.phdr_vaddr;
    d.phnum = out.phnum;
    d.phentsize = out.phentsize;

    sp = vibeos_elf_build_stack(stackt_buf, STACKT_LEN, STACKT_TOP, &d);
    if (sp == 0) {
        return -1;
    }
    p = sp + 8u + 8u + 8u;   /* argc, argv NULL, envp NULL */
    for (i = 0; i < 32; i++) {
        key = stackt_word(p);
        val = stackt_word(p + 8u);
        p += 16u;
        if (key == VIBEOS_AT_PHDR) {
            got_phdr = val;
        }
        if (key == VIBEOS_AT_NULL) {
            break;
        }
    }
    /* The address handed to the program must be the one the parser derived,
     * and it must fall inside the image that was mapped. */
    if (got_phdr != out.phdr_vaddr) {
        return -1;
    }
    if (got_phdr < out.min_vaddr || got_phdr >= out.end_vaddr) {
        return -1;
    }
    return 0;
}

/* ---- ET_DYN, PT_INTERP and AT_BASE ---------------------------------------
 *
 * A dynamically linked executable differs from a static one in three ways the
 * portable parser has to cope with: its addresses are relative to zero and the
 * kernel picks where they land, it names an interpreter to run instead of
 * itself, and that interpreter needs its own load address handed to it in the
 * auxiliary vector. Each of those is checked here rather than only on metal,
 * because each is arithmetic on untrusted input.
 */

#define ELFT_DYN_BIAS (ELFT_BASE + 0x20000ull)
#define ELFT_INTERP_PATH "/lib64/ld-linux-x86-64.so.2"

/* Drop a NUL-terminated string into the image at `off`, as the linker would. */
static void elft_put_str(uint8_t *buf, uint64_t off, const char *s) {
    uint64_t i = 0;
    do {
        buf[off + i] = (uint8_t)s[i];
    } while (s[i++]);
}

/* A PIE placed at a caller-chosen bias: everything reported must already
 * include it, because the caller maps what it is told and never adds the bias
 * a second time. */
static int test_elf_parse_dyn_bias(void) {
    static uint8_t img[8192];
    static uint8_t page[VIBEOS_ELF_PAGE_SIZE];
    vibeos_elf_image_t out, sized;
    uint32_t i;
    /* Zero-based, as a PIE's program headers really are. The first segment
     * starts at file offset 0 so it maps the program headers too. */
    elft_seg_t segs[2] = {
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0, 0, 0x800, 0x800},
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_W, 0x1000, 0x1000, 0x40, 0x200},
    };
    uint64_t len = elft_build(img, sizeof(img), 0x10, segs, 2, 3 /* ET_DYN */);

    img[0x100] = 0x5A;   /* a byte to prove the file offsets still line up */

    /* Without being asked to, the parser must still refuse: a caller that
     * cannot relocate would map these zero-based addresses literally. */
    if (vibeos_elf_parse(img, len, 0, ELFT_LIMIT, &out) != VIBEOS_ELF_EDYNAMIC) {
        return -1;
    }
    if (vibeos_elf_parse_ex(img, len, 0, 0, ELFT_LIMIT, 0, &out) !=
        VIBEOS_ELF_EDYNAMIC) {
        return -1;
    }

    /* The sizing pass: bias 0 is how a caller learns how much space to
     * reserve before it can choose where to put the image. */
    if (vibeos_elf_parse_ex(img, len, 0, 0, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN, &sized) != VIBEOS_ELF_OK) {
        return -1;
    }
    if (!sized.is_dyn || sized.load_bias != 0 || sized.image_span != 0x2000ull) {
        return -1;
    }
    if (sized.min_vaddr != 0 || sized.end_vaddr != 0x2000ull) {
        return -1;
    }

    /* And the placing pass. */
    if (vibeos_elf_parse_ex(img, len, ELFT_DYN_BIAS, ELFT_BASE, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN, &out) != VIBEOS_ELF_OK) {
        return -1;
    }
    if (!out.is_dyn || out.has_interp || out.load_bias != ELFT_DYN_BIAS) {
        return -1;
    }
    /* The span does not depend on the bias; if it did, a caller could not
     * reserve the right amount of space before choosing one. */
    if (out.image_span != sized.image_span) {
        return -1;
    }
    if (out.entry != ELFT_DYN_BIAS + 0x10ull) {
        return -1;
    }
    if (out.min_vaddr != ELFT_DYN_BIAS ||
        out.end_vaddr != ELFT_DYN_BIAS + 0x2000ull) {
        return -1;
    }
    if (out.count != 2 || out.seg[0].vaddr != ELFT_DYN_BIAS ||
        out.seg[1].vaddr != ELFT_DYN_BIAS + 0x1000ull) {
        return -1;
    }
    /* AT_PHDR after the bias, not before: an interpreter told the file-relative
     * address would read the program headers out of whatever is at 64. */
    if (out.phdr_vaddr != ELFT_DYN_BIAS + 64ull) {
        return -1;
    }
    /* Permissions and file bytes must still resolve at the biased addresses,
     * which is the whole point of reporting them biased. */
    if (vibeos_elf_page_flags(&out, ELFT_DYN_BIAS) !=
        (VIBEOS_ELF_R | VIBEOS_ELF_X)) {
        return -1;
    }
    if (vibeos_elf_page_flags(&out, ELFT_DYN_BIAS + 0x1000ull) !=
        (VIBEOS_ELF_R | VIBEOS_ELF_W)) {
        return -1;
    }
    if (vibeos_elf_page_flags(&out, 0) != 0) {
        return -1;   /* nothing is left behind at the unbiased address */
    }
    memset(page, 0xEE, sizeof(page));
    vibeos_elf_fill_page(&out, img, ELFT_DYN_BIAS, page);
    if (page[0x100] != 0x5A || page[0] != 0x7F) {
        return -1;
    }
    for (i = 0x800; i < VIBEOS_ELF_PAGE_SIZE; i++) {
        if (page[i] != 0) {
            return -1;   /* past filesz: padding, and it must be zeroed */
        }
    }
    return 0;
}

/* The bias is arithmetic on an address the file supplied, so every way it can
 * go wrong has to end in a refusal rather than a wrong mapping. */
static int test_elf_dyn_rejects_crafted(void) {
    static uint8_t img[8192];
    vibeos_elf_image_t out;
    uint64_t len;
    elft_seg_t dyn[2] = {
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0, 0, 0x800, 0x800},
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_W, 0x1000, 0x1000, 0x40, 0x200},
    };
    elft_seg_t exec = {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0x400, ELFT_BASE, 0x100, 0x100};

    /* A sub-page bias would leave every segment straddling a page boundary the
     * caller maps whole. */
    len = elft_build(img, sizeof(img), 0x10, dyn, 2, 3);
    if (vibeos_elf_parse_ex(img, len, ELFT_BASE + 0x800ull, ELFT_BASE, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN, &out) != VIBEOS_ELF_EMALFORMED) {
        return -1;
    }

    /* Biased past the range the caller permits. The check has to happen on the
     * biased address; on the file's own it would pass. */
    len = elft_build(img, sizeof(img), 0x10, dyn, 2, 3);
    if (vibeos_elf_parse_ex(img, len, ELFT_BASE + 0x3FF000ull, ELFT_BASE,
                            ELFT_LIMIT, VIBEOS_ELF_ALLOW_DYN, &out) !=
        VIBEOS_ELF_ERANGE) {
        return -1;
    }

    /* vaddr + bias wrapping 64 bits. Only the second segment wraps, and the
     * entry point does not, so the refusal has to come from the segment's own
     * arithmetic - left unchecked, the wrapped address lands back at the
     * bottom of memory and looks perfectly reasonable. */
    {
        elft_seg_t wrap[2] = {
            {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0x400, 0x1000, 0x100, 0x400},
            {1u, VIBEOS_ELF_R | VIBEOS_ELF_W, 0x600, 0x100000000ull, 0x100, 0x100},
        };
        /* The bias is chosen so the surviving segment stays well clear of the
         * top of memory: otherwise page_up rejects the image for its own
         * reasons and the segment check is never the thing under test. */
        len = elft_build(img, sizeof(img), 0x1000, wrap, 2, 3);
        if (vibeos_elf_parse_ex(img, len, 0xFFFFFFFF00000000ull, 0,
                                0xFFFFFFFFFFFFFFFFull, VIBEOS_ELF_ALLOW_DYN,
                                &out) != VIBEOS_ELF_EMALFORMED) {
            return -1;
        }
    }

    /* ET_EXEC is not relocatable: its addresses are absolute and biasing them
     * moves the pages out from under the program's own references. */
    len = elft_build(img, sizeof(img), ELFT_BASE, &exec, 1, 2);
    if (vibeos_elf_parse_ex(img, len, 0x1000, ELFT_BASE, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN, &out) != VIBEOS_ELF_EMALFORMED) {
        return -1;
    }
    /* ...but the same file with no bias is exactly what it was before. */
    len = elft_build(img, sizeof(img), ELFT_BASE, &exec, 1, 2);
    if (vibeos_elf_parse_ex(img, len, 0, ELFT_BASE, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN, &out) != VIBEOS_ELF_OK) {
        return -1;
    }
    if (out.is_dyn || out.load_bias != 0 || out.entry != ELFT_BASE) {
        return -1;
    }
    return 0;
}

/* PT_INTERP names the file the kernel must run instead of this one, so the
 * path is copied out whole or the file is refused - never truncated, because a
 * truncated path names a different file. */
static int test_elf_interp_reported(void) {
    static uint8_t img[8192];
    vibeos_elf_image_t out;
    uint64_t len;
    const uint64_t plen = sizeof(ELFT_INTERP_PATH);   /* includes the NUL */
    elft_seg_t segs[2] = {
        {1u, VIBEOS_ELF_R | VIBEOS_ELF_X, 0, 0, 0x800, 0x800},
        {3u /* PT_INTERP */, VIBEOS_ELF_R, 0x900, 0, 0, 0},
    };
    elft_seg_t three[3];

    segs[1].filesz = plen;
    segs[1].memsz = plen;

    /* Not asked for: still refused, so a caller that cannot load an
     * interpreter cannot be handed a file that needs one. */
    len = elft_build(img, sizeof(img), 0x10, segs, 2, 3);
    elft_put_str(img, 0x900, ELFT_INTERP_PATH);
    if (vibeos_elf_parse_ex(img, len, 0, 0, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN, &out) != VIBEOS_ELF_EDYNAMIC) {
        return -1;
    }

    /* Asked for: reported, alongside a perfectly normal biased image. */
    len = elft_build(img, sizeof(img), 0x10, segs, 2, 3);
    elft_put_str(img, 0x900, ELFT_INTERP_PATH);
    if (vibeos_elf_parse_ex(img, len, ELFT_DYN_BIAS, ELFT_BASE, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN | VIBEOS_ELF_ALLOW_INTERP,
                            &out) != VIBEOS_ELF_OK) {
        return -1;
    }
    if (!out.has_interp || strcmp(out.interp, ELFT_INTERP_PATH) != 0) {
        return -1;
    }
    /* PT_INTERP is not loadable, so it must not have become a segment or
     * stretched the span the caller reserves. */
    if (out.count != 1 || out.end_vaddr != ELFT_DYN_BIAS + 0x1000ull) {
        return -1;
    }

    /* A path the file never terminated. Terminating it here would turn a
     * malformed file into a request to open some prefix of a path. */
    segs[1].filesz = plen - 1u;
    len = elft_build(img, sizeof(img), 0x10, segs, 2, 3);
    elft_put_str(img, 0x900, ELFT_INTERP_PATH);
    if (vibeos_elf_parse_ex(img, len, 0, 0, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN | VIBEOS_ELF_ALLOW_INTERP,
                            &out) != VIBEOS_ELF_EMALFORMED) {
        return -1;
    }

    /* Longer than the buffer that holds it. */
    segs[1].filesz = VIBEOS_ELF_MAX_INTERP + 1u;
    segs[1].memsz = segs[1].filesz;
    len = elft_build(img, sizeof(img), 0x10, segs, 2, 3);
    memset(img + 0x900, 'a', (size_t)segs[1].filesz - 1u);
    img[0x900 + segs[1].filesz - 1u] = 0;
    if (vibeos_elf_parse_ex(img, len, 0, 0, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN | VIBEOS_ELF_ALLOW_INTERP,
                            &out) != VIBEOS_ELF_EMALFORMED) {
        return -1;
    }

    /* Empty. */
    segs[1].filesz = 0;
    segs[1].memsz = 0;
    len = elft_build(img, sizeof(img), 0x10, segs, 2, 3);
    if (vibeos_elf_parse_ex(img, len, 0, 0, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN | VIBEOS_ELF_ALLOW_INTERP,
                            &out) != VIBEOS_ELF_EMALFORMED) {
        return -1;
    }

    /* Running off the end of the file: read as-is this would walk past the
     * image the caller actually holds. */
    segs[1].filesz = plen;
    segs[1].memsz = plen;
    len = elft_build(img, sizeof(img), 0x10, segs, 2, 3);
    elft_put_str(img, 0x900, ELFT_INTERP_PATH);
    if (vibeos_elf_parse_ex(img, 0x905, 0, 0, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN | VIBEOS_ELF_ALLOW_INTERP,
                            &out) != VIBEOS_ELF_ETRUNCATED) {
        return -1;
    }

    /* Two interpreters is not a choice to make on the file's behalf. */
    three[0] = segs[0];
    three[1] = segs[1];
    three[2] = segs[1];
    three[2].off = 0xA00;
    len = elft_build(img, sizeof(img), 0x10, three, 3, 3);
    elft_put_str(img, 0x900, ELFT_INTERP_PATH);
    elft_put_str(img, 0xA00, ELFT_INTERP_PATH);
    if (vibeos_elf_parse_ex(img, len, 0, 0, ELFT_LIMIT,
                            VIBEOS_ELF_ALLOW_DYN | VIBEOS_ELF_ALLOW_INTERP,
                            &out) != VIBEOS_ELF_EMALFORMED) {
        return -1;
    }
    return 0;
}

/* AT_BASE is where the interpreter relocates itself from. Without it a
 * dynamic program faults on its very first relocation. */
static int test_elf_stack_at_base(void) {
    static const char *const argv[] = {"/bin/true", NULL};
    vibeos_elf_stack_desc_t d;
    uint64_t sp, p;
    int i;
    int seen_base = 0, seen_null = 0;

    memset(&d, 0, sizeof(d));
    d.argv = argv;
    d.entry = 0x400123ull;
    d.interp_base = 0x7FFFF7000000ull;

    sp = vibeos_elf_build_stack(stackt_buf, STACKT_LEN, STACKT_TOP, &d);
    if (sp == 0 || (sp & 0xFu) != 0u) {
        return -1;
    }
    /* The extra pair has to have been budgeted for, not just written: an
     * auxv one pair longer than the space reserved for it runs straight into
     * the argument strings sitting above. */
    if (strcmp(stackt_str(stackt_word(sp + 8u)), argv[0]) != 0) {
        return -1;
    }
    p = sp + 8u + 16u + 8u;   /* argc, argv[0] + NULL, envp NULL */
    for (i = 0; i < 32; i++) {
        uint64_t key = stackt_word(p);
        uint64_t val = stackt_word(p + 8u);
        p += 16u;
        if (key == VIBEOS_AT_BASE) {
            seen_base = (val == 0x7FFFF7000000ull);
        }
        if (key == VIBEOS_AT_NULL) {
            seen_null = 1;
            break;
        }
    }
    if (!seen_base || !seen_null) {
        return -1;
    }

    /* With no interpreter it must be absent, not zero: 0 is a legal load
     * address and a libc would believe it. */
    d.interp_base = 0;
    sp = vibeos_elf_build_stack(stackt_buf, STACKT_LEN, STACKT_TOP, &d);
    if (sp == 0) {
        return -1;
    }
    p = sp + 8u + 16u + 8u;
    for (i = 0; i < 32; i++) {
        uint64_t key = stackt_word(p);
        p += 16u;
        if (key == VIBEOS_AT_BASE) {
            return -1;
        }
        if (key == VIBEOS_AT_NULL) {
            return 0;
        }
    }
    return -1;   /* never terminated */
}

/* Reserving a physical range is what stops a kernel object from living at an
 * address a process can shadow, so the arithmetic is worth checking directly
 * rather than inferring it from a boot that happened not to crash. */
static int test_pmm_reserve(void) {
    vibeos_pmm_t pmm;
    void *a;

    /* A reservation covering the front of the region moves the base past it. */
    if (vibeos_pmm_init(&pmm, 0x100000u, 0x100000u, 4096u) != 0) {
        return -1;
    }
    if (vibeos_pmm_reserve(&pmm, 0x100000u, 0x10000u) != 0) {
        return -1;
    }
    if (pmm.base != 0x110000u || pmm.size_bytes != 0xF0000u) {
        return -1;
    }
    a = vibeos_pmm_alloc_page(&pmm);
    if ((uintptr_t)a < 0x110000u) {
        return -1;   /* handed out a reserved page */
    }

    /* Covering the tail shrinks the region instead. */
    if (vibeos_pmm_init(&pmm, 0x100000u, 0x100000u, 4096u) != 0) {
        return -1;
    }
    if (vibeos_pmm_reserve(&pmm, 0x1F0000u, 0x20000u) != 0) {
        return -1;
    }
    if (pmm.base != 0x100000u || pmm.size_bytes != 0xF0000u) {
        return -1;
    }

    /* A range that splits the region keeps the larger side; the allocator is a
     * bump allocator over one range and cannot hold a hole. */
    if (vibeos_pmm_init(&pmm, 0x100000u, 0x100000u, 4096u) != 0) {
        return -1;
    }
    if (vibeos_pmm_reserve(&pmm, 0x120000u, 0x1000u) != 0) {
        return -1;
    }
    if (pmm.base != 0x121000u || pmm.size_bytes != 0xDF000u) {
        return -1;   /* the suffix is larger than the 0x20000 prefix */
    }

    /* No overlap at all is success and changes nothing. */
    if (vibeos_pmm_init(&pmm, 0x100000u, 0x100000u, 4096u) != 0) {
        return -1;
    }
    if (vibeos_pmm_reserve(&pmm, 0x400000u, 0x800000u) != 0) {
        return -1;
    }
    if (pmm.base != 0x100000u || pmm.size_bytes != 0x100000u) {
        return -1;
    }

    /* A reservation swallowing the whole region has to fail rather than leave
     * an allocator with nothing in it. */
    if (vibeos_pmm_init(&pmm, 0x100000u, 0x10000u, 4096u) != 0) {
        return -1;
    }
    if (vibeos_pmm_reserve(&pmm, 0x100000u, 0x10000u) == 0) {
        return -1;
    }

    /* Reserving after something has been handed out would be a lie: that page
     * may already be inside the range. */
    if (vibeos_pmm_init(&pmm, 0x100000u, 0x100000u, 4096u) != 0) {
        return -1;
    }
    if (vibeos_pmm_alloc_page(&pmm) == 0) {
        return -1;
    }
    if (vibeos_pmm_reserve(&pmm, 0x180000u, 0x1000u) == 0) {
        return -1;
    }
    return 0;
}

/* ---- block cache ---------------------------------------------------------
 *
 * Driven against an array rather than a disk, which is the point of the cache
 * living in kernel/fs: eviction order and write-back timing are exactly the
 * things that are invisible in a boot and obvious in a sweep.
 */
#define BCT_SECTORS 64u

typedef struct {
    uint8_t disk[BCT_SECTORS][VIBEOS_BLOCK_SIZE];
    uint32_t reads;
    uint32_t writes;
    int fail_writes;
    int fail_reads;
} bct_disk_t;

static bct_disk_t g_bct;

static int bct_read(void *ctx, uint64_t lba, void *buf) {
    bct_disk_t *d = (bct_disk_t *)ctx;
    if (d->fail_reads || lba >= BCT_SECTORS) {
        return -1;
    }
    memcpy(buf, d->disk[lba], VIBEOS_BLOCK_SIZE);
    d->reads++;
    return 0;
}

static int bct_write(void *ctx, uint64_t lba, const void *buf) {
    bct_disk_t *d = (bct_disk_t *)ctx;
    if (d->fail_writes || lba >= BCT_SECTORS) {
        return -1;
    }
    memcpy(d->disk[lba], buf, VIBEOS_BLOCK_SIZE);
    d->writes++;
    return 0;
}

static void bct_reset(void) {
    uint32_t i, j;
    memset(&g_bct, 0, sizeof(g_bct));
    for (i = 0; i < BCT_SECTORS; i++) {
        for (j = 0; j < VIBEOS_BLOCK_SIZE; j++) {
            g_bct.disk[i][j] = (uint8_t)(i * 7u + j);
        }
    }
}

#define BCT_SLOTS 4u
static uint8_t g_bct_storage[BCT_SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_bct_slots[BCT_SLOTS];

static void bct_setup(vibeos_blockcache_t *bc, vibeos_blockdev_t *dev, int writable) {
    uint32_t i;
    bct_reset();
    for (i = 0; i < BCT_SLOTS; i++) {
        g_bct_slots[i].data = g_bct_storage[i];
    }
    memset(dev, 0, sizeof(*dev));
    dev->read = bct_read;
    dev->write = writable ? bct_write : 0;
    dev->ctx = &g_bct;
    dev->sectors = BCT_SECTORS;
    (void)vibeos_blockcache_init(bc, dev, g_bct_slots, BCT_SLOTS);
}

static int test_blockcache_hits(void) {
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    uint8_t buf[VIBEOS_BLOCK_SIZE];

    bct_setup(&bc, &dev, 1);
    if (vibeos_blockcache_read(&bc, 5, buf) != 0 || buf[0] != (uint8_t)(5u * 7u)) {
        return -1;
    }
    if (g_bct.reads != 1u) {
        return -1;
    }
    /* The second read of the same block must not reach the device - that is
     * the entire reason the cache exists. */
    if (vibeos_blockcache_read(&bc, 5, buf) != 0 || g_bct.reads != 1u) {
        return -1;
    }
    if (bc.hits != 1u || bc.misses != 1u) {
        return -1;
    }
    /* Out of range is refused rather than passed to the device. */
    if (vibeos_blockcache_read(&bc, BCT_SECTORS, buf) == 0) {
        return -1;
    }
    return 0;
}

static int test_blockcache_writeback(void) {
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    uint8_t buf[VIBEOS_BLOCK_SIZE];
    uint32_t i;

    bct_setup(&bc, &dev, 1);
    for (i = 0; i < VIBEOS_BLOCK_SIZE; i++) {
        buf[i] = 0xAB;
    }
    if (vibeos_blockcache_write(&bc, 9, buf) != 0) {
        return -1;
    }
    /* Write-back: the device must NOT have been touched yet. A cache that
     * writes through is a cache that cannot order anything, which is the
     * property a journal will need. */
    if (g_bct.writes != 0u || g_bct.disk[9][0] == 0xAB) {
        return -1;
    }
    /* Reading it back comes from the cache and sees the new bytes. */
    memset(buf, 0, sizeof(buf));
    if (vibeos_blockcache_read(&bc, 9, buf) != 0 || buf[0] != 0xAB) {
        return -1;
    }
    if (vibeos_blockcache_flush(&bc) != 0) {
        return -1;
    }
    if (g_bct.writes != 1u || g_bct.disk[9][0] != 0xAB) {
        return -1;
    }
    /* Flushing twice must not write twice: the block is no longer dirty. */
    if (vibeos_blockcache_flush(&bc) != 0 || g_bct.writes != 1u) {
        return -1;
    }
    return 0;
}

static int test_blockcache_eviction(void) {
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    uint8_t buf[VIBEOS_BLOCK_SIZE];
    uint32_t i;

    bct_setup(&bc, &dev, 1);
    /* Dirty one block, then push it out by touching more blocks than there
     * are slots. Eviction must write it, not drop it - dropping would discard
     * a write the caller was told had succeeded. */
    for (i = 0; i < VIBEOS_BLOCK_SIZE; i++) {
        buf[i] = 0x5C;
    }
    if (vibeos_blockcache_write(&bc, 1, buf) != 0) {
        return -1;
    }
    for (i = 2; i < 2u + BCT_SLOTS; i++) {
        if (vibeos_blockcache_read(&bc, i, buf) != 0) {
            return -1;
        }
    }
    if (g_bct.disk[1][0] != 0x5C || bc.writebacks != 1u) {
        return -1;
    }
    /* And the evicted block really left the cache: reading it again costs a
     * device read. */
    {
        uint32_t before = g_bct.reads;
        if (vibeos_blockcache_read(&bc, 1, buf) != 0 || g_bct.reads != before + 1u) {
            return -1;
        }
        if (buf[0] != 0x5C) {
            return -1;   /* what came back must be what was written */
        }
    }
    return 0;
}

static int test_blockcache_failures(void) {
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    uint8_t buf[VIBEOS_BLOCK_SIZE];

    /* A read-only device refuses writes rather than accepting them and
     * failing later at flush, when the caller has moved on. */
    bct_setup(&bc, &dev, 0);
    if (vibeos_blockcache_write(&bc, 3, buf) == 0) {
        return -1;
    }

    /* A device that fails must produce a failed read, not a stale slot that
     * later looks valid. */
    bct_setup(&bc, &dev, 1);
    g_bct.fail_reads = 1;
    if (vibeos_blockcache_read(&bc, 4, buf) == 0) {
        return -1;
    }
    g_bct.fail_reads = 0;
    if (vibeos_blockcache_read(&bc, 4, buf) != 0 || buf[0] != (uint8_t)(4u * 7u)) {
        return -1;
    }

    /* A flush that cannot write must say so. Reporting success would let a
     * journal trust an ordering that never reached the disk. */
    bct_setup(&bc, &dev, 1);
    memset(buf, 0x11, sizeof(buf));
    if (vibeos_blockcache_write(&bc, 6, buf) != 0) {
        return -1;
    }
    g_bct.fail_writes = 1;
    if (vibeos_blockcache_flush(&bc) == 0) {
        return -1;
    }
    /* Still dirty, so a later successful flush still writes it. */
    g_bct.fail_writes = 0;
    if (vibeos_blockcache_flush(&bc) != 0 || g_bct.disk[6][0] != 0x11) {
        return -1;
    }

    /* Invalidate drops dirty data on purpose; the caller flushes first if it
     * wanted it kept. */
    bct_setup(&bc, &dev, 1);
    memset(buf, 0x22, sizeof(buf));
    if (vibeos_blockcache_write(&bc, 7, buf) != 0) {
        return -1;
    }
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_blockcache_flush(&bc) != 0 || g_bct.writes != 0u) {
        return -1;
    }
    return 0;
}

/* ---- partition tables ----------------------------------------------------
 *
 * Fabricated tables rather than a disk image: every case below is a byte
 * layout, and a boot can only tell you that one particular real table worked.
 */
static uint8_t g_pt_sector[512];
static uint8_t g_pt_entries[128 * 4];

static void pt_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void pt_wr64(uint8_t *p, uint64_t v) {
    pt_wr32(p, (uint32_t)v);
    pt_wr32(p + 4, (uint32_t)(v >> 32));
}

static void pt_mbr_entry(uint32_t idx, uint8_t type, uint32_t first, uint32_t count) {
    uint8_t *e = g_pt_sector + 446u + idx * 16u;
    memset(e, 0, 16);
    e[4] = type;
    pt_wr32(e + 8, first);
    pt_wr32(e + 12, count);
}

static int test_partition_mbr(void) {
    vibeos_parttable_t tbl;
    int protective = 0;

    memset(g_pt_sector, 0, sizeof(g_pt_sector));
    /* No signature: not a table, and saying so beats reporting zero
     * partitions, which is what an empty-but-valid table looks like. */
    if (vibeos_partition_parse_mbr(g_pt_sector, &tbl, &protective) == 0) {
        return -1;
    }
    g_pt_sector[510] = 0x55; g_pt_sector[511] = 0xAA;

    pt_mbr_entry(0, 0x0Cu, 2048u, 100000u);   /* FAT32 LBA */
    pt_mbr_entry(1, 0x83u, 200000u, 50000u);  /* Linux */
    pt_mbr_entry(2, 0x00u, 0u, 0u);           /* empty slot in the middle */
    pt_mbr_entry(3, 0x0Cu, 300000u, 0u);      /* zero length: not a partition */

    if (vibeos_partition_parse_mbr(g_pt_sector, &tbl, &protective) != 0) {
        return -1;
    }
    if (tbl.count != 2u || protective != 0 || tbl.is_gpt != 0) {
        return -1;
    }
    if (tbl.entry[0].first_lba != 2048u || tbl.entry[0].sector_count != 100000u) {
        return -1;
    }
    if (tbl.entry[0].kind != VIBEOS_PART_FAT || tbl.entry[0].mbr_type != 0x0Cu) {
        return -1;
    }
    if (tbl.entry[1].kind != VIBEOS_PART_LINUX) {
        return -1;
    }

    /* A protective entry is reported as such and not as a partition -
     * mounting it would hand a filesystem the whole disk including the GPT. */
    memset(g_pt_sector + 446, 0, 64);
    g_pt_sector[510] = 0x55; g_pt_sector[511] = 0xAA;
    pt_mbr_entry(0, 0xEEu, 1u, 0xFFFFFFFFu);
    if (vibeos_partition_parse_mbr(g_pt_sector, &tbl, &protective) != 0) {
        return -1;
    }
    if (tbl.count != 0u || protective != 1) {
        return -1;
    }
    return 0;
}

/* Build a GPT header and entry array that check out, so a test can then break
 * exactly one thing and see it refused. */
static void pt_build_gpt(uint32_t entry_count, uint32_t entry_size) {
    uint8_t *h = g_pt_sector;
    uint32_t crc;

    memset(g_pt_sector, 0, sizeof(g_pt_sector));
    memset(g_pt_entries, 0, sizeof(g_pt_entries));
    memcpy(h, "EFI PART", 8);
    pt_wr32(h + 12, 92u);           /* header size */
    pt_wr64(h + 40, 34u);           /* first usable */
    pt_wr64(h + 48, 1000000u);      /* last usable  */
    pt_wr32(h + 80, entry_count);
    pt_wr32(h + 84, entry_size);

    /* One EFI system partition, one Linux one, named. */
    memcpy(g_pt_entries, "\x28\x73\x2A\xC1\x1F\xF8\xD2\x11\xBA\x4B\x00\xA0\xC9\x3E\xC9\x3B", 16);
    pt_wr64(g_pt_entries + 32, 2048u);
    pt_wr64(g_pt_entries + 40, 4095u);
    g_pt_entries[56] = 'E'; g_pt_entries[58] = 'S'; g_pt_entries[60] = 'P';

    memcpy(g_pt_entries + entry_size,
           "\xAF\x3D\xC6\x0F\x83\x84\x72\x47\x8E\x79\x3D\x69\xD8\x47\x7D\xE4", 16);
    pt_wr64(g_pt_entries + entry_size + 32, 4096u);
    pt_wr64(g_pt_entries + entry_size + 40, 8191u);

    pt_wr32(h + 88, vibeos_partition_crc32(g_pt_entries, entry_count * entry_size));
    pt_wr32(h + 16, 0);
    crc = vibeos_partition_crc32(h, 92u);
    pt_wr32(h + 16, crc);
}

static int test_partition_gpt(void) {
    vibeos_parttable_t tbl;

    pt_build_gpt(4u, 128u);
    if (vibeos_partition_parse_gpt(g_pt_sector, g_pt_entries, sizeof(g_pt_entries),
                                   1000000u, &tbl) != 0) {
        return -1;
    }
    if (!tbl.is_gpt || tbl.count != 2u) {
        return -1;
    }
    if (tbl.entry[0].first_lba != 2048u || tbl.entry[0].sector_count != 2048u) {
        return -1;
    }
    if (tbl.entry[0].kind != VIBEOS_PART_EFI_SYSTEM) {
        return -1;
    }
    if (strcmp(tbl.entry[0].name, "ESP") != 0) {
        return -1;
    }
    if (tbl.entry[1].kind != VIBEOS_PART_LINUX) {
        return -1;
    }
    return 0;
}

static int test_partition_gpt_refusals(void) {
    vibeos_parttable_t tbl;

    /* A corrupt header CRC. A table that fails its own checksum says where
     * other people's data begins, so using it cautiously is not an option. */
    pt_build_gpt(4u, 128u);
    g_pt_sector[20] ^= 0xFFu;
    if (vibeos_partition_parse_gpt(g_pt_sector, g_pt_entries, sizeof(g_pt_entries),
                                   1000000u, &tbl) == 0) {
        return -1;
    }

    /* A corrupt entry array, with the header still intact.
     *
     * The byte flipped is an attribute flag, chosen because nothing else in
     * the parser looks at it: corrupting a boundary field instead would be
     * refused for being an impossible partition, and the test would pass while
     * proving nothing about the checksum it names. */
    pt_build_gpt(4u, 128u);
    g_pt_entries[48] ^= 0xFFu;
    if (vibeos_partition_parse_gpt(g_pt_sector, g_pt_entries, sizeof(g_pt_entries),
                                   1000000u, &tbl) == 0) {
        return -1;
    }

    /* A header claiming more entries than the caller supplied bytes for.
     *
     * Everything stays consistent - the CRCs are correct over four entries -
     * and only the buffer handed in is short. Inflating the count instead
     * would make the checksum fail first, so the bounds check would never be
     * the reason for the refusal. */
    pt_build_gpt(4u, 128u);
    if (vibeos_partition_parse_gpt(g_pt_sector, g_pt_entries, 2u * 128u,
                                   1000000u, &tbl) == 0) {
        return -1;
    }

    /* A partition running off the end of the disk. */
    pt_build_gpt(4u, 128u);
    if (vibeos_partition_parse_gpt(g_pt_sector, g_pt_entries, sizeof(g_pt_entries),
                                   4000u, &tbl) == 0) {
        return -1;
    }

    /* A partition that ends before it begins. */
    pt_build_gpt(4u, 128u);
    pt_wr64(g_pt_entries + 40, 100u);
    pt_wr32(g_pt_sector + 88, vibeos_partition_crc32(g_pt_entries, 4u * 128u));
    pt_wr32(g_pt_sector + 16, 0);
    pt_wr32(g_pt_sector + 16, vibeos_partition_crc32(g_pt_sector, 92u));
    if (vibeos_partition_parse_gpt(g_pt_sector, g_pt_entries, sizeof(g_pt_entries),
                                   1000000u, &tbl) == 0) {
        return -1;
    }

    /* Not a GPT at all. */
    memset(g_pt_sector, 0, sizeof(g_pt_sector));
    if (vibeos_partition_parse_gpt(g_pt_sector, g_pt_entries, sizeof(g_pt_entries),
                                   1000000u, &tbl) == 0) {
        return -1;
    }
    return 0;
}

/* ---- ext2 ----------------------------------------------------------------
 *
 * A fabricated filesystem in an array. Building the image by hand is the point:
 * it is the only way to place a file's blocks exactly where the indirection
 * boundaries are, and those boundaries are what a real image almost never
 * exercises and a boot can never show you - a file simply reads back plausible
 * and wrong somewhere past its twelfth block.
 *
 * Layout, 1 KiB blocks, one group:
 *   block 0  padding (the superblock starts at byte 1024)
 *   block 1  superblock
 *   block 2  group descriptors
 *   block 3  inode table
 *   block 4  root directory data
 *   block 5+ file data
 */
#define E2_BLOCK 1024u
#define E2_BLOCKS 80u
#define E2_INODE_SIZE 128u

static uint8_t g_e2[E2_BLOCKS * E2_BLOCK];

static void e2_w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void e2_w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t *e2_block(uint32_t n) { return g_e2 + (uint64_t)n * E2_BLOCK; }

/* One inode, written into the table at block 3. */
static uint8_t *e2_inode(uint32_t ino) {
    return e2_block(3) + (uint64_t)(ino - 1u) * E2_INODE_SIZE;
}

static void e2_dirent(uint8_t *at, uint32_t ino, const char *name, uint16_t reclen) {
    uint32_t n = 0;
    while (name[n]) { n++; }
    e2_w32(at, ino);
    e2_w16(at + 4, reclen);
    at[6] = (uint8_t)n;
    at[7] = 0;
    memcpy(at + 8, name, n);
}

/* Build an image with:
 *   inode 2  root directory, containing "." ".." "small" "big"
 *   inode 11 "small", 100 bytes, one direct block
 *   inode 12 "big", spanning direct blocks and into single indirection, with a
 *            hole where a block pointer is zero
 */
static void e2_build(void) {
    uint8_t *sb, *gd, *root, *ino, *ind;
    uint32_t i;

    memset(g_e2, 0, sizeof(g_e2));
    /* Block zero is padding before the superblock, and it is filled with a
     * marker rather than left zeroed on purpose: a driver that treats a hole
     * as a real block reads block zero, and if that block were zeroes the
     * mistake would produce exactly the right answer. */
    memset(e2_block(0), 0xEE, E2_BLOCK);

    sb = e2_block(1);
    e2_w32(sb + 0, 32u);          /* inodes count      */
    e2_w32(sb + 4, E2_BLOCKS);    /* blocks count      */
    e2_w32(sb + 20, 1u);          /* first data block  */
    e2_w32(sb + 24, 0u);          /* log block size -> 1024 */
    e2_w32(sb + 32, E2_BLOCKS);   /* blocks per group  */
    e2_w32(sb + 40, 32u);         /* inodes per group  */
    e2_w16(sb + 56, 0xEF53u);     /* magic             */
    e2_w32(sb + 76, 0u);          /* revision 0 -> 128-byte inodes */

    gd = e2_block(2);
    e2_w32(gd + 8, 3u);           /* inode table starts at block 3 */

    /* Root directory: one block of entries. */
    ino = e2_inode(2);
    e2_w16(ino + 0, 0x41EDu);     /* directory, 0755 */
    e2_w32(ino + 4, E2_BLOCK);    /* size            */
    e2_w32(ino + 40, 4u);         /* first direct block */

    root = e2_block(4);
    e2_dirent(root + 0,   2u,  ".",     12u);
    e2_dirent(root + 12,  2u,  "..",    12u);
    e2_dirent(root + 24,  11u, "small", 16u);
    e2_dirent(root + 40,  12u, "big",   (uint16_t)(E2_BLOCK - 40u));

    /* "small": 100 bytes in block 5. */
    ino = e2_inode(11);
    e2_w16(ino + 0, 0x81A4u);     /* regular, 0644 */
    e2_w32(ino + 4, 100u);
    e2_w32(ino + 40, 5u);
    for (i = 0; i < 100u; i++) {
        e2_block(5)[i] = (uint8_t)(i + 1u);
    }

    /* "big": 14 blocks. Twelve direct (6..17, with 10 left as a hole), then
     * single indirection through block 20 for blocks 12 and 13. */
    ino = e2_inode(12);
    e2_w16(ino + 0, 0x81A4u);
    e2_w32(ino + 4, 14u * E2_BLOCK);
    for (i = 0; i < 12u; i++) {
        /* Block index 4 is deliberately left zero: a hole. */
        e2_w32(ino + 40 + i * 4u, (i == 4u) ? 0u : (6u + i));
    }
    e2_w32(ino + 40 + 12u * 4u, 20u);      /* single indirect block */
    ind = e2_block(20);
    e2_w32(ind + 0, 30u);
    e2_w32(ind + 4, 31u);

    /* Fill every data block with a byte identifying it, so a wrong mapping
     * produces the wrong marker rather than something that merely looks odd. */
    for (i = 0; i < 12u; i++) {
        if (i != 4u) {
            memset(e2_block(6u + i), (int)(0xA0u + i), E2_BLOCK);
        }
    }
    memset(e2_block(30), 0xC0, E2_BLOCK);
    memset(e2_block(31), 0xC1, E2_BLOCK);
}

static int e2_dev_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= (uint64_t)E2_BLOCKS * (E2_BLOCK / VIBEOS_BLOCK_SIZE)) {
        return -1;
    }
    memcpy(buf, g_e2 + lba * VIBEOS_BLOCK_SIZE, VIBEOS_BLOCK_SIZE);
    return 0;
}

#define E2_SLOTS 8u
static uint8_t g_e2_cache_mem[E2_SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_e2_slots[E2_SLOTS];

static int e2_mount(vibeos_ext2_t *fs, vibeos_blockcache_t *bc,
                    vibeos_blockdev_t *dev) {
    uint32_t i;
    e2_build();
    for (i = 0; i < E2_SLOTS; i++) {
        g_e2_slots[i].data = g_e2_cache_mem[i];
    }
    memset(dev, 0, sizeof(*dev));
    dev->read = e2_dev_read;
    dev->write = 0;
    dev->ctx = 0;
    dev->sectors = (uint64_t)E2_BLOCKS * (E2_BLOCK / VIBEOS_BLOCK_SIZE);
    if (vibeos_blockcache_init(bc, dev, g_e2_slots, E2_SLOTS) != 0) {
        return -1;
    }
    return vibeos_ext2_mount(fs, bc, 0);
}

static int test_ext2_mount_and_lookup(void) {
    vibeos_ext2_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fs_node_t node;
    vibeos_fsmount_t mnt;

    if (e2_mount(&fs, &bc, &dev) != 0) {
        return -1;
    }
    if (fs.block_size != 1024u || fs.inode_size != 128u) {
        return -1;
    }
    if (vibeos_fs_mount(&mnt, vibeos_ext2_ops(), &fs, "ext2") != 0) {
        return -1;
    }
    /* The root, and a file inside it, through the same interface FAT uses. */
    if (vibeos_fs_lookup(&mnt, "/", &node) != 0 || !node.is_dir) {
        return -1;
    }
    if (vibeos_fs_lookup(&mnt, "/small", &node) != 0) {
        return -1;
    }
    if (node.is_dir || node.size != 100u || node.id != 11u) {
        return -1;
    }
    if (vibeos_fs_lookup(&mnt, "missing", &node) == 0) {
        return -1;
    }
    /* Read-only: the wrapper must refuse rather than follow a null pointer. */
    if (vibeos_fs_write_file(&mnt, "/small", "x", 1u) >= 0) {
        return -1;
    }
    if (vibeos_fs_mkdir(&mnt, "/d") == 0) {
        return -1;
    }
    return 0;
}

static int test_ext2_read(void) {
    vibeos_ext2_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fs_node_t node;
    vibeos_fsmount_t mnt;
    uint8_t buf[E2_BLOCK * 2u];
    long n;
    uint32_t i;

    if (e2_mount(&fs, &bc, &dev) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_ext2_ops(), &fs, "ext2") != 0) {
        return -1;
    }

    /* A short file out of one direct block. */
    if (vibeos_fs_lookup(&mnt, "small", &node) != 0) {
        return -1;
    }
    n = vibeos_fs_read_at(&mnt, &node, 0, buf, sizeof(buf));
    if (n != 100) {
        return -1;   /* clamped to the file's size, not the buffer's */
    }
    for (i = 0; i < 100u; i++) {
        if (buf[i] != (uint8_t)(i + 1u)) {
            return -1;
        }
    }
    /* Reading past the end is end of file, not an error. */
    if (vibeos_fs_read_at(&mnt, &node, 100u, buf, 10u) != 0) {
        return -1;
    }

    if (vibeos_fs_lookup(&mnt, "big", &node) != 0 || node.size != 14u * E2_BLOCK) {
        return -1;
    }
    /* Direct block 0. */
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, 16u) != 16 || buf[0] != 0xA0u) {
        return -1;
    }
    /* Direct block 11, the last one before indirection. */
    if (vibeos_fs_read_at(&mnt, &node, 11u * E2_BLOCK, buf, 16u) != 16 ||
        buf[0] != 0xABu) {
        return -1;
    }
    /* The first indirect block - the boundary that matters. */
    if (vibeos_fs_read_at(&mnt, &node, 12u * E2_BLOCK, buf, 16u) != 16 ||
        buf[0] != 0xC0u) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 13u * E2_BLOCK, buf, 16u) != 16 ||
        buf[0] != 0xC1u) {
        return -1;
    }
    /* The hole reads as zeroes rather than failing: a sparse file is valid. */
    if (vibeos_fs_read_at(&mnt, &node, 4u * E2_BLOCK, buf, 16u) != 16) {
        return -1;
    }
    for (i = 0; i < 16u; i++) {
        if (buf[i] != 0u) {
            return -1;
        }
    }
    /* A read spanning the direct/indirect boundary must not repeat or skip. */
    n = vibeos_fs_read_at(&mnt, &node, 12u * E2_BLOCK - 8u, buf, 16u);
    if (n != 16) {
        return -1;
    }
    for (i = 0; i < 8u; i++) {
        if (buf[i] != 0xABu || buf[8u + i] != 0xC0u) {
            return -1;
        }
    }
    return 0;
}

static int test_ext2_list(void) {
    vibeos_ext2_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fsmount_t mnt;
    char name[VIBEOS_FS_NAME_MAX];
    uint64_t size = 0;
    int is_dir = 0;

    if (e2_mount(&fs, &bc, &dev) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_ext2_ops(), &fs, "ext2") != 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 0, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, ".") != 0 || !is_dir) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 2, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "small") != 0 || is_dir || size != 100u) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 3, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "big") != 0) {
        return -1;
    }
    /* Past the last entry is a refusal, which is how a caller stops. */
    if (vibeos_fs_list(&mnt, "/", 4, name, sizeof(name), &size, &is_dir) == 0) {
        return -1;
    }
    /* Listing a file is not listing. */
    if (vibeos_fs_list(&mnt, "/small", 0, name, sizeof(name), &size, &is_dir) == 0) {
        return -1;
    }
    return 0;
}

static int test_ext2_refusals(void) {
    vibeos_ext2_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;

    /* A wrong magic is not an ext2 volume, and mounting one anyway would read
     * plausible bytes from the wrong offsets forever after. */
    if (e2_mount(&fs, &bc, &dev) != 0) {
        return -1;
    }
    e2_w16(e2_block(1) + 56, 0x1234u);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ext2_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A block size this driver does not support is refused rather than
     * approximated. */
    e2_build();
    e2_w32(e2_block(1) + 24, 5u);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ext2_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A directory record claiming zero length. Without the guard the scan
     * advances by zero and never terminates, so the failure mode here is a
     * hung test rather than a red one - which is why the guard exists at all
     * rather than being left to the caller. */
    {
        vibeos_fsmount_t mnt;
        vibeos_fs_node_t node;
        e2_build();
        e2_w16(e2_block(4) + 4, 0u);
        vibeos_blockcache_invalidate(&bc);
        if (vibeos_ext2_mount(&fs, &bc, 0) != 0 ||
            vibeos_fs_mount(&mnt, vibeos_ext2_ops(), &fs, "ext2") != 0) {
            return -1;
        }
        if (vibeos_fs_lookup(&mnt, "/small", &node) == 0) {
            return -1;   /* a corrupt directory cannot resolve a name */
        }
    }

    /* Zero inodes per group would divide by zero on the first lookup. */
    e2_build();
    e2_w32(e2_block(1) + 40, 0u);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ext2_mount(&fs, &bc, 0) == 0) {
        return -1;
    }
    return 0;
}

/* ---- ISO9660 -------------------------------------------------------------
 *
 * A fabricated disc. The cases that matter are all about names: ISO9660 stores
 * them upper case with a version suffix, so "vmlinuz" is "VMLINUZ;1" on the
 * disc, and a driver that reports the stored form hands back a name its caller
 * cannot pass to open().
 */
#define ISO_SECTORS 40u
static uint8_t g_iso[ISO_SECTORS * VIBEOS_ISO_SECTOR];

static uint8_t *iso_sec(uint32_t n) { return g_iso + (uint64_t)n * VIBEOS_ISO_SECTOR; }

static void iso_w32both(uint8_t *p, uint32_t v) {
    /* Little-endian then big-endian, which is how ISO9660 stores every number
     * and the reason a naive 8-byte read produces an enormous wrong one. */
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
    p[6] = (uint8_t)(v >> 8); p[7] = (uint8_t)v;
}

/* One directory record. Returns its length. */
static uint32_t iso_rec(uint8_t *at, uint32_t extent, uint32_t len, int is_dir,
                        const char *name) {
    uint32_t nlen = 0, total;
    while (name[nlen]) { nlen++; }
    total = 33u + nlen;
    if (total & 1u) { total++; }   /* records are padded to an even length */
    memset(at, 0, total);
    at[0] = (uint8_t)total;
    iso_w32both(at + 2, extent);
    iso_w32both(at + 10, len);
    at[25] = is_dir ? 0x02u : 0x00u;
    at[32] = (uint8_t)nlen;
    memcpy(at + 33, name, nlen);
    return total;
}

/* Special "." / ".." records, whose names are a single byte 0 or 1. */
static uint32_t iso_rec_dot(uint8_t *at, uint32_t extent, uint8_t which) {
    memset(at, 0, 34);
    at[0] = 34u;
    iso_w32both(at + 2, extent);
    iso_w32both(at + 10, VIBEOS_ISO_SECTOR);
    at[25] = 0x02u;
    at[32] = 1u;
    at[33] = which;
    return 34u;
}

/* Root at sector 20 with "README.TXT;1", "NOEXT." and a directory "SUB";
 * SUB at 21 containing "INNER.BIN;1"; file data at 22 and 23. */
static void iso_build(void) {
    uint8_t *pvd, *root, *sub;
    uint32_t off;
    uint32_t i;

    memset(g_iso, 0, sizeof(g_iso));

    pvd = iso_sec(VIBEOS_ISO_PVD_SECTOR);
    pvd[0] = 1u;
    memcpy(pvd + 1, "CD001", 5);
    iso_w32both(pvd + 128, VIBEOS_ISO_SECTOR);
    iso_rec(pvd + 156, 20u, VIBEOS_ISO_SECTOR, 1, "\x00");
    /* iso_rec wrote a one-character name; fix it up to the root's form. */
    pvd[156 + 32] = 1u;
    pvd[156 + 33] = 0u;

    root = iso_sec(20);
    off = iso_rec_dot(root, 20u, 0u);
    off += iso_rec_dot(root + off, 20u, 1u);
    off += iso_rec(root + off, 22u, 300u, 0, "README.TXT;1");
    off += iso_rec(root + off, 23u, 2048u, 0, "NOEXT.");
    (void)iso_rec(root + off, 21u, VIBEOS_ISO_SECTOR, 1, "SUB");

    sub = iso_sec(21);
    off = iso_rec_dot(sub, 21u, 0u);
    off += iso_rec_dot(sub + off, 20u, 1u);
    (void)iso_rec(sub + off, 24u, 64u, 0, "INNER.BIN;1");

    for (i = 0; i < 300u; i++) {
        iso_sec(22)[i] = (uint8_t)(i & 0xFFu);
    }
    memset(iso_sec(23), 0x77, VIBEOS_ISO_SECTOR);
    memset(iso_sec(24), 0x5A, 64u);
}

static int iso_dev_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= (uint64_t)ISO_SECTORS * (VIBEOS_ISO_SECTOR / VIBEOS_BLOCK_SIZE)) {
        return -1;
    }
    memcpy(buf, g_iso + lba * VIBEOS_BLOCK_SIZE, VIBEOS_BLOCK_SIZE);
    return 0;
}

#define ISO_SLOTS 8u
static uint8_t g_iso_cache_mem[ISO_SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_iso_slots[ISO_SLOTS];

static int iso_mount(vibeos_iso9660_t *fs, vibeos_blockcache_t *bc,
                     vibeos_blockdev_t *dev) {
    uint32_t i;
    iso_build();
    for (i = 0; i < ISO_SLOTS; i++) {
        g_iso_slots[i].data = g_iso_cache_mem[i];
    }
    memset(dev, 0, sizeof(*dev));
    dev->read = iso_dev_read;
    dev->write = 0;
    dev->ctx = 0;
    dev->sectors = (uint64_t)ISO_SECTORS * (VIBEOS_ISO_SECTOR / VIBEOS_BLOCK_SIZE);
    if (vibeos_blockcache_init(bc, dev, g_iso_slots, ISO_SLOTS) != 0) {
        return -1;
    }
    return vibeos_iso9660_mount(fs, bc, 0);
}

static int test_iso_lookup_and_read(void) {
    vibeos_iso9660_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fsmount_t mnt;
    vibeos_fs_node_t node;
    uint8_t buf[512];
    uint32_t i;

    if (iso_mount(&fs, &bc, &dev) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_iso9660_ops(), &fs, "iso9660") != 0) {
        return -1;
    }

    /* Asked for in lower case and without the version suffix, which is how a
     * program would ask - and is not how the disc stores it. */
    if (vibeos_fs_lookup(&mnt, "/readme.txt", &node) != 0) {
        return -1;
    }
    if (node.is_dir || node.size != 300u) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, sizeof(buf)) != 300) {
        return -1;
    }
    for (i = 0; i < 300u; i++) {
        if (buf[i] != (uint8_t)(i & 0xFFu)) {
            return -1;
        }
    }
    /* A name with no extension is stored with a trailing dot. */
    if (vibeos_fs_lookup(&mnt, "NOEXT", &node) != 0 || node.size != 2048u) {
        return -1;
    }
    /* Down one level. */
    if (vibeos_fs_lookup(&mnt, "/sub", &node) != 0 || !node.is_dir) {
        return -1;
    }
    if (vibeos_fs_lookup(&mnt, "/sub/inner.bin", &node) != 0 || node.size != 64u) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, 64u) != 64 || buf[0] != 0x5Au) {
        return -1;
    }
    /* A component below a file is not a path. */
    if (vibeos_fs_lookup(&mnt, "/readme.txt/x", &node) == 0) {
        return -1;
    }
    if (vibeos_fs_lookup(&mnt, "/nope", &node) == 0) {
        return -1;
    }
    /* Read-only. */
    if (vibeos_fs_write_file(&mnt, "/x", "y", 1u) >= 0) {
        return -1;
    }
    return 0;
}

static int test_iso_list(void) {
    vibeos_iso9660_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fsmount_t mnt;
    char name[VIBEOS_FS_NAME_MAX];
    uint64_t size = 0;
    int is_dir = 0;

    if (iso_mount(&fs, &bc, &dev) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_iso9660_ops(), &fs, "iso9660") != 0) {
        return -1;
    }
    /* "." and ".." are skipped, and the version suffix is not reported - a
     * name a caller cannot hand back to open() is not a useful answer. */
    if (vibeos_fs_list(&mnt, "/", 0, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "README.TXT") != 0 || is_dir || size != 300u) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 1, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "NOEXT") != 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 2, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "SUB") != 0 || !is_dir) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 3, name, sizeof(name), &size, &is_dir) == 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/sub", 0, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "INNER.BIN") != 0) {
        return -1;
    }
    return 0;
}

static int test_iso_refusals(void) {
    vibeos_iso9660_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;

    /* No CD001: not an ISO9660 volume. */
    if (iso_mount(&fs, &bc, &dev) != 0) {
        return -1;
    }
    iso_sec(VIBEOS_ISO_PVD_SECTOR)[2] = 'X';
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_iso9660_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* Wrong descriptor type in the right place. */
    iso_build();
    iso_sec(VIBEOS_ISO_PVD_SECTOR)[0] = 2u;
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_iso9660_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A logical block size this driver does not handle. */
    iso_build();
    iso_w32both(iso_sec(VIBEOS_ISO_PVD_SECTOR) + 128, 512u);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_iso9660_mount(&fs, &bc, 0) == 0) {
        return -1;
    }
    return 0;
}

/* ---- exFAT ---------------------------------------------------------------
 *
 * The two cases worth building an image for: a file described by a set of
 * entries rather than one, and a file that is contiguous so the allocation
 * table says nothing about it. A driver that follows the chain anyway reads
 * whatever the table happens to contain - and on a real volume most files are
 * contiguous, so that mistake affects almost everything.
 */
#define XF_SECTORS 128u
static uint8_t g_xf[XF_SECTORS * VIBEOS_BLOCK_SIZE];

static uint8_t *xf_sec(uint32_t n) { return g_xf + (uint64_t)n * VIBEOS_BLOCK_SIZE; }

static void xf_w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void xf_w64(uint8_t *p, uint64_t v) {
    xf_w32(p, (uint32_t)v);
    xf_w32(p + 4, (uint32_t)(v >> 32));
}

/* Geometry: 512-byte sectors, one sector per cluster, FAT at sector 8,
 * cluster heap at sector 16 (so cluster 2 is sector 16). */
#define XF_FAT_SEC 8u
#define XF_HEAP_SEC 16u
static uint8_t *xf_cluster(uint32_t c) { return xf_sec(XF_HEAP_SEC + (c - 2u)); }

static void xf_fat_set(uint32_t cluster, uint32_t next) {
    xf_w32(xf_sec(XF_FAT_SEC + (cluster * 4u) / VIBEOS_BLOCK_SIZE)
           + ((cluster * 4u) % VIBEOS_BLOCK_SIZE), next);
}

/* Write one file entry set at `at`: a file entry, a stream entry, one name
 * entry. Returns the bytes used. */
static uint32_t xf_entry(uint8_t *at, const char *name, uint32_t first_cluster,
                         uint64_t size, int is_dir, int contiguous) {
    uint32_t nlen = 0, i;
    while (name[nlen]) { nlen++; }

    memset(at, 0, 96);
    at[0] = 0x85u;                       /* file entry           */
    at[1] = 2u;                          /* two secondary entries */
    at[4] = (uint8_t)(is_dir ? 0x10u : 0x20u);

    at[32] = 0xC0u;                      /* stream entry */
    at[33] = (uint8_t)(contiguous ? 0x02u : 0x00u);
    at[35] = (uint8_t)nlen;
    xf_w32(at + 32 + 20, first_cluster);
    xf_w64(at + 32 + 24, size);

    at[64] = 0xC1u;                      /* name entry */
    for (i = 0; i < nlen && i < 15u; i++) {
        at[64 + 2 + i * 2] = (uint8_t)name[i];
    }
    return 96u;
}

/* Root directory in cluster 2. Contains:
 *   CONTIG.BIN  - 3 clusters starting at 5, marked contiguous, FAT left wrong
 *                 on purpose so a driver that follows it reads the wrong data
 *   CHAINED.BIN - 2 clusters, 8 then 20, joined through the table
 *   SUB         - a directory in cluster 30 containing DEEP.TXT
 */
static void xf_build(void) {
    uint8_t *boot, *root, *sub;
    uint32_t off;
    uint32_t i;

    memset(g_xf, 0, sizeof(g_xf));

    boot = xf_sec(0);
    memcpy(boot + 3, "EXFAT   ", 8);
    xf_w32(boot + 80, XF_FAT_SEC);       /* FatOffset          */
    xf_w32(boot + 84, 8u);               /* FatLength          */
    xf_w32(boot + 88, XF_HEAP_SEC);      /* ClusterHeapOffset  */
    xf_w32(boot + 92, 100u);             /* ClusterCount       */
    xf_w32(boot + 96, 2u);               /* root cluster       */
    boot[108] = 9u;                      /* 512-byte sectors   */
    boot[109] = 0u;                      /* one sector/cluster */

    root = xf_cluster(2);
    off = xf_entry(root, "CONTIG.BIN", 5u, 3u * VIBEOS_BLOCK_SIZE, 0, 1);
    off += xf_entry(root + off, "CHAINED.BIN", 8u, 2u * VIBEOS_BLOCK_SIZE, 0, 0);
    (void)xf_entry(root + off, "SUB", 30u, 0u, 1, 1);

    sub = xf_cluster(30);
    (void)xf_entry(sub, "DEEP.TXT", 40u, 10u, 0, 1);

    /* A malformed set at the end of the root: a file entry whose secondary is
     * not a stream entry. A driver that does not insist on the stream decodes
     * garbage as a file, so the check is what stops a corrupt directory from
     * inventing entries. */
    {
        uint8_t *bad = root + off + 96u;
        memset(bad, 0, 96);
        bad[0] = 0x85u;
        bad[1] = 2u;
        bad[32] = 0xC1u;   /* a name entry where the stream must be */
        bad[64] = 0xC1u;
    }

    /* The contiguous file's clusters, 5..7, each marked with its own byte. */
    for (i = 0; i < 3u; i++) {
        memset(xf_cluster(5u + i), (int)(0xB0u + i), VIBEOS_BLOCK_SIZE);
    }
    /* Deliberately wrong table entries for it: a driver that follows the chain
     * instead of honouring the contiguous flag lands on cluster 60. */
    xf_fat_set(5u, 60u);
    xf_fat_set(6u, 60u);
    memset(xf_cluster(60), 0xDD, VIBEOS_BLOCK_SIZE);

    /* The chained file: cluster 8 then 20. */
    memset(xf_cluster(8), 0xC8, VIBEOS_BLOCK_SIZE);
    memset(xf_cluster(20), 0xCA, VIBEOS_BLOCK_SIZE);
    xf_fat_set(8u, 20u);
    xf_fat_set(20u, 0xFFFFFFFFu);

    memset(xf_cluster(40), 0x77, 10u);
}

static int xf_dev_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= XF_SECTORS) {
        return -1;
    }
    memcpy(buf, g_xf + lba * VIBEOS_BLOCK_SIZE, VIBEOS_BLOCK_SIZE);
    return 0;
}

#define XF_SLOTS 8u
static uint8_t g_xf_cache_mem[XF_SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_xf_slots[XF_SLOTS];

static int xf_mount(vibeos_exfat_t *fs, vibeos_blockcache_t *bc,
                    vibeos_blockdev_t *dev) {
    uint32_t i;
    xf_build();
    for (i = 0; i < XF_SLOTS; i++) {
        g_xf_slots[i].data = g_xf_cache_mem[i];
    }
    memset(dev, 0, sizeof(*dev));
    dev->read = xf_dev_read;
    dev->write = 0;
    dev->ctx = 0;
    dev->sectors = XF_SECTORS;
    if (vibeos_blockcache_init(bc, dev, g_xf_slots, XF_SLOTS) != 0) {
        return -1;
    }
    return vibeos_exfat_mount(fs, bc, 0);
}

static int test_exfat_lookup_and_read(void) {
    vibeos_exfat_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fsmount_t mnt;
    vibeos_fs_node_t node;
    uint8_t buf[VIBEOS_BLOCK_SIZE * 3u];

    if (xf_mount(&fs, &bc, &dev) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_exfat_ops(), &fs, "exfat") != 0) {
        return -1;
    }
    if (fs.cluster_bytes != VIBEOS_BLOCK_SIZE || fs.root_cluster != 2u) {
        return -1;
    }

    /* The contiguous file. The allocation table points somewhere else on
     * purpose, so reading the right bytes proves the flag was honoured. */
    if (vibeos_fs_lookup(&mnt, "contig.bin", &node) != 0) {
        return -1;
    }
    if (node.is_dir || node.size != 3u * VIBEOS_BLOCK_SIZE) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, sizeof(buf)) != (long)sizeof(buf)) {
        return -1;
    }
    if (buf[0] != 0xB0u || buf[VIBEOS_BLOCK_SIZE] != 0xB1u ||
        buf[VIBEOS_BLOCK_SIZE * 2u] != 0xB2u) {
        return -1;
    }

    /* The chained file, which does need the table. */
    if (vibeos_fs_lookup(&mnt, "CHAINED.BIN", &node) != 0) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, VIBEOS_BLOCK_SIZE * 2u) !=
        (long)(VIBEOS_BLOCK_SIZE * 2u)) {
        return -1;
    }
    if (buf[0] != 0xC8u || buf[VIBEOS_BLOCK_SIZE] != 0xCAu) {
        return -1;
    }

    /* A directory, and a file inside it. */
    if (vibeos_fs_lookup(&mnt, "/sub", &node) != 0 || !node.is_dir) {
        return -1;
    }
    if (vibeos_fs_lookup(&mnt, "/sub/deep.txt", &node) != 0 || node.size != 10u) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, 10u) != 10 || buf[0] != 0x77u) {
        return -1;
    }
    /* Asking for more than the file holds returns the file, not the buffer.
     * An unclamped read walks into the next cluster and hands back bytes that
     * belong to something else. */
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, 400u) != 10) {
        return -1;
    }
    if (vibeos_fs_lookup(&mnt, "/nothing", &node) == 0) {
        return -1;
    }
    if (vibeos_fs_write_file(&mnt, "/x", "y", 1u) >= 0) {
        return -1;
    }
    return 0;
}

static int test_exfat_list(void) {
    vibeos_exfat_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fsmount_t mnt;
    char name[VIBEOS_FS_NAME_MAX];
    uint64_t size = 0;
    int is_dir = 0;

    if (xf_mount(&fs, &bc, &dev) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_exfat_ops(), &fs, "exfat") != 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 0, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "CONTIG.BIN") != 0 || is_dir ||
        size != 3u * VIBEOS_BLOCK_SIZE) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 1, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "CHAINED.BIN") != 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 2, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "SUB") != 0 || !is_dir) {
        return -1;
    }
    /* The malformed set at the end contributes nothing: there is no fourth
     * entry, and a driver that accepted it would report a fourth with a name
     * made of whatever those bytes happened to be. */
    if (vibeos_fs_list(&mnt, "/", 3, name, sizeof(name), &size, &is_dir) == 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/sub", 0, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "DEEP.TXT") != 0) {
        return -1;
    }
    return 0;
}

static int test_exfat_refusals(void) {
    vibeos_exfat_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;

    if (xf_mount(&fs, &bc, &dev) != 0) {
        return -1;
    }
    /* Not an exFAT volume. */
    xf_sec(0)[5] = 'X';
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_exfat_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A sector size the block cache cannot serve. Reading anyway would use the
     * wrong offsets from the first cluster onwards. */
    xf_build();
    xf_sec(0)[108] = 12u;
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_exfat_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A cluster size beyond what this driver buffers. */
    xf_build();
    xf_sec(0)[109] = 6u;
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_exfat_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A root cluster below 2 is not a cluster at all. */
    xf_build();
    xf_w32(xf_sec(0) + 96, 1u);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_exfat_mount(&fs, &bc, 0) == 0) {
        return -1;
    }
    return 0;
}

/* ---- NTFS ----------------------------------------------------------------
 *
 * Building the image is most of the work, and deliberately so: the fixups have
 * to be applied when writing it, which means the test image is only readable
 * if the test builder and the driver agree about them. A driver that skipped
 * fixups would read two wrong bytes per sector, and the only way to notice is
 * to have written the right ones in the first place.
 */
#define NT_SECTORS 256u
#define NT_RECORD 1024u
#define NT_MFT_LCN 4u          /* cluster 4, one sector per cluster */
static uint8_t g_nt[NT_SECTORS * VIBEOS_BLOCK_SIZE];

static uint8_t *nt_sec(uint32_t n) { return g_nt + (uint64_t)n * VIBEOS_BLOCK_SIZE; }

/* Readers to match the writers: the hostile-attribute cases have to walk the
 * record the way the driver does rather than assume an offset the fixture
 * could change. */
static uint16_t nt_r16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t nt_r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void nt_w16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void nt_w32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void nt_w64(uint8_t *p, uint64_t v) {
    nt_w32(p, (uint32_t)v);
    nt_w32(p + 4, (uint32_t)(v >> 32));
}

/* A record lives at MFT cluster + number * 1024, i.e. two sectors each. */
static uint8_t *nt_record(uint32_t number) {
    return nt_sec(NT_MFT_LCN + number * (NT_RECORD / VIBEOS_BLOCK_SIZE));
}

/* Write a UTF-16 name. */
static uint32_t nt_name(uint8_t *at, const char *name) {
    uint32_t n = 0;
    while (name[n]) {
        nt_w16(at + n * 2u, (uint16_t)name[n]);
        n++;
    }
    return n;
}

/* Apply the update sequence to a finished record: stash the last two bytes of
 * each sector into the array and write the sequence number in their place. */
static void nt_fixup(uint8_t *rec) {
    uint16_t seq = 0x0BAD;
    uint32_t sectors = NT_RECORD / VIBEOS_BLOCK_SIZE;
    uint32_t i;

    nt_w16(rec + 4, 48u);              /* update sequence array offset */
    nt_w16(rec + 6, (uint16_t)(sectors + 1u));
    nt_w16(rec + 48, seq);
    for (i = 1; i <= sectors; i++) {
        uint8_t *tail = rec + i * VIBEOS_BLOCK_SIZE - 2u;
        rec[48 + i * 2u] = tail[0];
        rec[48 + i * 2u + 1u] = tail[1];
        nt_w16(tail, seq);
    }
}

/* An index entry inside a directory's index root. Returns its length. */
static uint32_t nt_index_entry(uint8_t *at, uint64_t ref, const char *name,
                               uint64_t size, int is_dir, int dos_only) {
    uint32_t chars, len;

    memset(at, 0, 128);
    nt_w64(at, ref);
    nt_w64(at + 64, size);                          /* real size    */
    nt_w32(at + 72, is_dir ? 0x10000000u : 0u);     /* flags        */
    chars = nt_name(at + 82, name);
    at[80] = (uint8_t)chars;
    at[81] = (uint8_t)(dos_only ? 2u : 1u);         /* namespace    */
    len = 82u + chars * 2u;
    len = (len + 7u) & ~7u;                         /* eight-byte aligned */
    nt_w16(at + 8, (uint16_t)len);
    return len;
}

/* Records: 5 is the root directory, 6 a resident file, 7 a file with a run
 * list, 8 a subdirectory, 9 a file inside it. */
static void nt_build(void) {
    uint8_t *boot, *rec, *attr, *root_idx;
    uint32_t off, chars, i;

    memset(g_nt, 0, sizeof(g_nt));

    boot = nt_sec(0);
    memcpy(boot + 3, "NTFS    ", 8);
    nt_w16(boot + 11, VIBEOS_BLOCK_SIZE);
    boot[13] = 1u;                       /* one sector per cluster */
    nt_w64(boot + 0x30, NT_MFT_LCN);
    boot[0x40] = (uint8_t)(int8_t)(-10); /* 2^10 = 1024-byte records */

    /* ---- record 5: the root directory ---- */
    rec = nt_record(5);
    memcpy(rec, "FILE", 4);
    nt_w16(rec + 22, 0x0003u);           /* in use + directory */
    nt_w16(rec + 0x14, 64u);             /* first attribute    */

    attr = rec + 64;
    nt_w32(attr, 0x90u);                 /* $INDEX_ROOT        */
    attr[8] = 0u;                        /* resident           */
    nt_w16(attr + 0x14, 32u);            /* value offset       */
    root_idx = attr + 32;
    nt_w32(root_idx + 16, 16u);          /* first entry, from the header */
    off = 16u + 16u;
    off += nt_index_entry(root_idx + off, 6u, "RESIDENT.TXT", 12u, 0, 0);
    off += nt_index_entry(root_idx + off, 7u, "BIG.BIN", 3u * VIBEOS_BLOCK_SIZE, 0, 0);
    off += nt_index_entry(root_idx + off, 8u, "SUB", 0u, 1, 0);
    /* A DOS short-name duplicate, which must not be reported twice. */
    off += nt_index_entry(root_idx + off, 6u, "RESIDE~1.TXT", 12u, 0, 1);
    {
        /* The end entry: no name, flag bit 1 set. */
        uint8_t *end = root_idx + off;
        memset(end, 0, 16);
        nt_w16(end + 8, 16u);
        nt_w32(end + 12, 0x02u);
        off += 16u;
    }
    nt_w32(attr + 0x10, off);            /* value length */
    nt_w32(attr + 4, 32u + off);         /* attribute length */
    nt_w32(rec + 64 + 32u + off, 0xFFFFFFFFu);   /* end of attributes */
    nt_fixup(rec);

    /* ---- record 6: a resident file ---- */
    rec = nt_record(6);
    memcpy(rec, "FILE", 4);
    nt_w16(rec + 22, 0x0001u);
    nt_w16(rec + 0x14, 64u);
    attr = rec + 64;
    nt_w32(attr, 0x80u);                 /* $DATA    */
    attr[8] = 0u;                        /* resident */
    nt_w32(attr + 0x10, 12u);            /* value length */
    nt_w16(attr + 0x14, 24u);            /* value offset */
    memcpy(attr + 24, "hello resid", 11);
    attr[24 + 11] = '!';
    nt_w32(attr + 4, 24u + 12u);
    nt_w32(rec + 64 + 24u + 12u, 0xFFFFFFFFu);
    nt_fixup(rec);

    /* ---- record 7: a file described by a run list ----
     * Two runs: three clusters from 100, then a sparse run, so both paths are
     * exercised by one file. */
    rec = nt_record(7);
    memcpy(rec, "FILE", 4);
    nt_w16(rec + 22, 0x0001u);
    nt_w16(rec + 0x14, 64u);
    attr = rec + 64;
    nt_w32(attr, 0x80u);
    attr[8] = 1u;                        /* non-resident */
    nt_w16(attr + 0x20, 64u);            /* mapping pairs offset */
    nt_w64(attr + 0x30, 5u * VIBEOS_BLOCK_SIZE);  /* data size */
    {
        uint8_t *runs = attr + 64;
        uint32_t r = 0;
        runs[r++] = 0x11u;               /* one length byte, one offset byte */
        runs[r++] = 3u;                  /* three clusters                   */
        runs[r++] = 100u;                /* starting at cluster 100          */
        runs[r++] = 0x01u;               /* one length byte, no offset: sparse */
        runs[r++] = 1u;                  /* one cluster                      */
        /* A run that starts *below* the previous one. The offset is a signed
         * delta stored in as few bytes as it fits, so 0xCE here means -50, and
         * a reader that does not sign-extend lands at cluster 306 instead of
         * 50 - off the end of this volume, and on a real one simply the wrong
         * data. */
        runs[r++] = 0x11u;
        runs[r++] = 1u;
        runs[r++] = 0xCEu;               /* -50 from cluster 100             */
        runs[r++] = 0x00u;               /* end of the run list              */
        nt_w32(attr + 4, 64u + r);
        nt_w32(attr + 64u + r, 0xFFFFFFFFu);
    }
    nt_fixup(rec);
    for (i = 0; i < 3u; i++) {
        memset(nt_sec(100u + i), (int)(0xE0u + i), VIBEOS_BLOCK_SIZE);
    }
    memset(nt_sec(50), 0xE5, VIBEOS_BLOCK_SIZE);   /* the backwards run */

    /* ---- record 8: a subdirectory holding record 9 ---- */
    rec = nt_record(8);
    memcpy(rec, "FILE", 4);
    nt_w16(rec + 22, 0x0003u);
    nt_w16(rec + 0x14, 64u);
    attr = rec + 64;
    nt_w32(attr, 0x90u);
    attr[8] = 0u;
    nt_w16(attr + 0x14, 32u);
    root_idx = attr + 32;
    nt_w32(root_idx + 16, 16u);
    off = 16u + 16u;
    off += nt_index_entry(root_idx + off, 9u, "INNER.DAT", 5u, 0, 0);
    off += nt_index_entry(root_idx + off, 10u, "BADATTR", 0u, 0, 0);
    {
        uint8_t *end = root_idx + off;
        memset(end, 0, 16);
        nt_w16(end + 8, 16u);
        nt_w32(end + 12, 0x02u);
        off += 16u;
    }
    nt_w32(attr + 0x10, off);
    nt_w32(attr + 4, 32u + off);
    nt_w32(rec + 64 + 32u + off, 0xFFFFFFFFu);
    nt_fixup(rec);

    /* ---- record 10: an attribute claiming zero length ----
     * Walking attributes by their own length is how the list is traversed, so
     * a zero length means the walk never advances. The guard turns that into
     * "no such attribute"; without it the search never returns. */
    rec = nt_record(10);
    memcpy(rec, "FILE", 4);
    nt_w16(rec + 22, 0x0001u);
    nt_w16(rec + 0x14, 64u);
    attr = rec + 64;
    nt_w32(attr, 0x30u);                 /* $FILE_NAME, not $DATA */
    nt_w32(attr + 4, 0u);                /* length zero           */
    nt_fixup(rec);

    /* ---- record 9 ---- */
    rec = nt_record(9);
    memcpy(rec, "FILE", 4);
    nt_w16(rec + 22, 0x0001u);
    nt_w16(rec + 0x14, 64u);
    attr = rec + 64;
    nt_w32(attr, 0x80u);
    attr[8] = 0u;
    nt_w32(attr + 0x10, 5u);
    nt_w16(attr + 0x14, 24u);
    memcpy(attr + 24, "inner", 5);
    nt_w32(attr + 4, 24u + 5u);
    nt_w32(rec + 64 + 24u + 5u, 0xFFFFFFFFu);
    nt_fixup(rec);
    (void)chars;
}

static int nt_dev_read(void *ctx, uint64_t lba, void *buf) {
    (void)ctx;
    if (lba >= NT_SECTORS) {
        return -1;
    }
    memcpy(buf, g_nt + lba * VIBEOS_BLOCK_SIZE, VIBEOS_BLOCK_SIZE);
    return 0;
}

#define NT_SLOTS 8u
static uint8_t g_nt_cache_mem[NT_SLOTS][VIBEOS_BLOCK_SIZE];
static vibeos_block_slot_t g_nt_slots[NT_SLOTS];

static int nt_mount(vibeos_ntfs_t *fs, vibeos_blockcache_t *bc,
                    vibeos_blockdev_t *dev) {
    uint32_t i;
    nt_build();
    for (i = 0; i < NT_SLOTS; i++) {
        g_nt_slots[i].data = g_nt_cache_mem[i];
    }
    memset(dev, 0, sizeof(*dev));
    dev->read = nt_dev_read;
    dev->write = 0;
    dev->ctx = 0;
    dev->sectors = NT_SECTORS;
    if (vibeos_blockcache_init(bc, dev, g_nt_slots, NT_SLOTS) != 0) {
        return -1;
    }
    return vibeos_ntfs_mount(fs, bc, 0);
}

static int test_ntfs_mount_and_read(void) {
    vibeos_ntfs_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fsmount_t mnt;
    vibeos_fs_node_t node;
    uint8_t buf[VIBEOS_BLOCK_SIZE * 4u];
    uint32_t i;

    if (nt_mount(&fs, &bc, &dev) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_ntfs_ops(), &fs, "ntfs") != 0) {
        return -1;
    }
    if (fs.mft_record_bytes != NT_RECORD || fs.cluster_bytes != VIBEOS_BLOCK_SIZE) {
        return -1;
    }

    /* A resident file: its contents are inside its own record, so it occupies
     * no clusters at all. */
    if (vibeos_fs_lookup(&mnt, "/resident.txt", &node) != 0) {
        return -1;
    }
    if (node.is_dir || node.size != 12u) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, sizeof(buf)) != 12) {
        return -1;
    }
    if (memcmp(buf, "hello resid!", 12) != 0) {
        return -1;
    }

    /* A file with a run list: three real clusters then a sparse one. */
    if (vibeos_fs_lookup(&mnt, "BIG.BIN", &node) != 0 ||
        node.size != 5u * VIBEOS_BLOCK_SIZE) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, sizeof(buf)) != (long)sizeof(buf)) {
        return -1;
    }
    if (buf[0] != 0xE0u || buf[VIBEOS_BLOCK_SIZE] != 0xE1u ||
        buf[VIBEOS_BLOCK_SIZE * 2u] != 0xE2u) {
        return -1;
    }
    /* The sparse run reads as zeroes; treating it as cluster zero would hand
     * back the boot sector from the middle of the file. */
    for (i = 0; i < VIBEOS_BLOCK_SIZE; i++) {
        if (buf[VIBEOS_BLOCK_SIZE * 3u + i] != 0u) {
            return -1;
        }
    }
    /* The backwards run: cluster four of the file lives at a lower cluster
     * than cluster zero, which only works if the offset was read as a signed
     * delta. Read as unsigned it lands at cluster 306 - off this volume, and
     * on a real one simply somebody else's data. */
    if (vibeos_fs_read_at(&mnt, &node, 4u * VIBEOS_BLOCK_SIZE, buf, 16u) != 16) {
        return -1;
    }
    if (buf[0] != 0xE5u) {
        return -1;
    }

    /* Down a level. */
    if (vibeos_fs_lookup(&mnt, "/sub", &node) != 0 || !node.is_dir) {
        return -1;
    }
    if (vibeos_fs_lookup(&mnt, "/sub/inner.dat", &node) != 0 || node.size != 5u) {
        return -1;
    }
    if (vibeos_fs_read_at(&mnt, &node, 0, buf, 5u) != 5 ||
        memcmp(buf, "inner", 5) != 0) {
        return -1;
    }
    /* A record whose attribute claims zero length: the walk must end rather
     * than never advance. Reaching this line at all is the assertion. */
    if (vibeos_fs_lookup(&mnt, "/sub/badattr", &node) != 0 || node.size != 0u) {
        return -1;
    }
    if (vibeos_fs_lookup(&mnt, "/absent", &node) == 0) {
        return -1;
    }
    if (vibeos_fs_write_file(&mnt, "/x", "y", 1u) >= 0) {
        return -1;
    }
    return 0;
}

static int test_ntfs_list(void) {
    vibeos_ntfs_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fsmount_t mnt;
    char name[VIBEOS_FS_NAME_MAX];
    uint64_t size = 0;
    int is_dir = 0;

    if (nt_mount(&fs, &bc, &dev) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_ntfs_ops(), &fs, "ntfs") != 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 0, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "RESIDENT.TXT") != 0 || is_dir || size != 12u) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 1, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "BIG.BIN") != 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/", 2, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "SUB") != 0 || !is_dir) {
        return -1;
    }
    /* The DOS short-name duplicate must not appear: every file would be listed
     * twice, under two names, and a caller has no way to tell which is which. */
    if (vibeos_fs_list(&mnt, "/", 3, name, sizeof(name), &size, &is_dir) == 0) {
        return -1;
    }
    if (vibeos_fs_list(&mnt, "/sub", 0, name, sizeof(name), &size, &is_dir) != 0 ||
        strcmp(name, "INNER.DAT") != 0) {
        return -1;
    }
    return 0;
}

static int test_ntfs_refusals(void) {
    vibeos_ntfs_t fs;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_fsmount_t mnt;
    vibeos_fs_node_t node;
    uint8_t buf[VIBEOS_BLOCK_SIZE * 4u];

    if (nt_mount(&fs, &bc, &dev) != 0) {
        return -1;
    }
    /* Not NTFS. */
    nt_sec(0)[5] = 'X';
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ntfs_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A sector size the block cache cannot serve. */
    nt_build();
    nt_w16(nt_sec(0) + 11, 4096u);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ntfs_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A record size out of range. */
    nt_build();
    nt_sec(0)[0x40] = (uint8_t)(int8_t)(-40);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ntfs_mount(&fs, &bc, 0) == 0) {
        return -1;
    }

    /* A torn record: one sector of it carries a different sequence number, so
     * it was not written with the rest and its contents cannot be trusted. */
    nt_build();
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ntfs_mount(&fs, &bc, 0) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_ntfs_ops(), &fs, "ntfs") != 0) {
        return -1;
    }
    nt_w16(nt_record(6) + 2u * VIBEOS_BLOCK_SIZE - 2u, 0x1234u);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_fs_lookup(&mnt, "/resident.txt", &node) == 0) {
        return -1;
    }

    /* A record that is not a record. Without the magic check the fixups are
     * applied to whatever those bytes are and the result is decoded as a file. */
    nt_build();
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ntfs_mount(&fs, &bc, 0) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_ntfs_ops(), &fs, "ntfs") != 0) {
        return -1;
    }
    memcpy(nt_record(6), "XXXX", 4);
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_fs_lookup(&mnt, "/resident.txt", &node) == 0) {
        return -1;
    }

    /* ---- an attribute whose own fields do not fit inside it ---------------
     *
     * From an external security review. ntfs_find_attr bounds an attribute's
     * total length inside the record and nothing more; every field *inside* the
     * attribute came off the volume and was used as it arrived.
     *
     * The resident read path copies from `rec` - a four-kilobyte buffer on the
     * kernel stack - into the buffer a user read() supplied, so a resident
     * length chosen by the volume chose how much kernel stack to hand to an
     * unprivileged process. And storage.c tries NTFS during filesystem
     * recognition, so a USB stick is enough to reach it.
     *
     * Record 6 is /resident.txt, and its $DATA attribute starts after the
     * $FILE_NAME one; rather than assume an offset, walk the record the way the
     * driver does. */
    nt_build();
    vibeos_blockcache_invalidate(&bc);
    if (vibeos_ntfs_mount(&fs, &bc, 0) != 0 ||
        vibeos_fs_mount(&mnt, vibeos_ntfs_ops(), &fs, "ntfs") != 0) {
        return -1;
    }
    {
        uint8_t *rec6 = nt_record(6);
        uint32_t off = nt_r16(rec6 + 0x14);
        uint8_t *data = 0;

        while (off + 8u <= NT_RECORD) {
            uint32_t type = nt_r32(rec6 + off);
            uint32_t alen = nt_r32(rec6 + off + 4);
            if (type == 0xFFFFFFFFu || alen < 8u || off + alen > NT_RECORD) {
                break;
            }
            if (type == 0x80u) {          /* $DATA */
                data = rec6 + off;
                break;
            }
            off += alen;
        }
        if (!data) {
            return -1;                    /* the fixture changed shape */
        }

        /* A resident value that claims to be far longer than the attribute
         * holding it. Before the fix this became a read of 4096 bytes of kernel
         * stack into a user buffer; now the attribute is refused, so the file
         * has no readable length and the read yields nothing. */
        nt_w32(data + 0x10, 4096u);
        vibeos_blockcache_invalidate(&bc);
        if (vibeos_fs_lookup(&mnt, "/resident.txt", &node) == 0 && node.size != 0u) {
            return -1;
        }
        if (vibeos_fs_read_at(&mnt, &node, 0, buf, sizeof(buf)) > 0) {
            return -1;
        }

        /* A value offset pointing outside the attribute. */
        nt_build();
        vibeos_blockcache_invalidate(&bc);
        rec6 = nt_record(6);
        nt_w16(data + 0x14, 0xFFFFu);
        vibeos_blockcache_invalidate(&bc);
        /* Asserted the way the case above is, and the first version of these
         * two was not: it read "if the lookup succeeded AND the read returned
         * something", which passes silently whenever the lookup fails for an
         * unrelated reason. Removing the bound under test left both cases
         * green - which is how a test comes to assert nothing at all. */
        if (vibeos_fs_lookup(&mnt, "/resident.txt", &node) == 0 &&
            node.size != 0u) {
            return -1;
        }

        /* A value offset inside the attribute header, which would let the value
         * overlap the fields describing it. */
        nt_build();
        vibeos_blockcache_invalidate(&bc);
        nt_w16(data + 0x14, 4u);
        vibeos_blockcache_invalidate(&bc);
        /* Asserted the way the case above is, and the first version of these
         * two was not: it read "if the lookup succeeded AND the read returned
         * something", which passes silently whenever the lookup fails for an
         * unrelated reason. Removing the bound under test left both cases
         * green - which is how a test comes to assert nothing at all. */
        if (vibeos_fs_lookup(&mnt, "/resident.txt", &node) == 0 &&
            node.size != 0u) {
            return -1;
        }
    }

    /* ---- a run-list offset past the attribute ----------------------------
     *
     * runs_len was computed as (attribute length - run-list offset) with no
     * check that the offset was inside the attribute. Unsigned, so an offset
     * past the end wrapped to a run list of nearly four gigabytes, walked out
     * of a 4 KiB stack buffer. BIG.BIN is the non-resident file. */
    nt_build();
    vibeos_blockcache_invalidate(&bc);
    {
        uint8_t *rec7 = nt_record(7);
        uint32_t off = nt_r16(rec7 + 0x14);
        uint8_t *data = 0;

        while (off + 8u <= NT_RECORD) {
            uint32_t type = nt_r32(rec7 + off);
            uint32_t alen = nt_r32(rec7 + off + 4);
            if (type == 0xFFFFFFFFu || alen < 8u || off + alen > NT_RECORD) {
                break;
            }
            if (type == 0x80u) {
                data = rec7 + off;
                break;
            }
            off += alen;
        }
        if (data && data[8] != 0u) {
            uint32_t alen = nt_r32(data + 4);
            nt_w16(data + 0x20, (uint16_t)(alen + 16u));
            vibeos_blockcache_invalidate(&bc);
            if (vibeos_fs_lookup(&mnt, "BIG.BIN", &node) == 0 &&
                node.size != 0u) {
                return -1;
            }
        }
    }

    return 0;
}

/* ---------------------------------------------------------------- journal --
 * The claim being tested is not "the journal writes a journal", it is that a
 * machine losing power part way through an update comes back holding either
 * the old contents or the new ones, and never a mixture. So the device below
 * can be told to stop accepting writes after exactly N of them, and the test
 * walks N across every write the transaction performs.
 */

#define JT_SECTORS 64u
#define JT_JOURNAL_BASE 32u
#define JT_JOURNAL_BLOCKS 8u
#define JT_TARGETS 3u

/* A drive with a volatile cache of its own, which is what real ones have.
 * A write is acknowledged immediately but sits in `pending` until a flush;
 * only then does it reach `platter`, and the flush applies the pending writes
 * in an order the test chooses rather than the order they were issued. That
 * second part matters more than it looks: a journal that survives a power cut
 * only because the cache happened to write its blocks in a helpful order has
 * not been shown to survive anything. */
#define JT_PENDING_MAX 32u

static uint8_t g_jt_platter[JT_SECTORS][VIBEOS_BLOCK_SIZE];
static uint64_t g_jt_pending_lba[JT_PENDING_MAX];
static uint8_t g_jt_pending_data[JT_PENDING_MAX][VIBEOS_BLOCK_SIZE];
static uint32_t g_jt_pending;
static uint32_t g_jt_landed;      /* writes that reached the platter */
static uint32_t g_jt_power_off;   /* let this many land, then the power goes */
static uint32_t g_jt_order;       /* which flush order to use */

static int jt_read(void *ctx, uint64_t lba, void *buf)
{
    uint32_t i;

    (void)ctx;
    if (lba >= JT_SECTORS) {
        return -1;
    }
    /* The drive answers from its own cache, so a block written but not yet
     * flushed still reads back as the new contents. */
    for (i = g_jt_pending; i > 0u; i--) {
        if (g_jt_pending_lba[i - 1u] == lba) {
            memcpy(buf, g_jt_pending_data[i - 1u], VIBEOS_BLOCK_SIZE);
            return 0;
        }
    }
    memcpy(buf, g_jt_platter[lba], VIBEOS_BLOCK_SIZE);
    return 0;
}

static int jt_write(void *ctx, uint64_t lba, const void *buf)
{
    (void)ctx;
    if (lba >= JT_SECTORS || g_jt_pending >= JT_PENDING_MAX) {
        return -1;
    }
    g_jt_pending_lba[g_jt_pending] = lba;
    memcpy(g_jt_pending_data[g_jt_pending], buf, VIBEOS_BLOCK_SIZE);
    g_jt_pending++;
    return 0;
}

static int jt_flush(void *ctx)
{
    uint32_t n = g_jt_pending;
    uint32_t k;

    (void)ctx;
    for (k = 0; k < n; k++) {
        /* The loop condition already established that n is not zero. */
        uint32_t i = (k + g_jt_order) % n;

        if (g_jt_landed >= g_jt_power_off) {
            /* The power went during the flush. Whatever has not landed is
             * gone, and so is everything the drive still held. */
            g_jt_pending = 0;
            return -1;
        }
        memcpy(g_jt_platter[g_jt_pending_lba[i]], g_jt_pending_data[i],
               VIBEOS_BLOCK_SIZE);
        g_jt_landed++;
    }
    g_jt_pending = 0;
    return 0;
}

/* The lights come back: the drive's cache did not survive. */
static void jt_power_restore(void)
{
    g_jt_pending = 0;
    g_jt_power_off = 0xFFFFFFFFu;
}

static const uint64_t g_jt_target_lba[JT_TARGETS] = { 4u, 9u, 17u };

/* The volume starts with 'O' in every target and the transaction puts 'N'
 * there. Two states are acceptable afterwards and nothing else is. */
static void jt_reset(void)
{
    uint32_t i;

    memset(g_jt_platter, 0, sizeof(g_jt_platter));
    for (i = 0; i < JT_TARGETS; i++) {
        memset(g_jt_platter[g_jt_target_lba[i]], 'O', VIBEOS_BLOCK_SIZE);
    }
    g_jt_pending = 0;
    g_jt_landed = 0;
    g_jt_power_off = 0xFFFFFFFFu;
}

static void jt_attach(vibeos_blockdev_t *dev, vibeos_blockcache_t *bc,
                      vibeos_block_slot_t *slots, uint8_t *storage,
                      uint32_t slot_count)
{
    uint32_t i;

    memset(dev, 0, sizeof(*dev));
    dev->read = jt_read;
    dev->write = jt_write;
    dev->flush = jt_flush;
    dev->ctx = 0;
    dev->sectors = JT_SECTORS;
    for (i = 0; i < slot_count; i++) {
        slots[i].data = storage + (size_t)i * VIBEOS_BLOCK_SIZE;
    }
    vibeos_blockcache_init(bc, dev, slots, slot_count);
}

/* 0 = all targets old, 1 = all targets new, -1 = a mixture, which is the
 * failure this whole layer exists to prevent. */
static int jt_volume_state(void)
{
    uint32_t i;
    uint32_t j;
    int old_count = 0;
    int new_count = 0;

    for (i = 0; i < JT_TARGETS; i++) {
        const uint8_t *b = g_jt_platter[g_jt_target_lba[i]];
        int all_old = 1;
        int all_new = 1;

        for (j = 0; j < VIBEOS_BLOCK_SIZE; j++) {
            if (b[j] != 'O') {
                all_old = 0;
            }
            if (b[j] != 'N') {
                all_new = 0;
            }
        }
        if (all_old) {
            old_count++;
        } else if (all_new) {
            new_count++;
        } else {
            return -1;   /* one block half updated */
        }
    }
    if (old_count == (int)JT_TARGETS) {
        return 0;
    }
    if (new_count == (int)JT_TARGETS) {
        return 1;
    }
    return -1;
}

static int jt_run_transaction_on(const uint64_t *lbas)
{
    vibeos_blockdev_t dev;
    vibeos_blockcache_t bc;
    vibeos_block_slot_t slots[8];
    static uint8_t storage[8][VIBEOS_BLOCK_SIZE];
    vibeos_journal_t j;
    uint64_t targets[JT_JOURNAL_BLOCKS];
    static uint8_t staging[JT_JOURNAL_BLOCKS][VIBEOS_BLOCK_SIZE];
    uint8_t block[VIBEOS_BLOCK_SIZE];
    uint32_t i;

    jt_attach(&dev, &bc, slots, &storage[0][0], 8u);
    if (vibeos_journal_init(&j, &bc, JT_JOURNAL_BASE, JT_JOURNAL_BLOCKS,
                            targets, &staging[0][0]) != 0) {
        return -1;
    }
    if (vibeos_journal_begin(&j) != 0) {
        return -1;
    }
    memset(block, 'N', sizeof(block));
    for (i = 0; i < JT_TARGETS; i++) {
        if (vibeos_journal_stage(&j, lbas[i], block) != 0) {
            return -1;
        }
    }
    return vibeos_journal_commit(&j);
}

static int jt_run_transaction(void)
{
    return jt_run_transaction_on(g_jt_target_lba);
}

/* Mount after the lights come back: recovery decides what the volume holds. */
static int jt_recover(void)
{
    vibeos_blockdev_t dev;
    vibeos_blockcache_t bc;
    vibeos_block_slot_t slots[8];
    static uint8_t storage[8][VIBEOS_BLOCK_SIZE];
    vibeos_journal_t j;
    uint64_t targets[JT_JOURNAL_BLOCKS];
    static uint8_t staging[JT_JOURNAL_BLOCKS][VIBEOS_BLOCK_SIZE];

    jt_attach(&dev, &bc, slots, &storage[0][0], 8u);
    return vibeos_journal_init(&j, &bc, JT_JOURNAL_BASE, JT_JOURNAL_BLOCKS,
                               targets, &staging[0][0]);
}

static int test_journal_commit(void)
{
    jt_reset();
    if (jt_run_transaction() != 0) {
        return -1;
    }
    if (jt_volume_state() != 1) {
        return -1;
    }
    /* The region must not still claim a transaction is in flight, or every
     * later mount would replay this one over whatever came after it. */
    if (g_jt_platter[JT_JOURNAL_BASE][0] != 0u) {
        return -1;
    }
    return 0;
}

static int test_journal_power_cut(void)
{
    uint32_t total;
    uint32_t cut;
    uint32_t order;
    int saw_old = 0;
    int saw_new = 0;

    /* How many blocks a clean transaction lands. Cutting the power at each of
     * them in turn, under each flush order, is the sweep. */
    jt_reset();
    if (jt_run_transaction() != 0) {
        return -1;
    }
    total = g_jt_landed;
    if (total < 4u) {
        return -1;   /* too few to be exercising the phases at all */
    }

    for (order = 0; order < 6u; order++) {
        for (cut = 0; cut <= total; cut++) {
            int state;

            jt_reset();
            g_jt_order = order;
            g_jt_power_off = cut;
            (void)jt_run_transaction();   /* expected to fail for most cuts */

            jt_power_restore();
            if (jt_recover() != 0) {
                return -1;
            }

            state = jt_volume_state();
            if (state < 0) {
                return -1;   /* a mixture: the property is broken */
            }
            if (state == 0) {
                saw_old = 1;
            } else {
                saw_new = 1;
            }

            /* Recovery must also leave the region retired, so that mounting
             * twice does not replay a transaction already checkpointed. */
            if (g_jt_platter[JT_JOURNAL_BASE][0] != 0u) {
                return -1;
            }
            /* And a second mount must not change its mind. */
            if (jt_recover() != 0 || jt_volume_state() != state) {
                return -1;
            }
        }
    }
    g_jt_order = 0;

    /* If every cut produced the same answer the sweep proved nothing about
     * the commit point - either it never got far enough to commit, or it
     * committed before writing anything. */
    if (!saw_old || !saw_new) {
        return -1;
    }
    return 0;
}

/* A commit record left behind by an earlier transaction, sitting where the
 * current one's would go. Its data happens to match, so only the sequence
 * number distinguishes it - and the transaction it belongs to is finished,
 * which makes replaying it a change nobody asked for. */
static int test_journal_stale_commit(void)
{
    static const uint64_t others[JT_TARGETS] = { 5u, 11u, 19u };
    uint8_t old_commit[VIBEOS_BLOCK_SIZE];
    uint32_t cut;
    uint32_t i;
    int staged = 0;

    jt_reset();
    if (jt_run_transaction() != 0) {
        return -1;
    }
    /* Keep the finished transaction's commit record. Retiring the region only
     * clears the descriptor, so on a real volume this block is still lying
     * there when the next transaction writes its own descriptor over the old
     * one - and it carries the same sequence number, because the counter
     * restarts at every mount. */
    memcpy(old_commit, g_jt_platter[JT_JOURNAL_BASE + 1u + JT_TARGETS],
           VIBEOS_BLOCK_SIZE);

    /* A second transaction, over different blocks with identical contents, cut
     * after its descriptor and data are durable but before its own commit
     * record lands. */
    for (cut = 1u; cut < 24u; cut++) {
        jt_power_restore();
        g_jt_landed = 0;
        memset(g_jt_platter[JT_JOURNAL_BASE], 0, VIBEOS_BLOCK_SIZE);
        memcpy(g_jt_platter[JT_JOURNAL_BASE + 1u + JT_TARGETS], old_commit,
               VIBEOS_BLOCK_SIZE);
        for (i = 0; i < JT_TARGETS; i++) {
            memset(g_jt_platter[others[i]], 'O', VIBEOS_BLOCK_SIZE);
        }

        g_jt_power_off = cut;
        (void)jt_run_transaction_on(others);
        jt_power_restore();

        if (g_jt_platter[JT_JOURNAL_BASE][0] == 0x56u &&
            g_jt_platter[JT_JOURNAL_BASE + 1u + JT_TARGETS][0] == 0x56u &&
            g_jt_platter[others[0]][0] == 'O') {
            staged = 1;
            break;
        }
    }
    if (!staged) {
        return -1;   /* never reached the state this test is about */
    }

    if (jt_recover() != 0) {
        return -1;
    }
    for (i = 0; i < JT_TARGETS; i++) {
        if (g_jt_platter[others[i]][0] != 'O') {
            return -1;   /* an uncommitted transaction was replayed */
        }
    }
    return 0;
}

/* A commit record whose every field is right except the four bytes that say
 * it is one. The region is reused by transactions of different sizes, so the
 * block a short transaction's commit record lands on is where a longer one
 * kept file data - and that data must not be mistaken for a commit. */
static int test_journal_commit_magic(void)
{
    uint8_t good_commit[VIBEOS_BLOCK_SIZE];
    uint32_t cut;
    uint32_t i;
    int staged = 0;

    /* A transaction that runs to the end produces the commit record belonging
     * to this exact descriptor; keep it. */
    jt_reset();
    if (jt_run_transaction() != 0) {
        return -1;
    }
    memcpy(good_commit, g_jt_platter[JT_JOURNAL_BASE + 1u + JT_TARGETS],
           VIBEOS_BLOCK_SIZE);

    /* Now stop the same transaction before its commit record lands. */
    for (cut = 1u; cut < 24u; cut++) {
        jt_reset();
        g_jt_power_off = cut;
        (void)jt_run_transaction();
        jt_power_restore();
        /* The descriptor and every staged block must be on the platter, so
         * that nothing but the commit record is missing - otherwise the
         * checksum would be what rejects the transaction and this test would
         * be about the checksum instead. */
        if (g_jt_platter[JT_JOURNAL_BASE][0] == 0x56u &&
            jt_volume_state() == 0) {
            int complete = 1;

            for (i = 0; i < JT_TARGETS; i++) {
                if (g_jt_platter[JT_JOURNAL_BASE + 1u + i][0] != 'N') {
                    complete = 0;
                }
            }
            if (complete) {
                staged = 1;
                break;
            }
        }
    }
    if (!staged) {
        return -1;
    }

    memcpy(g_jt_platter[JT_JOURNAL_BASE + 1u + JT_TARGETS], good_commit,
           VIBEOS_BLOCK_SIZE);
    g_jt_platter[JT_JOURNAL_BASE + 1u + JT_TARGETS][0] ^= 0xFFu;

    if (jt_recover() != 0) {
        return -1;
    }
    for (i = 0; i < JT_TARGETS; i++) {
        if (g_jt_platter[g_jt_target_lba[i]][0] != 'O') {
            return -1;   /* replayed a transaction that never committed */
        }
    }
    return 0;
}

/* A descriptor claiming more targets than the region could ever hold. It was
 * never committed, so the only correct response is to drop it - but a reader
 * that believes the count walks off the end of the region first. */
static int test_journal_absurd_count(void)
{
    uint8_t desc[VIBEOS_BLOCK_SIZE];

    jt_reset();
    memset(desc, 0, sizeof(desc));
    desc[0] = 0x56u; desc[1] = 0x42u; desc[2] = 0x4Au; desc[3] = 0x31u;
    desc[4] = 0xFFu; desc[5] = 0xFFu;   /* count far beyond the region */
    memcpy(g_jt_platter[JT_JOURNAL_BASE], desc, sizeof(desc));

    if (jt_recover() != 0) {
        return -1;
    }
    if (jt_volume_state() != 0) {
        return -1;
    }
    if (g_jt_platter[JT_JOURNAL_BASE][0] != 0u) {
        return -1;   /* the bad descriptor is still there for the next mount */
    }
    return 0;
}

static int test_journal_refusals(void)
{
    vibeos_blockdev_t dev;
    vibeos_blockcache_t bc;
    vibeos_block_slot_t slots[8];
    static uint8_t storage[8][VIBEOS_BLOCK_SIZE];
    vibeos_journal_t j;
    uint64_t targets[JT_JOURNAL_BLOCKS];
    static uint8_t staging[JT_JOURNAL_BLOCKS][VIBEOS_BLOCK_SIZE];
    uint8_t block[VIBEOS_BLOCK_SIZE];
    uint32_t i;

    jt_reset();
    jt_attach(&dev, &bc, slots, &storage[0][0], 8u);

    /* A region with no room for a single target is not a small journal, it is
     * an unusable one. */
    if (vibeos_journal_init(&j, &bc, JT_JOURNAL_BASE, 2u, targets,
                            &staging[0][0]) == 0) {
        return -1;
    }
    if (vibeos_journal_init(&j, &bc, JT_JOURNAL_BASE, JT_JOURNAL_BLOCKS,
                            targets, &staging[0][0]) != 0) {
        return -1;
    }
    /* Staging outside a transaction has nowhere to go. */
    memset(block, 'N', sizeof(block));
    if (vibeos_journal_stage(&j, 4u, block) == 0) {
        return -1;
    }
    if (vibeos_journal_commit(&j) == 0) {
        return -1;
    }
    if (vibeos_journal_begin(&j) != 0 || vibeos_journal_begin(&j) == 0) {
        return -1;
    }
    /* More targets than the region holds must be refused while nothing has
     * been written, not discovered half way through the commit. */
    for (i = 0; i < j.max_targets; i++) {
        if (vibeos_journal_stage(&j, 100u + i, block) != 0) {
            return -1;
        }
    }
    if (vibeos_journal_stage(&j, 999u, block) == 0) {
        return -1;
    }
    /* Re-staging one that is already there is not a new target, so it must
     * still be accepted at the limit. */
    if (vibeos_journal_stage(&j, 100u, block) != 0) {
        return -1;
    }
    vibeos_journal_abort(&j);
    if (jt_volume_state() != 0) {
        return -1;   /* an aborted transaction touched the volume */
    }

    /* A commit record whose data no longer matches it. Stop the machine at
     * the first cut that leaves a committed transaction not yet checkpointed,
     * then damage a block inside the region. Recovery must discard the whole
     * transaction rather than checkpoint bytes it cannot vouch for. */
    for (i = 0; i < 64u; i++) {
        jt_reset();
        jt_power_restore();
        g_jt_power_off = i;
        (void)jt_run_transaction();
        jt_power_restore();
        if (g_jt_platter[JT_JOURNAL_BASE][0] == 0x56u &&
            g_jt_platter[JT_JOURNAL_BASE + 1u + JT_TARGETS][0] == 0x56u &&
            jt_volume_state() == 0) {
            break;
        }
    }
    if (i == 64u) {
        return -1;   /* no such moment: the sweep below would prove nothing */
    }
    g_jt_platter[JT_JOURNAL_BASE + 1u][7] ^= 0xFFu;
    jt_power_restore();
    if (jt_recover() != 0) {
        return -1;
    }
    if (jt_volume_state() != 0) {
        return -1;   /* damaged data was checkpointed anyway */
    }
    return 0;
}

/* ---------------------------------------------------------------- storage --
 * Bring-up: the step that takes a disk and produces something mounted. Every
 * piece under it was already tested; what was never tested is that they join
 * up, because until now nothing joined them.
 *
 * The interesting claim is not "an ext2 partition mounts as ext2" but that the
 * three drivers which did *not* recognise it stayed quiet. Probing is only
 * safe if refusing is reliable, so a volume claimed by the wrong driver is the
 * failure this checks for by name.
 */

#define ST_PART_LBA 64u
#define ST_SECTORS (ST_PART_LBA + (uint64_t)E2_BLOCKS * (E2_BLOCK / VIBEOS_BLOCK_SIZE))

static int g_st_partitioned;   /* serve an MBR at sector 0 */
static int g_st_garbage;       /* serve a disk with no filesystem on it */

static int st_dev_read(void *ctx, uint64_t lba, void *buf)
{
    (void)ctx;
    if (lba >= ST_SECTORS) {
        return -1;
    }
    memset(buf, 0, VIBEOS_BLOCK_SIZE);

    if (g_st_garbage) {
        /* Not zeroes: a driver that reads a zeroed field as "absent" would be
         * let off by an empty disk. This is plausible-looking rubbish. */
        memset(buf, 0x5A, VIBEOS_BLOCK_SIZE);
        return 0;
    }

    if (g_st_partitioned) {
        if (lba == 0) {
            uint8_t *pe = (uint8_t *)buf + 446;

            pe[0] = 0x00;          /* not bootable          */
            pe[4] = 0x83;          /* Linux                 */
            pe[8] = (uint8_t)ST_PART_LBA;
            pe[12] = (uint8_t)(E2_BLOCKS * (E2_BLOCK / VIBEOS_BLOCK_SIZE));
            ((uint8_t *)buf)[510] = 0x55;
            ((uint8_t *)buf)[511] = 0xAA;
            return 0;
        }
        if (lba < ST_PART_LBA) {
            return 0;   /* the gap before the partition */
        }
        memcpy(buf, g_e2 + (lba - ST_PART_LBA) * VIBEOS_BLOCK_SIZE,
               VIBEOS_BLOCK_SIZE);
        return 0;
    }

    /* Unpartitioned: the filesystem starts at sector zero. */
    if (lba >= (uint64_t)E2_BLOCKS * (E2_BLOCK / VIBEOS_BLOCK_SIZE)) {
        return 0;
    }
    memcpy(buf, g_e2 + lba * VIBEOS_BLOCK_SIZE, VIBEOS_BLOCK_SIZE);
    return 0;
}

static int st_scan(vibeos_storage_t *st, vibeos_blockcache_t *bc,
                   vibeos_blockdev_t *dev, vibeos_block_slot_t *slots,
                   uint8_t *storage, uint32_t slot_count)
{
    uint32_t i;

    e2_build();
    memset(dev, 0, sizeof(*dev));
    dev->read = st_dev_read;
    dev->sectors = ST_SECTORS;
    for (i = 0; i < slot_count; i++) {
        slots[i].data = storage + (size_t)i * VIBEOS_BLOCK_SIZE;
    }
    if (vibeos_blockcache_init(bc, dev, slots, slot_count) != 0) {
        return -1;
    }
    return vibeos_storage_scan(st, bc, ST_SECTORS);
}

static int test_storage_partitioned(void)
{
    static vibeos_storage_t st;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_block_slot_t slots[8];
    static uint8_t mem[8][VIBEOS_BLOCK_SIZE];
    vibeos_fsmount_t *root;
    vibeos_fs_node_t node;

    g_st_partitioned = 1;
    g_st_garbage = 0;
    if (st_scan(&st, &bc, &dev, slots, &mem[0][0], 8u) != 0) {
        return -1;
    }
    if (st.volume_count != 1u || st.mounted_count != 1u) {
        return -1;
    }
    if (st.volume[0].first_lba != ST_PART_LBA) {
        return -1;
    }
    /* By name: the point is that the other three refused it. */
    if (st.volume[0].fs_name == 0 ||
        strcmp(st.volume[0].fs_name, "ext2") != 0) {
        return -1;
    }
    /* And the mount is usable, not merely reported. A scan that filled in the
     * fields without producing something readable would pass every check
     * above. */
    root = vibeos_storage_first(&st);
    if (root == 0 || vibeos_fs_lookup(root, "/small", &node) != 0) {
        return -1;
    }
    return 0;
}

static int test_storage_unpartitioned(void)
{
    static vibeos_storage_t st;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_block_slot_t slots[8];
    static uint8_t mem[8][VIBEOS_BLOCK_SIZE];

    /* A bare filesystem from sector zero is ordinary on removable media, so it
     * must not need a partition table to be found. */
    g_st_partitioned = 0;
    g_st_garbage = 0;
    if (st_scan(&st, &bc, &dev, slots, &mem[0][0], 8u) != 0) {
        return -1;
    }
    if (st.volume_count != 1u || st.mounted_count != 1u) {
        return -1;
    }
    if (st.volume[0].first_lba != 0u ||
        strcmp(st.volume[0].fs_name, "ext2") != 0) {
        return -1;
    }
    return 0;
}

static int test_storage_claims_nothing(void)
{
    static vibeos_storage_t st;
    vibeos_blockcache_t bc;
    vibeos_blockdev_t dev;
    vibeos_block_slot_t slots[8];
    static uint8_t mem[8][VIBEOS_BLOCK_SIZE];

    /* A disk holding no filesystem any of these drivers implements. Probing is
     * only safe because refusing is reliable, so a driver claiming this is the
     * failure that matters - and it would not look like a failure at the time,
     * it would look like a mounted volume returning wrong bytes. */
    g_st_partitioned = 0;
    g_st_garbage = 1;
    if (st_scan(&st, &bc, &dev, slots, &mem[0][0], 8u) != 0) {
        return -1;   /* the scan itself must still complete */
    }
    if (st.mounted_count != 0u) {
        return -1;
    }
    if (vibeos_storage_first(&st) != 0) {
        return -1;
    }
    g_st_garbage = 0;
    return 0;
}

/*
 * Kernel test runner entry point.
 *
 * This function executes the full kernel/unit/integration test matrix in a
 * fixed order. Each test returns 0 on success and non-zero on failure.
 * Failures are counted and emitted as "FAIL:<test_name>" lines to make
 * diagnostics easy to parse in CI logs.
 *
 * Exit contract:
 *   - returns 0 and prints "ALL_TESTS_PASS" when no tests fail
 *   - returns 1 and prints "TEST_FAILURES=<n>" otherwise
 */
int test_mm_stats(void);
int test_frame(void);
int test_vmspace(void);
int test_vma(void);
int test_backing(void);
int test_rmap(void);
int test_reclaim(void);
int test_task(void);
int test_runq(void);
int test_lifetime(void);
int test_loader(void);
int test_account(void);
int test_sched_policy(void);
int test_forkguard(void);

int main(void) {
    int failures = 0;
    /* Run each test and accumulate failures while preserving full execution. */
#define RUN_TEST(fn) do { if ((fn)() != 0) { failures++; printf("FAIL:%s\n", #fn); } } while (0)
    /* Memory-management counters: defined in mm_stats_tests.c, kept out of
     * this file because it is already past eight thousand lines. */
    RUN_TEST(test_mm_stats);
    RUN_TEST(test_frame);
    RUN_TEST(test_vmspace);
    RUN_TEST(test_vma);
    RUN_TEST(test_backing);
    RUN_TEST(test_rmap);
    RUN_TEST(test_reclaim);
    RUN_TEST(test_task);
    RUN_TEST(test_runq);
    RUN_TEST(test_lifetime);
    RUN_TEST(test_loader);
    RUN_TEST(test_account);
    RUN_TEST(test_sched_policy);
    RUN_TEST(test_forkguard);
    RUN_TEST(test_pmm);
    RUN_TEST(test_pmm_reserve);
    RUN_TEST(test_blockcache_hits);
    RUN_TEST(test_blockcache_writeback);
    RUN_TEST(test_blockcache_eviction);
    RUN_TEST(test_blockcache_failures);
    RUN_TEST(test_partition_mbr);
    RUN_TEST(test_partition_gpt);
    RUN_TEST(test_partition_gpt_refusals);
    RUN_TEST(test_ext2_mount_and_lookup);
    RUN_TEST(test_ext2_read);
    RUN_TEST(test_ext2_list);
    RUN_TEST(test_ext2_refusals);
    RUN_TEST(test_iso_lookup_and_read);
    RUN_TEST(test_iso_list);
    RUN_TEST(test_iso_refusals);
    RUN_TEST(test_exfat_lookup_and_read);
    RUN_TEST(test_exfat_list);
    RUN_TEST(test_exfat_refusals);
    RUN_TEST(test_ntfs_mount_and_read);
    RUN_TEST(test_ntfs_list);
    RUN_TEST(test_ntfs_refusals);
    RUN_TEST(test_journal_commit);
    RUN_TEST(test_journal_power_cut);
    RUN_TEST(test_journal_stale_commit);
    RUN_TEST(test_journal_commit_magic);
    RUN_TEST(test_journal_absurd_count);
    RUN_TEST(test_journal_refusals);
    RUN_TEST(test_storage_partitioned);
    RUN_TEST(test_storage_unpartitioned);
    RUN_TEST(test_storage_claims_nothing);
    RUN_TEST(test_scheduler);
    RUN_TEST(test_scheduler_balanced);
    RUN_TEST(test_scheduler_wait_runtime);
    RUN_TEST(test_scheduler_qos_affinity);
    RUN_TEST(test_ipc);
    RUN_TEST(test_kernel_log);
    RUN_TEST(test_kmain);
    RUN_TEST(test_vm);
    RUN_TEST(test_vm_user_address_space_contract);
    RUN_TEST(test_interrupts);
    RUN_TEST(test_syscalls);
    RUN_TEST(test_services);
    RUN_TEST(test_servicemgr_and_drivers);
    RUN_TEST(test_user_api_and_bootloader);
    RUN_TEST(test_native_userland_abi);
    RUN_TEST(test_native_service_supervisor);
    RUN_TEST(test_native_service_spawn_hook);
    RUN_TEST(test_process_groups_and_sessions);
    RUN_TEST(test_process_orphan_adoption);
    RUN_TEST(test_bootloader_sanitized_map);
    RUN_TEST(test_bootloader_handoff_metadata);
    RUN_TEST(test_bootloader_firmware_tags_and_pe_plan);
    RUN_TEST(test_timer_and_idt);
    RUN_TEST(test_compat_runtime);
    RUN_TEST(test_waitset);
    RUN_TEST(test_waitset_timed);
    RUN_TEST(test_waitset_thread_integration);
    RUN_TEST(test_waitset_ownership);
    RUN_TEST(test_waitset_lifecycle);
    RUN_TEST(test_waitset_wake_policy);
    RUN_TEST(test_waitset_stats);
    RUN_TEST(test_waitset_priority_and_batch);
    RUN_TEST(test_waitset_wait_all_and_ext_stats);
    RUN_TEST(test_waitset_large_fan_in);
    RUN_TEST(test_filesystem_runtime);
    RUN_TEST(test_network_runtime);
    RUN_TEST(test_inet_checksum);
    RUN_TEST(test_tls_dependency);
    RUN_TEST(test_inet_arp_and_icmp);
    RUN_TEST(test_inet_udp);
    RUN_TEST(test_inet_dhcp_and_dns);
    RUN_TEST(test_inet_l4_checksum_rejection);
    RUN_TEST(test_network_route_and_firewall_policy);
    RUN_TEST(test_network_policy_data_path);
    RUN_TEST(test_inet_tcp_connection);
    RUN_TEST(test_inet_tcp_listen_accept);
    RUN_TEST(test_inet_udp_datagram_queue);
    RUN_TEST(test_inet_tcp_out_of_order);
    RUN_TEST(test_inet_tcp_close_reclaims_socket);
    RUN_TEST(test_inet_dhcp_lease_lifecycle);
    RUN_TEST(test_inet_dns_timeout_and_negative_cache);
    RUN_TEST(test_fat_chain_layouts);
    RUN_TEST(test_fat_chain_coalescing);
    RUN_TEST(test_fat_chain_short_and_failed);
    RUN_TEST(test_elf_parse_valid);
    RUN_TEST(test_elf_shared_page);
    RUN_TEST(test_elf_rejects_malformed);
    RUN_TEST(test_elf_bss_is_zeroed);
    RUN_TEST(test_elf_stack_layout);
    RUN_TEST(test_elf_stack_edges);
    RUN_TEST(test_elf_stack_carries_phdr);
    RUN_TEST(test_elf_parse_dyn_bias);
    RUN_TEST(test_elf_dyn_rejects_crafted);
    RUN_TEST(test_elf_interp_reported);
    RUN_TEST(test_elf_stack_at_base);
    RUN_TEST(test_security_token);
    RUN_TEST(test_security_audit_log);
    RUN_TEST(test_driver_host);
    RUN_TEST(test_service_ipc_contract);
    RUN_TEST(test_trap_dispatch);
    RUN_TEST(test_trap_fault_decisions);
    RUN_TEST(test_trap_debug_resumable);
    RUN_TEST(test_kernel_trap_fault_handling);
    RUN_TEST(test_ipc_handle_transfer);
    RUN_TEST(test_handle_lifecycle_hooks);
    RUN_TEST(test_cross_process_handle_dup_policy);
    RUN_TEST(test_process_lifecycle);
    RUN_TEST(test_process_relationships);
    RUN_TEST(test_thread_lifecycle_controls);
    RUN_TEST(test_process_wait_state_aggregation);
    RUN_TEST(test_process_thread_object_handles);
    RUN_TEST(test_process_security_tokens);
    RUN_TEST(test_handle_revocation_propagation);
    RUN_TEST(test_handle_revocation_scoped);
    RUN_TEST(test_handle_revocation_audit);
    RUN_TEST(test_proc_audit_retention_policy);
#undef RUN_TEST

    /* Emit a stable summary line and process exit code for automation. */
    if (failures == 0) {
        puts("ALL_TESTS_PASS");
        return 0;
    }
    printf("TEST_FAILURES=%d\n", failures);
    return 1;
}
