#ifndef VIBEOS_UEFI_BOOT_HANDOFF_H
#define VIBEOS_UEFI_BOOT_HANDOFF_H

#include "vibeos/boot.h"
#include "vibeos/kernel.h"
#include "uefi_protocol.h"

typedef int (*kernel_entry_fn)(vibeos_kernel_t *kernel, const vibeos_boot_info_t *boot_info);

int uefi_boot_exit_services(EFI_SYSTEM_TABLE *st, EFI_HANDLE image_handle, uint32_t max_attempts);

/* Exit UEFI boot services and hand off to kernel */
int uefi_boot_handoff(EFI_SYSTEM_TABLE *st,
                      EFI_HANDLE image_handle,
                      vibeos_kernel_t *kernel,
                      vibeos_boot_info_t *boot_info,
                      kernel_entry_fn entry_point);

#endif
