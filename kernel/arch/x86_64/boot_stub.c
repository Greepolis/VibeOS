#include "vibeos/kernel.h"

/*
 * Host-side bring-up stub used until the real UEFI boot path is wired.
 * This keeps module contracts testable without blocking on firmware code.
 */
int vibeos_arch_boot_stub(vibeos_kernel_t *kernel, const vibeos_boot_info_t *boot_info) {
    if (vibeos_x86_64_validate_boot_environment(VIBEOS_X86_64_FEATURE_SSE2 | VIBEOS_X86_64_FEATURE_NX) != 0) {
        return -1;
    }
    return vibeos_kmain(kernel, boot_info);
}
