#include "vibeos/object.h"
#include "vibeos/syscall_policy.h"

vibeos_syscall_policy_t vibeos_syscall_policy_for(vibeos_syscall_id_t id) {
    vibeos_syscall_policy_t p;
    p.requires_handle = 0;
    p.required_rights = 0;

    switch (id) {
        case VIBEOS_SYSCALL_EVENT_SIGNAL:
        case VIBEOS_SYSCALL_EVENT_CLEAR:
        case VIBEOS_SYSCALL_WAITSET_ADD_EVENT:
            p.requires_handle = 1;
            p.required_rights = VIBEOS_HANDLE_RIGHT_SIGNAL;
            return p;
        case VIBEOS_SYSCALL_HANDLE_CLOSE:
            p.requires_handle = 1;
            p.required_rights = VIBEOS_HANDLE_RIGHT_MANAGE;
            return p;
        default:
            return p;
    }
}
