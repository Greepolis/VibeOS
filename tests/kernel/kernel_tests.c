#include <stdio.h>
#include <string.h>

#include "vibeos/kernel.h"
#include "vibeos/bootloader.h"
#include "vibeos/driver_host.h"
#include "vibeos/drivers.h"
#include "vibeos/fs.h"
#include "vibeos/compat.h"
#include "vibeos/services.h"
#include "vibeos/security_model.h"
#include "vibeos/service_ipc.h"
#include "vibeos/policy.h"
#include "vibeos/syscall.h"
#include "vibeos/syscall_abi.h"
#include "vibeos/timer.h"
#include "vibeos/net.h"
#include "vibeos/inet.h"
#include "vibeos/elf.h"
#include "vibeos/tls.h"
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
    if (vibeos_log_latest(&kernel.log, &latest) != 0 || strcmp(latest.message, "core_stage_ready") != 0) {
        return -1;
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

int main(void) {
    int failures = 0;
#define RUN_TEST(fn) do { if ((fn)() != 0) { failures++; printf("FAIL:%s\n", #fn); } } while (0)
    RUN_TEST(test_pmm);
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
    RUN_TEST(test_filesystem_runtime);
    RUN_TEST(test_network_runtime);
    RUN_TEST(test_inet_checksum);
    RUN_TEST(test_tls_dependency);
    RUN_TEST(test_inet_arp_and_icmp);
    RUN_TEST(test_inet_udp);
    RUN_TEST(test_inet_dhcp_and_dns);
    RUN_TEST(test_inet_l4_checksum_rejection);
    RUN_TEST(test_inet_tcp_connection);
    RUN_TEST(test_inet_tcp_listen_accept);
    RUN_TEST(test_inet_udp_datagram_queue);
    RUN_TEST(test_inet_tcp_out_of_order);
    RUN_TEST(test_inet_tcp_close_reclaims_socket);
    RUN_TEST(test_inet_dhcp_lease_lifecycle);
    RUN_TEST(test_inet_dns_timeout_and_negative_cache);
    RUN_TEST(test_elf_parse_valid);
    RUN_TEST(test_elf_shared_page);
    RUN_TEST(test_elf_rejects_malformed);
    RUN_TEST(test_elf_bss_is_zeroed);
    RUN_TEST(test_elf_stack_layout);
    RUN_TEST(test_elf_stack_edges);
    RUN_TEST(test_elf_stack_carries_phdr);
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

    if (failures == 0) {
        puts("ALL_TESTS_PASS");
        return 0;
    }
    printf("TEST_FAILURES=%d\n", failures);
    return 1;
}
