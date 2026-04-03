#ifndef VIBEOS_SYSCALL_H
#define VIBEOS_SYSCALL_H

#include <stdint.h>

typedef enum vibeos_syscall_id {
    VIBEOS_SYSCALL_NOP = 0,
    VIBEOS_SYSCALL_EVENT_SIGNAL = 1,
    VIBEOS_SYSCALL_EVENT_CLEAR = 2,
    VIBEOS_SYSCALL_HANDLE_ALLOC = 3,
    VIBEOS_SYSCALL_HANDLE_CLOSE = 4,
    VIBEOS_SYSCALL_WAITSET_ADD_EVENT = 5,
    VIBEOS_SYSCALL_PROCESS_SPAWN = 10,
    VIBEOS_SYSCALL_THREAD_CREATE = 20,
    VIBEOS_SYSCALL_VM_MAP = 30,
    VIBEOS_SYSCALL_VM_UNMAP = 31,
    VIBEOS_SYSCALL_VM_PROTECT = 32
} vibeos_syscall_id_t;

typedef struct vibeos_syscall_frame {
    uint64_t id;
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    int64_t result;
} vibeos_syscall_frame_t;

struct vibeos_kernel;

int64_t vibeos_syscall_dispatch(struct vibeos_kernel *kernel, vibeos_syscall_frame_t *frame);

#endif
