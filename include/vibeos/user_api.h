#ifndef VIBEOS_USER_API_H
#define VIBEOS_USER_API_H

#include <stdint.h>

typedef struct vibeos_user_context {
    uint32_t pid;
    uint32_t tid;
} vibeos_user_context_t;

int vibeos_user_context_init(vibeos_user_context_t *ctx, uint32_t pid, uint32_t tid);
int vibeos_user_signal_boot_event(void *kernel_ptr);

#endif
