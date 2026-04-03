#ifndef VIBEOS_KERNEL_H
#define VIBEOS_KERNEL_H

#include "vibeos/boot.h"
#include "vibeos/arch_x86_64.h"
#include "vibeos/interrupts.h"
#include "vibeos/ipc.h"
#include "vibeos/mm.h"
#include "vibeos/object.h"
#include "vibeos/proc.h"
#include "vibeos/scheduler.h"
#include "vibeos/timer.h"
#include "vibeos/vm.h"

typedef struct vibeos_kernel {
    vibeos_boot_state_t boot_state;
    vibeos_pmm_t pmm;
    vibeos_address_space_t kernel_aspace;
    vibeos_handle_table_t handles;
    vibeos_process_table_t proc_table;
    vibeos_scheduler_t scheduler;
    vibeos_interrupt_controller_t intc;
    vibeos_timer_t timer;
    vibeos_x86_64_idt_t idt;
    vibeos_event_t boot_event;
} vibeos_kernel_t;

int vibeos_kmain(vibeos_kernel_t *kernel, const vibeos_boot_info_t *boot_info);
const char *vibeos_kernel_stage_name(vibeos_boot_stage_t stage);

#endif
