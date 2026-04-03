#ifndef VIBEOS_BOOTLOADER_H
#define VIBEOS_BOOTLOADER_H

#include "vibeos/boot.h"

int vibeos_bootloader_build_boot_info(vibeos_boot_info_t *boot_info, vibeos_memory_region_t *region_buffer, uint64_t region_count);

#endif
