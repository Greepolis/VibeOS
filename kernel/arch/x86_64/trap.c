#include "vibeos/trap.h"

int vibeos_trap_state_init(vibeos_trap_state_t *state) {
    if (!state) {
        return -1;
    }
    state->last_vector = 0;
    state->trap_count = 0;
    return 0;
}

int vibeos_trap_dispatch(vibeos_trap_state_t *state, const vibeos_trap_frame_t *frame) {
    if (!state || !frame) {
        return -1;
    }
    state->last_vector = frame->vector;
    state->trap_count++;
    return 0;
}
