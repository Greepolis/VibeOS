#include "vibeos/trap.h"

int vibeos_trap_state_init(vibeos_trap_state_t *state) {
    uint32_t i;
    if (!state) {
        return -1;
    }
    state->last_vector = 0;
    state->trap_count = 0;
    for (i = 0; i < 4; i++) {
        state->class_counts[i] = 0;
    }
    return 0;
}

vibeos_trap_class_t vibeos_trap_classify(uint32_t vector) {
    if (vector < 32u) {
        return VIBEOS_TRAP_CLASS_FAULT;
    }
    if (vector == 0x80u) {
        return VIBEOS_TRAP_CLASS_SYSCALL;
    }
    if (vector < 240u) {
        return VIBEOS_TRAP_CLASS_INTERRUPT;
    }
    return VIBEOS_TRAP_CLASS_SPURIOUS;
}

int vibeos_trap_dispatch(vibeos_trap_state_t *state, const vibeos_trap_frame_t *frame) {
    vibeos_trap_class_t klass;
    if (!state || !frame) {
        return -1;
    }
    state->last_vector = frame->vector;
    state->trap_count++;
    klass = vibeos_trap_classify(frame->vector);
    state->class_counts[(uint32_t)klass]++;
    return 0;
}
