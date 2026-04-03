#ifndef VIBEOS_KERNEL_H
#define VIBEOS_KERNEL_H

#include "vibeos/boot.h"
#include "vibeos/ipc.h"
#include "vibeos/mm.h"
#include "vibeos/scheduler.h"

typedef struct vibeos_kernel {
    vibeos_boot_state_t boot_state;
    vibeos_pmm_t pmm;
    vibeos_scheduler_t scheduler;
    vibeos_event_t boot_event;
} vibeos_kernel_t;

int vibeos_kmain(vibeos_kernel_t *kernel, const vibeos_boot_info_t *boot_info);
const char *vibeos_kernel_stage_name(vibeos_boot_stage_t stage);

#endif
