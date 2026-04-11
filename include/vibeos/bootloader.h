#ifndef VIBEOS_BOOTLOADER_H
#define VIBEOS_BOOTLOADER_H

#include "vibeos/boot.h"

int vibeos_bootloader_build_boot_info(vibeos_boot_info_t *boot_info, vibeos_memory_region_t *region_buffer, uint64_t region_count);
int vibeos_bootloader_validate_boot_info(const vibeos_boot_info_t *boot_info);
int vibeos_bootloader_memory_summary(const vibeos_boot_info_t *boot_info, uint64_t *out_total_bytes, uint64_t *out_usable_bytes);
int vibeos_bootloader_count_region_type(const vibeos_boot_info_t *boot_info, uint32_t region_type, uint64_t *out_count);
int vibeos_bootloader_has_overlap(const vibeos_boot_info_t *boot_info, uint32_t *out_has_overlap);

#endif
