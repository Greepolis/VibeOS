#include "vibeos/kernel.h"

/*
 * Host-side bring-up stub used until the real UEFI boot path is wired.
 * This keeps module contracts testable without blocking on firmware code.
 */
int vibeos_arch_boot_stub(vibeos_kernel_t *kernel, const vibeos_boot_info_t *boot_info) {
    return vibeos_kmain(kernel, boot_info);
}
