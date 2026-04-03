#include <stdio.h>
#include <string.h>

#include "vibeos/kernel.h"
#include "vibeos/bootloader.h"
#include "vibeos/drivers.h"
#include "vibeos/services.h"
#include "vibeos/syscall.h"
#include "vibeos/timer.h"
#include "vibeos/user_api.h"
#include "vibeos/vm.h"
#include "vibeos/waitset.h"

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
    memset(&kernel, 0, sizeof(kernel));
    vibeos_event_init(&kernel.boot_event);

    frame.id = VIBEOS_SYSCALL_EVENT_SIGNAL;
    frame.arg0 = 0;
    frame.arg1 = 0;
    frame.arg2 = 0;
    frame.result = -1;
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
        return -1;
    }
    if (!vibeos_event_is_signaled(&kernel.boot_event)) {
        return -1;
    }
    frame.id = VIBEOS_SYSCALL_PROCESS_SPAWN;
    frame.arg0 = 0;
    if (vibeos_proc_init(&kernel.proc_table) != 0) {
        return -1;
    }
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    frame.id = VIBEOS_SYSCALL_THREAD_CREATE;
    frame.arg0 = 1;
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result != 1) {
        return -1;
    }
    frame.id = VIBEOS_SYSCALL_HANDLE_ALLOC;
    frame.arg0 = 0x7;
    if (vibeos_handle_table_init(&kernel.handles) != 0) {
        return -1;
    }
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0 || frame.result <= 0) {
        return -1;
    }
    frame.id = VIBEOS_SYSCALL_HANDLE_CLOSE;
    frame.arg0 = (uint64_t)frame.result;
    if (vibeos_syscall_dispatch(&kernel, &frame) != 0) {
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
    if (vibeos_user_signal_boot_event(&kernel) != 0) {
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
    vibeos_event_t ev;
    size_t count = 0;
    vibeos_event_init(&ev);
    if (vibeos_waitset_init(&waitset) != 0) {
        return -1;
    }
    if (vibeos_waitset_add(&waitset, &ev) != 0) {
        return -1;
    }
    if (vibeos_waitset_count(&waitset, &count) != 0 || count != 1) {
        return -1;
    }
    return 0;
}

int main(void) {
    int failures = 0;
    failures += test_pmm() != 0;
    failures += test_scheduler() != 0;
    failures += test_ipc() != 0;
    failures += test_kmain() != 0;
    failures += test_vm() != 0;
    failures += test_interrupts() != 0;
    failures += test_syscalls() != 0;
    failures += test_services() != 0;
    failures += test_servicemgr_and_drivers() != 0;
    failures += test_user_api_and_bootloader() != 0;
    failures += test_timer_and_idt() != 0;
    failures += test_waitset() != 0;

    if (failures == 0) {
        puts("ALL_TESTS_PASS");
        return 0;
    }
    printf("TEST_FAILURES=%d\n", failures);
    return 1;
}
