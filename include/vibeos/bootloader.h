#ifndef VIBEOS_BOOTLOADER_H
#define VIBEOS_BOOTLOADER_H

#include "vibeos/boot.h"

int vibeos_bootloader_build_boot_info(vibeos_boot_info_t *boot_info, vibeos_memory_region_t *region_buffer, uint64_t region_count);
int vibeos_bootloader_build_boot_info_sanitized(vibeos_boot_info_t *boot_info, const vibeos_memory_region_t *input_regions, uint64_t input_count, vibeos_memory_region_t *scratch_buffer, uint64_t scratch_capacity, uint64_t *out_sanitized_count);
int vibeos_bootloader_validate_boot_info(const vibeos_boot_info_t *boot_info);
int vibeos_bootloader_memory_summary(const vibeos_boot_info_t *boot_info, uint64_t *out_total_bytes, uint64_t *out_usable_bytes);
int vibeos_bootloader_count_region_type(const vibeos_boot_info_t *boot_info, uint32_t region_type, uint64_t *out_count);
int vibeos_bootloader_has_overlap(const vibeos_boot_info_t *boot_info, uint32_t *out_has_overlap);
int vibeos_bootloader_max_physical_address(const vibeos_boot_info_t *boot_info, uint64_t *out_max_physical_exclusive);
int vibeos_bootloader_set_firmware_tables(vibeos_boot_info_t *boot_info, uint64_t acpi_rsdp, uint64_t smbios_entry);
int vibeos_bootloader_set_initrd(vibeos_boot_info_t *boot_info, uint64_t initrd_base, uint64_t initrd_size);
int vibeos_bootloader_set_framebuffer(vibeos_boot_info_t *boot_info, uint64_t framebuffer_base, uint32_t width, uint32_t height);
int vibeos_bootloader_find_region_type_for_range(const vibeos_boot_info_t *boot_info, uint64_t base, uint64_t size, uint32_t *out_region_type);

#endif
