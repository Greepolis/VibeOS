#include "vibeos/bootloader.h"

int vibeos_bootloader_build_boot_info(vibeos_boot_info_t *boot_info, vibeos_memory_region_t *region_buffer, uint64_t region_count) {
    if (!boot_info || !region_buffer || region_count == 0) {
        return -1;
    }

    boot_info->version = VIBEOS_BOOTINFO_VERSION;
    boot_info->flags = 0;
    boot_info->memory_map_entries = region_count;
    boot_info->memory_map = region_buffer;
    boot_info->framebuffer_base = 0;
    boot_info->framebuffer_width = 0;
    boot_info->framebuffer_height = 0;
    return 0;
}

int vibeos_bootloader_validate_boot_info(const vibeos_boot_info_t *boot_info) {
    uint64_t i;
    if (!boot_info || boot_info->version != VIBEOS_BOOTINFO_VERSION || !boot_info->memory_map || boot_info->memory_map_entries == 0) {
        return -1;
    }
    for (i = 0; i < boot_info->memory_map_entries; i++) {
        if (boot_info->memory_map[i].length == 0) {
            return -1;
        }
    }
    return 0;
}

int vibeos_bootloader_memory_summary(const vibeos_boot_info_t *boot_info, uint64_t *out_total_bytes, uint64_t *out_usable_bytes) {
    uint64_t i;
    uint64_t total = 0;
    uint64_t usable = 0;
    if (!boot_info || !out_total_bytes || !out_usable_bytes) {
        return -1;
    }
    if (vibeos_bootloader_validate_boot_info(boot_info) != 0) {
        return -1;
    }
    for (i = 0; i < boot_info->memory_map_entries; i++) {
        total += boot_info->memory_map[i].length;
        if (boot_info->memory_map[i].type == 1u) {
            usable += boot_info->memory_map[i].length;
        }
    }
    *out_total_bytes = total;
    *out_usable_bytes = usable;
    return 0;
}

int vibeos_bootloader_count_region_type(const vibeos_boot_info_t *boot_info, uint32_t region_type, uint64_t *out_count) {
    uint64_t i;
    uint64_t count = 0;
    if (!boot_info || !out_count) {
        return -1;
    }
    if (vibeos_bootloader_validate_boot_info(boot_info) != 0) {
        return -1;
    }
    for (i = 0; i < boot_info->memory_map_entries; i++) {
        if (boot_info->memory_map[i].type == region_type) {
            count++;
        }
    }
    *out_count = count;
    return 0;
}

int vibeos_bootloader_has_overlap(const vibeos_boot_info_t *boot_info, uint32_t *out_has_overlap) {
    uint64_t i;
    uint64_t j;
    if (!boot_info || !out_has_overlap) {
        return -1;
    }
    if (vibeos_bootloader_validate_boot_info(boot_info) != 0) {
        return -1;
    }
    *out_has_overlap = 0;
    for (i = 0; i < boot_info->memory_map_entries; i++) {
        uint64_t a_start = boot_info->memory_map[i].base;
        uint64_t a_end = a_start + boot_info->memory_map[i].length;
        for (j = i + 1; j < boot_info->memory_map_entries; j++) {
            uint64_t b_start = boot_info->memory_map[j].base;
            uint64_t b_end = b_start + boot_info->memory_map[j].length;
            if (a_start < b_end && b_start < a_end) {
                *out_has_overlap = 1;
                return 0;
            }
        }
    }
    return 0;
}
