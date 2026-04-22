#ifndef VIBEOS_UEFI_BOOT_INFO_H
#define VIBEOS_UEFI_BOOT_INFO_H

#include "vibeos/boot.h"
#include "vibeos/kernel.h"
#include "vibeos/bootloader.h"
#include "uefi_protocol.h"
#include "uefi_pe_loader.h"

/* Allocate and build vibeos_boot_info_t structure with kernel parameters */
int uefi_boot_info_allocate(EFI_SYSTEM_TABLE *st, 
                             const vibeos_memory_region_t *memory_regions,
                             uint32_t memory_count,
                             uint64_t acpi_rsdp,
                             uint64_t smbios_entry,
                             const uefi_kernel_load_plan_t *kernel_plan,
                             vibeos_kernel_t **out_kernel,
                             vibeos_boot_info_t **out_boot_info);

/* Validate and normalize boot_info structure */
int uefi_boot_info_finalize(vibeos_kernel_t *kernel, vibeos_boot_info_t *boot_info);

#endif
