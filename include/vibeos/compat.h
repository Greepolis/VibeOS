#ifndef VIBEOS_COMPAT_H
#define VIBEOS_COMPAT_H

#include <stdint.h>

typedef enum vibeos_compat_target {
    VIBEOS_COMPAT_TARGET_LINUX = 1,
    VIBEOS_COMPAT_TARGET_WINDOWS = 2,
    VIBEOS_COMPAT_TARGET_MACOS = 3
} vibeos_compat_target_t;

typedef struct vibeos_compat_runtime {
    uint32_t initialized;
    uint32_t linux_enabled;
    uint32_t windows_enabled;
    uint32_t macos_enabled;
    uint64_t translated_syscalls;
    uint64_t denied_syscalls;
} vibeos_compat_runtime_t;

int vibeos_compat_init(vibeos_compat_runtime_t *rt);
int vibeos_compat_enable(vibeos_compat_runtime_t *rt, vibeos_compat_target_t target, uint32_t enabled);
int vibeos_compat_translate_syscall(vibeos_compat_runtime_t *rt, vibeos_compat_target_t target, uint32_t foreign_syscall, uint32_t *out_native_syscall);
int vibeos_compat_stats(const vibeos_compat_runtime_t *rt, uint64_t *out_translated, uint64_t *out_denied);
int vibeos_linux_translate_syscall(vibeos_compat_runtime_t *rt, uint32_t foreign_syscall, uint32_t *out_native_syscall);
int vibeos_windows_translate_syscall(vibeos_compat_runtime_t *rt, uint32_t foreign_syscall, uint32_t *out_native_syscall);
int vibeos_macos_translate_syscall(vibeos_compat_runtime_t *rt, uint32_t foreign_syscall, uint32_t *out_native_syscall);

#endif
