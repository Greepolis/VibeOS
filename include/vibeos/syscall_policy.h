#ifndef VIBEOS_SYSCALL_POLICY_H
#define VIBEOS_SYSCALL_POLICY_H

#include <stdint.h>

#include "vibeos/syscall.h"

typedef struct vibeos_syscall_policy {
    uint32_t requires_handle;
    uint32_t required_rights;
} vibeos_syscall_policy_t;

vibeos_syscall_policy_t vibeos_syscall_policy_for(vibeos_syscall_id_t id);

#endif
