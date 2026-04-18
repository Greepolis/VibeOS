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
    if (vibeos_pmm_init(&pmm, 0x100000, 8192, 4096) != 0) {
        return -1;
    }
    a = vibeos_pmm_alloc_page(&pmm);
    b = vibeos_pmm_alloc_page(&pmm);
    if (!a || !b || a == b) {
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
    return 0;
}

static int test_scheduler(void) {
    vibeos_scheduler_t sched;
    vibeos_thread_t t1 = { .id = 1, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 0 };
    vibeos_thread_t t2 = { .id = 2, .cpu_hint = 0, .klass = VIBEOS_THREAD_INTERACTIVE, .timeslice_ticks = 2 };
    vibeos_thread_t *out;
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

static int test_kmain(void) {
    vibeos_memory_region_t region;
    vibeos_boot_info_t boot;
    vibeos_kernel_t kernel;

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
    return 0;
}

static int test_vm(void) {
    vibeos_address_space_t aspace;
    vibeos_address_space_t cloned;
    const vibeos_vm_map_t *found;
    uint32_t merged = 0;
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
    return 0;
}

static int test_interrupts(void) {
    vibeos_interrupt_controller_t intc;
    uint32_t acc = 0;
    vibeos_intc_init(&intc);
    if (vibeos_intc_register(&intc, 32, irq_handler, &acc) != 0) {
        return -1;
    }
    if (vibeos_intc_dispatch(&intc, 32) != 0) {
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
    if (vibeos_intc_dispatch(&intc, 32) != 0) {
        return -1;
    }
    if (acc != 64 || vibeos_intc_counter(&intc, 32) != 2) {
        return -1;
    }
    return 0;
}

static int test_syscalls(void) {
    vibeos_kernel_t kernel;
    vibeos_syscall_frame_t frame;
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
    vibeos_handle_table_t *pid1_handles = 0;
    memset(&kernel, 0, sizeof(kernel));
    memset(&frame, 0, sizeof(frame));
    vibeos_event_init(&kernel.boot_event);

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
    if (vibeos_driver_register(&fw, 100) != 0 || fw.count != 1) {
        return -1;
    }
    if (vibeos_driver_register(&fw, 100) == 0) {
        return -1;
    }
    if (vibeos_driver_state(&fw, 100, &state) != 0 || state != VIBEOS_DRIVER_LOADED) {
        return -1;
    }
    if (vibeos_servicemgr_health(&mgr, &init_state, &devmgr_state, &vfs_state, &net_state, &running) != 0 || running != 5) {
        return -1;
    }
    if (vibeos_driver_unregister(&fw, 100) != 0 || fw.count != 0) {
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

static int test_user_api_and_bootloader(void) {
    vibeos_kernel_t kernel;
    vibeos_user_context_t user_ctx;
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

static int test_bootloader_firmware_tags_and_pe_plan(void) {
    vibeos_memory_region_t regions[2];
    vibeos_boot_info_t boot_info;
    vibeos_firmware_tag_t tags[4];
    vibeos_boot_image_plan_t plan;
    uint8_t image[1024];
    memset(&boot_info, 0, sizeof(boot_info));
    memset(tags, 0, sizeof(tags));
    memset(&plan, 0, sizeof(plan));
    memset(image, 0, sizeof(image));

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
    vibeos_thread_state_t state;
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
    if (vibeos_thread_state(&pt, tid, &state) != 0 || state != VIBEOS_THREAD_STATE_RUNNABLE) {
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
    vibeos_event_signal(&ev);
    if (vibeos_waitset_wait_timed_for_thread(&waitset, &timer, 1, &idx, &sched, 0, &pt, tid) != 0 || idx != 0) {
        return -1;
    }
    if (vibeos_thread_state(&pt, tid, &state) != 0 || state != VIBEOS_THREAD_STATE_RUNNABLE) {
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

static int test_filesystem_runtime(void) {
    vibeos_vfs_runtime_t rt;
    vibeos_policy_state_t policy;
    vibeos_security_token_t token;
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
    if (vibeos_net_stats(&rt, &total_tx, &total_rx, &open_sockets) != 0) {
        return -1;
    }
    if (total_tx != 1 || total_rx != 1 || open_sockets != 2) {
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

int main(void) {
    int failures = 0;
#define RUN_TEST(fn) do { if ((fn)() != 0) { failures++; printf("FAIL:%s\n", #fn); } } while (0)
    RUN_TEST(test_pmm);
    RUN_TEST(test_scheduler);
    RUN_TEST(test_scheduler_balanced);
    RUN_TEST(test_ipc);
    RUN_TEST(test_kmain);
    RUN_TEST(test_vm);
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
    RUN_TEST(test_filesystem_runtime);
    RUN_TEST(test_network_runtime);
    RUN_TEST(test_security_token);
    RUN_TEST(test_security_audit_log);
    RUN_TEST(test_driver_host);
    RUN_TEST(test_service_ipc_contract);
    RUN_TEST(test_trap_dispatch);
    RUN_TEST(test_ipc_handle_transfer);
    RUN_TEST(test_handle_lifecycle_hooks);
    RUN_TEST(test_cross_process_handle_dup_policy);
    RUN_TEST(test_process_lifecycle);
    RUN_TEST(test_process_relationships);
    RUN_TEST(test_thread_lifecycle_controls);
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
