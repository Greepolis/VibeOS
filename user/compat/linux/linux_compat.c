#include "vibeos/compat.h"

int vibeos_linux_translate_syscall(vibeos_compat_runtime_t *rt, uint32_t foreign_syscall, uint32_t *out_native_syscall) {
    if (!rt || !out_native_syscall || !rt->initialized) {
        return -1;
    }
    if (!rt->linux_enabled) {
        rt->denied_syscalls++;
        return -1;
    }
    switch (foreign_syscall) {
        case 39u: /* getpid */
            *out_native_syscall = 14u; /* PROCESS_COUNT_GET placeholder in host model */
            break;
        case 56u: /* clone */
            *out_native_syscall = 10u; /* PROCESS_SPAWN */
            break;
        default:
            rt->denied_syscalls++;
            return -1;
    }
    rt->translated_syscalls++;
    return 0;
}
