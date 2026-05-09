#ifndef VIBEOS_KERNEL_H
#define VIBEOS_KERNEL_H

#include "vibeos/boot.h"
#include "vibeos/arch_x86_64.h"
#include "vibeos/interrupts.h"
#include "vibeos/ipc.h"
#include "vibeos/mm.h"
#include "vibeos/object.h"
#include "vibeos/policy.h"
#include "vibeos/proc.h"
#include "vibeos/scheduler.h"
#include "vibeos/security_model.h"
#include "vibeos/timer.h"
#include "vibeos/trap.h"
#include "vibeos/vm.h"

#define VIBEOS_BOOT_HEALTH_PMM_READY (1u << 0)
#define VIBEOS_BOOT_HEALTH_VM_READY (1u << 1)
#define VIBEOS_BOOT_HEALTH_HANDLES_READY (1u << 2)
#define VIBEOS_BOOT_HEALTH_POLICY_READY (1u << 3)
#define VIBEOS_BOOT_HEALTH_SCHED_READY (1u << 4)
#define VIBEOS_BOOT_HEALTH_PROC_READY (1u << 5)
#define VIBEOS_BOOT_HEALTH_IRQ_READY (1u << 6)
#define VIBEOS_BOOT_HEALTH_BOOT_EVENT_SIGNALLED (1u << 7)

typedef struct vibeos_kernel {
    vibeos_boot_state_t boot_state;
    uint32_t boot_health_flags;
    uint32_t boot_failure_fatal;
    vibeos_pmm_t pmm;
    vibeos_address_space_t kernel_aspace;
    vibeos_handle_table_t handles;
    vibeos_policy_state_t policy;
    vibeos_security_token_t kernel_token;
    vibeos_security_audit_log_t sec_audit;
    vibeos_process_table_t proc_table;
    vibeos_scheduler_t scheduler;
    vibeos_interrupt_controller_t intc;
    vibeos_timer_t timer;
    vibeos_x86_64_idt_t idt;
    vibeos_trap_state_t trap_state;
    vibeos_event_t boot_event;
} vibeos_kernel_t;

int vibeos_kmain(vibeos_kernel_t *kernel, const vibeos_boot_info_t *boot_info);
const char *vibeos_kernel_stage_name(vibeos_boot_stage_t stage);
int vibeos_kernel_boot_health(const vibeos_kernel_t *kernel, uint32_t *out_health_flags, uint32_t *out_failure_fatal);

#endif
