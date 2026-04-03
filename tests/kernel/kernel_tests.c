#include <stdio.h>
#include <string.h>

#include "vibeos/kernel.h"

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

int main(void) {
    int failures = 0;
    failures += test_pmm() != 0;
    failures += test_scheduler() != 0;
    failures += test_ipc() != 0;
    failures += test_kmain() != 0;

    if (failures == 0) {
        puts("ALL_TESTS_PASS");
        return 0;
    }
    printf("TEST_FAILURES=%d\n", failures);
    return 1;
}
