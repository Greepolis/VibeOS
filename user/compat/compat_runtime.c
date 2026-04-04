#include "vibeos/compat.h"

int vibeos_compat_init(vibeos_compat_runtime_t *rt) {
    if (!rt) {
        return -1;
    }
    rt->initialized = 1;
    rt->linux_enabled = 0;
    rt->windows_enabled = 0;
    rt->macos_enabled = 0;
    rt->translated_syscalls = 0;
    rt->denied_syscalls = 0;
    return 0;
}

int vibeos_compat_enable(vibeos_compat_runtime_t *rt, vibeos_compat_target_t target, uint32_t enabled) {
    if (!rt || !rt->initialized) {
        return -1;
    }
    switch (target) {
        case VIBEOS_COMPAT_TARGET_LINUX:
            rt->linux_enabled = enabled ? 1u : 0u;
            return 0;
        case VIBEOS_COMPAT_TARGET_WINDOWS:
            rt->windows_enabled = enabled ? 1u : 0u;
            return 0;
        case VIBEOS_COMPAT_TARGET_MACOS:
            rt->macos_enabled = enabled ? 1u : 0u;
            return 0;
        default:
            return -1;
    }
}

int vibeos_compat_translate_syscall(vibeos_compat_runtime_t *rt, vibeos_compat_target_t target, uint32_t foreign_syscall, uint32_t *out_native_syscall) {
    if (!rt || !rt->initialized || !out_native_syscall) {
        return -1;
    }
    switch (target) {
        case VIBEOS_COMPAT_TARGET_LINUX:
            return vibeos_linux_translate_syscall(rt, foreign_syscall, out_native_syscall);
        case VIBEOS_COMPAT_TARGET_WINDOWS:
            return vibeos_windows_translate_syscall(rt, foreign_syscall, out_native_syscall);
        case VIBEOS_COMPAT_TARGET_MACOS:
            return vibeos_macos_translate_syscall(rt, foreign_syscall, out_native_syscall);
        default:
            rt->denied_syscalls++;
            return -1;
    }
}

int vibeos_compat_stats(const vibeos_compat_runtime_t *rt, uint64_t *out_translated, uint64_t *out_denied) {
    if (!rt || !out_translated || !out_denied || !rt->initialized) {
        return -1;
    }
    *out_translated = rt->translated_syscalls;
    *out_denied = rt->denied_syscalls;
    return 0;
}
