#include "vibeos/trap.h"

#define VIBEOS_TRAP_LOG_USER_FAULT 0x54554655u
#define VIBEOS_TRAP_LOG_KERNEL_FAULT 0x5454464bu

int vibeos_trap_state_init(vibeos_trap_state_t *state) {
    uint32_t i;
    if (!state) {
        return -1;
    }
    state->last_vector = 0;
    state->last_pid = 0;
    state->last_user_mode = 0;
    state->last_fault_address = 0;
    state->trap_count = 0;
    for (i = 0; i < 4; i++) {
        state->class_counts[i] = 0;
    }
    for (i = 0; i < 3; i++) {
        state->action_counts[i] = 0;
    }
    state->last_action = VIBEOS_TRAP_ACTION_CONTINUE;
    return 0;
}

vibeos_trap_class_t vibeos_trap_classify(uint32_t vector) {
    if (vector < 32u) {
        return VIBEOS_TRAP_CLASS_FAULT;
    }
    if (vector == VIBEOS_TRAP_VECTOR_SYSCALL) {
        return VIBEOS_TRAP_CLASS_SYSCALL;
    }
    if (vector < 240u) {
        return VIBEOS_TRAP_CLASS_INTERRUPT;
    }
    return VIBEOS_TRAP_CLASS_SPURIOUS;
}

int vibeos_trap_frame_is_user(const vibeos_trap_frame_t *frame) {
    if (!frame) {
        return 0;
    }
    return ((uint32_t)frame->cs & VIBEOS_TRAP_X86_CPL_MASK) == VIBEOS_TRAP_X86_USER_CPL;
}

static int trap_vector_is_resumable_debug(uint32_t vector) {
    /* #DB (1), #BP (3) and #OF (4) are trap-class exceptions whose saved RIP
     * already points past the trapping instruction, so execution can resume.
     * They must not be treated as fatal kernel faults. */
    return (vector == 1u || vector == 3u || vector == 4u) ? 1 : 0;
}

static vibeos_trap_action_t trap_action_for(vibeos_trap_class_t klass, uint32_t vector, uint32_t user_mode, uint32_t current_pid) {
    if (klass != VIBEOS_TRAP_CLASS_FAULT) {
        return VIBEOS_TRAP_ACTION_CONTINUE;
    }
    if (trap_vector_is_resumable_debug(vector)) {
        return VIBEOS_TRAP_ACTION_CONTINUE;
    }
    if (user_mode && current_pid != 0) {
        return VIBEOS_TRAP_ACTION_KILL_CURRENT;
    }
    return VIBEOS_TRAP_ACTION_PANIC;
}

static void trap_log_decision(vibeos_log_t *log, const vibeos_trap_decision_t *decision, const vibeos_trap_frame_t *frame) {
    if (!log || !decision || !frame || !log->initialized) {
        return;
    }
    if (decision->action == VIBEOS_TRAP_ACTION_KILL_CURRENT) {
        (void)vibeos_log_record(log,
            VIBEOS_LOG_ERROR,
            VIBEOS_TRAP_LOG_USER_FAULT,
            ((uint64_t)decision->current_pid << 32) | decision->vector,
            decision->fault_address,
            "user_fault_kill_process");
        return;
    }
    if (decision->action == VIBEOS_TRAP_ACTION_PANIC) {
        (void)vibeos_log_record(log,
            VIBEOS_LOG_FATAL,
            VIBEOS_TRAP_LOG_KERNEL_FAULT,
            ((uint64_t)decision->current_pid << 32) | decision->vector,
            frame->rip,
            "kernel_fault_panic");
    }
}

int vibeos_trap_dispatch_ex(vibeos_trap_state_t *state, const vibeos_trap_frame_t *frame, vibeos_log_t *log, uint32_t current_pid, vibeos_trap_decision_t *out_decision) {
    vibeos_trap_class_t klass;
    vibeos_trap_action_t action;
    uint32_t user_mode;
    if (!state || !frame || !out_decision) {
        return -1;
    }
    klass = vibeos_trap_classify(frame->vector);
    user_mode = (uint32_t)vibeos_trap_frame_is_user(frame);
    action = trap_action_for(klass, frame->vector, user_mode, current_pid);

    state->last_vector = frame->vector;
    state->last_pid = current_pid;
    state->last_user_mode = user_mode;
    state->last_fault_address = frame->fault_address;
    state->last_action = action;
    state->trap_count++;
    state->class_counts[(uint32_t)klass]++;
    state->action_counts[(uint32_t)action]++;

    out_decision->vector = frame->vector;
    out_decision->current_pid = current_pid;
    out_decision->user_mode = user_mode;
    out_decision->fault_address = frame->fault_address;
    out_decision->trap_class = klass;
    out_decision->action = action;

    trap_log_decision(log, out_decision, frame);
    return 0;
}

int vibeos_trap_dispatch(vibeos_trap_state_t *state, const vibeos_trap_frame_t *frame) {
    vibeos_trap_decision_t decision;
    return vibeos_trap_dispatch_ex(state, frame, 0, 0, &decision);
}
