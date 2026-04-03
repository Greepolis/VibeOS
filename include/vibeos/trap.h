#ifndef VIBEOS_TRAP_H
#define VIBEOS_TRAP_H

#include <stdint.h>

typedef enum vibeos_trap_class {
    VIBEOS_TRAP_CLASS_FAULT = 0,
    VIBEOS_TRAP_CLASS_INTERRUPT = 1,
    VIBEOS_TRAP_CLASS_SYSCALL = 2,
    VIBEOS_TRAP_CLASS_SPURIOUS = 3
} vibeos_trap_class_t;

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
    uint64_t class_counts[4];
} vibeos_trap_state_t;

int vibeos_trap_state_init(vibeos_trap_state_t *state);
int vibeos_trap_dispatch(vibeos_trap_state_t *state, const vibeos_trap_frame_t *frame);
vibeos_trap_class_t vibeos_trap_classify(uint32_t vector);

#endif
