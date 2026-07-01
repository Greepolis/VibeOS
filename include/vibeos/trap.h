#ifndef VIBEOS_TRAP_H
#define VIBEOS_TRAP_H

#include <stdint.h>

#include "vibeos/log.h"

#define VIBEOS_TRAP_VECTOR_GP_FAULT 13u
#define VIBEOS_TRAP_VECTOR_PAGE_FAULT 14u
#define VIBEOS_TRAP_VECTOR_SYSCALL 0x80u
#define VIBEOS_TRAP_X86_CPL_MASK 0x3u
#define VIBEOS_TRAP_X86_USER_CPL 0x3u

typedef enum vibeos_trap_class {
    VIBEOS_TRAP_CLASS_FAULT = 0,
    VIBEOS_TRAP_CLASS_INTERRUPT = 1,
    VIBEOS_TRAP_CLASS_SYSCALL = 2,
    VIBEOS_TRAP_CLASS_SPURIOUS = 3
} vibeos_trap_class_t;

typedef enum vibeos_trap_action {
    VIBEOS_TRAP_ACTION_CONTINUE = 0,
    VIBEOS_TRAP_ACTION_KILL_CURRENT = 1,
    VIBEOS_TRAP_ACTION_PANIC = 2
} vibeos_trap_action_t;

typedef struct vibeos_trap_frame {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t error_code;
    uint64_t cs;
    uint64_t fault_address;
    uint32_t vector;
} vibeos_trap_frame_t;

typedef struct vibeos_trap_decision {
    uint32_t vector;
    uint32_t current_pid;
    uint32_t user_mode;
    uint64_t fault_address;
    vibeos_trap_class_t trap_class;
    vibeos_trap_action_t action;
} vibeos_trap_decision_t;

typedef struct vibeos_trap_state {
    uint32_t last_vector;
    uint32_t last_pid;
    uint32_t last_user_mode;
    uint64_t last_fault_address;
    uint64_t trap_count;
    uint64_t class_counts[4];
    uint64_t action_counts[3];
    vibeos_trap_action_t last_action;
} vibeos_trap_state_t;

int vibeos_trap_state_init(vibeos_trap_state_t *state);
int vibeos_trap_dispatch(vibeos_trap_state_t *state, const vibeos_trap_frame_t *frame);
int vibeos_trap_dispatch_ex(vibeos_trap_state_t *state, const vibeos_trap_frame_t *frame, vibeos_log_t *log, uint32_t current_pid, vibeos_trap_decision_t *out_decision);
vibeos_trap_class_t vibeos_trap_classify(uint32_t vector);
int vibeos_trap_frame_is_user(const vibeos_trap_frame_t *frame);

#endif
