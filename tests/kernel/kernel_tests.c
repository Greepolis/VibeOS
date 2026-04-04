#include <stdio.h>
#include <string.h>

#include "vibeos/kernel.h"
#include "vibeos/bootloader.h"
#include "vibeos/driver_host.h"
#include "vibeos/drivers.h"
#include "vibeos/fs.h"
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

static int test_pmm(void) {
    vibeos_pmm_t pmm;
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
    return 0;
}

static int test_scheduler(void) {
    vibeos_scheduler_t sched;
    vibeos_thread_t t1 = { .id = 1, .cpu_hint = 0, .klass = VIBEOS_THREAD_NORMAL, .timeslice_ticks = 4 };
    vibeos_thread_t t2 = { .id = 2, .cpu_hint = 0, .klass = VIBEOS_THREAD_INTERACTIVE, .timeslice_ticks = 2 };
    vibeos_thread_t *out;
    if (vibeos_sched_init(&sched, 1) != 0) {
        return -1;
    }
    if (vibeos_sched_enqueue(&sched, &t1) != 0 || vibeos_sched_enqueue(&sched, &t2) != 0) {
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
    region.base = 0x100000;
    region.length = 0x200000;
    region.type = 1;
    region.reserved = 0;

    boot.version = VIBEOS_BOOTINFO_VERSION;
    boot.flags = 0;
    boot.memory_map_entries = 1;
    boot.memory_map = &region;
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
    const vibeos_vm_map_t *found;
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
    if (vibeos_vm_protect(&aspace, 0x400000, 0x2000, VIBEOS_VM_PERM_READ) != 0) {
        return -1;
    }
    found = vibeos_vm_lookup(&aspace, 0x400010);
    if (!found || found->perms != VIBEOS_VM_PERM_READ) {
        return -1;
    }
    if (vibeos_vm_unmap(&aspace, 0x400000, 0x2000) != 0) {
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
    if (acc != 32 || vibeos_intc_counter(&intc, 32) != 1) {
        return -1;
    }
    return 0;
}

static int test_syscalls(void) {
    vibeos_kernel_t kernel;
    vibeos_syscall_frame_t frame;
    uint32_t pid1 = 0;
    uint32_t pid2 = 0;
    uint32_t p1_handle = 0;
    uint32_t signal_handle = 0;
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
    vibeos_syscall_make_process_spawn(&frame, 0);
    if (vibeos_proc_init(&kernel.proc_table) != 0) {
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
    vibeos_syscall_make_thread_create(&frame, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
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
    vibeos_syscall_make_waitset_add_event(&frame, (uint32_t)frame.result, 100, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    vibeos_syscall_make_waitset_add_event(&frame, (uint32_t)frame.result, 200, pid1);
    if (vibeos_syscall_dispatch(&kernel, &frame) == 0) {
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
    return 0;
}

static int test_servicemgr_and_drivers(void) {
    vibeos_servicemgr_state_t mgr;
    vibeos_init_state_t init_state;
    vibeos_devmgr_state_t devmgr_state;
    vibeos_vfs_state_t vfs_state;
    vibeos_net_state_t net_state;
    vibeos_driver_framework_t fw;
    if (vibeos_servicemgr_start(&mgr, &init_state, &devmgr_state, &vfs_state, &net_state) != 0) {
        return -1;
    }
    if (init_state.started_services != 4) {
        return -1;
    }
    if (mgr.supervised_count != 4 || mgr.state != VIBEOS_SERVICE_RUNNING) {
        return -1;
    }
    if (vibeos_driver_framework_init(&fw) != 0) {
        return -1;
    }
    if (vibeos_driver_register(&fw, 100) != 0 || fw.count != 1) {
        return -1;
    }
    return 0;
}

static int test_user_api_and_bootloader(void) {
    vibeos_kernel_t kernel;
    vibeos_user_context_t user_ctx;
    uint32_t signal_handle = 0;
    vibeos_memory_region_t regions[1];
    vibeos_boot_info_t boot_info;
    memset(&kernel, 0, sizeof(kernel));
    regions[0].base = 0x200000;
    regions[0].length = 0x100000;
    regions[0].type = 1;
    regions[0].reserved = 0;

    if (vibeos_user_context_init(&user_ctx, 10, 1) != 0) {
        return -1;
    }
    if (user_ctx.pid != 10 || user_ctx.tid != 1) {
        return -1;
    }
    if (vibeos_bootloader_build_boot_info(&boot_info, regions, 1) != 0) {
        return -1;
    }
    if (boot_info.version != VIBEOS_BOOTINFO_VERSION || boot_info.memory_map_entries != 1) {
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
    return 0;
}

static int test_timer_and_idt(void) {
    vibeos_timer_t timer;
    vibeos_x86_64_idt_t idt;
    int timer_vec;
    if (vibeos_timer_init(&timer, 1000) != 0) {
        return -1;
    }
    vibeos_timer_tick(&timer);
    vibeos_timer_tick(&timer);
    if (vibeos_timer_ticks(&timer) != 2) {
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

static int test_filesystem_runtime(void) {
    vibeos_vfs_runtime_t rt;
    vibeos_policy_state_t policy;
    vibeos_security_token_t token;
    uint32_t mount_id;
    uint32_t fd;
    if (vibeos_vfs_runtime_init(&rt) != 0) {
        return -1;
    }
    if (vibeos_vfs_mount(&rt, &mount_id) != 0 || mount_id == 0) {
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
    return 0;
}

static int test_network_runtime(void) {
    vibeos_net_runtime_t rt;
    uint32_t sock;
    const char payload[] = "ping";
    if (vibeos_net_runtime_init(&rt) != 0) {
        return -1;
    }
    if (vibeos_socket_create(&rt, &sock) != 0 || sock == 0) {
        return -1;
    }
    if (vibeos_socket_bind(&rt, sock, 1234) != 0) {
        return -1;
    }
    if (vibeos_socket_send(&rt, sock, payload, sizeof(payload)) != 0) {
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
    return 0;
}

static int test_driver_host(void) {
    vibeos_driver_framework_t fw;
    vibeos_devmgr_state_t devmgr;
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
    if (vibeos_handle_table_init(&sender) != 0 || vibeos_handle_table_init(&receiver) != 0) {
        return -1;
    }
    if (vibeos_handle_alloc(&sender, VIBEOS_HANDLE_RIGHT_READ | VIBEOS_HANDLE_RIGHT_SIGNAL, &src_handle) != 0) {
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

int main(void) {
    int failures = 0;
#define RUN_TEST(fn) do { if ((fn)() != 0) { failures++; printf("FAIL:%s\n", #fn); } } while (0)
    RUN_TEST(test_pmm);
    RUN_TEST(test_scheduler);
    RUN_TEST(test_ipc);
    RUN_TEST(test_kmain);
    RUN_TEST(test_vm);
    RUN_TEST(test_interrupts);
    RUN_TEST(test_syscalls);
    RUN_TEST(test_services);
    RUN_TEST(test_servicemgr_and_drivers);
    RUN_TEST(test_user_api_and_bootloader);
    RUN_TEST(test_timer_and_idt);
    RUN_TEST(test_waitset);
    RUN_TEST(test_waitset_timed);
    RUN_TEST(test_waitset_ownership);
    RUN_TEST(test_waitset_lifecycle);
    RUN_TEST(test_filesystem_runtime);
    RUN_TEST(test_network_runtime);
    RUN_TEST(test_security_token);
    RUN_TEST(test_driver_host);
    RUN_TEST(test_service_ipc_contract);
    RUN_TEST(test_trap_dispatch);
    RUN_TEST(test_ipc_handle_transfer);
    RUN_TEST(test_cross_process_handle_dup_policy);
    RUN_TEST(test_process_lifecycle);
    RUN_TEST(test_process_thread_object_handles);
    RUN_TEST(test_handle_revocation_propagation);
    RUN_TEST(test_handle_revocation_scoped);
    RUN_TEST(test_handle_revocation_audit);
#undef RUN_TEST

    if (failures == 0) {
        puts("ALL_TESTS_PASS");
        return 0;
    }
    printf("TEST_FAILURES=%d\n", failures);
    return 1;
}
