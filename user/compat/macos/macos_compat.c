#include "vibeos/compat.h"

int vibeos_macos_translate_syscall(vibeos_compat_runtime_t *rt, uint32_t foreign_syscall, uint32_t *out_native_syscall) {
    if (!rt || !out_native_syscall || !rt->initialized) {
        return -1;
    }
    if (!rt->macos_enabled) {
        rt->denied_syscalls++;
        return -1;
    }
    switch (foreign_syscall) {
        case 5u: /* open */
            *out_native_syscall = 3u; /* HANDLE_ALLOC placeholder */
            break;
        case 2u: /* fork */
            *out_native_syscall = 10u; /* PROCESS_SPAWN */
            break;
        default:
            rt->denied_syscalls++;
            return -1;
    }
    rt->translated_syscalls++;
    return 0;
}
