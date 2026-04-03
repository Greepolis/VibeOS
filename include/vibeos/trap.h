#ifndef VIBEOS_TRAP_H
#define VIBEOS_TRAP_H

#include <stdint.h>

typedef struct vibeos_trap_frame {
    uint64_t rip;
    uint64_t rsp;
    uint64_t rflags;
    uint64_t error_code;
    uint32_t vector;
} vibeos_trap_frame_t;

typedef struct vibeos_trap_state {
    uint32_t last_vector;
    uint64_t trap_count;
} vibeos_trap_state_t;

int vibeos_trap_state_init(vibeos_trap_state_t *state);
int vibeos_trap_dispatch(vibeos_trap_state_t *state, const vibeos_trap_frame_t *frame);

#endif
