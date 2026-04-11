#include "vibeos/bootloader.h"

static int region_end(const vibeos_memory_region_t *region, uint64_t *out_end) {
    if (!region || !out_end || region->length == 0) {
        return -1;
    }
    if (region->base > UINT64_MAX - region->length) {
        return -1;
    }
    *out_end = region->base + region->length;
    return 0;
}

static void sort_regions_by_base(vibeos_memory_region_t *regions, uint64_t count) {
    uint64_t i;
    uint64_t j;
    for (i = 0; i < count; i++) {
        for (j = i + 1; j < count; j++) {
            if (regions[j].base < regions[i].base) {
                vibeos_memory_region_t tmp = regions[i];
                regions[i] = regions[j];
                regions[j] = tmp;
            }
        }
    }
}

static int merge_adjacent_regions(vibeos_memory_region_t *regions, uint64_t count, uint64_t *out_count) {
    uint64_t i;
    uint64_t write_index;
    if (!regions || !out_count) {
        return -1;
    }
    if (count == 0) {
        *out_count = 0;
        return 0;
    }
    write_index = 0;
    for (i = 1; i < count; i++) {
        uint64_t write_end = 0;
        if (region_end(&regions[write_index], &write_end) != 0) {
            return -1;
        }
        if (regions[write_index].type == regions[i].type && write_end == regions[i].base) {
            regions[write_index].length += regions[i].length;
            continue;
        }
        write_index++;
        regions[write_index] = regions[i];
    }
    *out_count = write_index + 1;
    return 0;
}

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
        uint64_t end = 0;
        if (region_end(&boot_info->memory_map[i], &end) != 0) {
            return -1;
        }
        if (i > 0) {
            uint64_t prev_end = 0;
            if (region_end(&boot_info->memory_map[i - 1], &prev_end) != 0) {
                return -1;
            }
            if (boot_info->memory_map[i].base < boot_info->memory_map[i - 1].base) {
                return -1;
            }
            if (boot_info->memory_map[i].base < prev_end) {
                return -1;
            }
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

int vibeos_bootloader_max_physical_address(const vibeos_boot_info_t *boot_info, uint64_t *out_max_physical_exclusive) {
    uint64_t i;
    uint64_t max_addr = 0;
    if (!boot_info || !out_max_physical_exclusive) {
        return -1;
    }
    if (vibeos_bootloader_validate_boot_info(boot_info) != 0) {
        return -1;
    }
    for (i = 0; i < boot_info->memory_map_entries; i++) {
        uint64_t end = 0;
        if (region_end(&boot_info->memory_map[i], &end) != 0) {
            return -1;
        }
        if (end > max_addr) {
            max_addr = end;
        }
    }
    *out_max_physical_exclusive = max_addr;
    return 0;
}

int vibeos_bootloader_build_boot_info_sanitized(vibeos_boot_info_t *boot_info, const vibeos_memory_region_t *input_regions, uint64_t input_count, vibeos_memory_region_t *scratch_buffer, uint64_t scratch_capacity, uint64_t *out_sanitized_count) {
    uint64_t i;
    uint64_t write_count = 0;
    uint64_t merged_count = 0;
    if (!boot_info || !input_regions || !scratch_buffer || !out_sanitized_count || input_count == 0 || scratch_capacity == 0) {
        return -1;
    }
    for (i = 0; i < input_count; i++) {
        uint64_t end = 0;
        if (region_end(&input_regions[i], &end) != 0) {
            return -1;
        }
        if (input_regions[i].type == VIBEOS_MEMORY_REGION_USABLE ||
            input_regions[i].type == VIBEOS_MEMORY_REGION_RESERVED ||
            input_regions[i].type == VIBEOS_MEMORY_REGION_ACPI_RECLAIMABLE ||
            input_regions[i].type == VIBEOS_MEMORY_REGION_ACPI_NVS ||
            input_regions[i].type == VIBEOS_MEMORY_REGION_MMIO) {
            if (write_count >= scratch_capacity) {
                return -1;
            }
            scratch_buffer[write_count] = input_regions[i];
            write_count++;
        }
    }
    if (write_count == 0) {
        return -1;
    }
    sort_regions_by_base(scratch_buffer, write_count);
    for (i = 1; i < write_count; i++) {
        uint64_t prev_end = 0;
        if (region_end(&scratch_buffer[i - 1], &prev_end) != 0) {
            return -1;
        }
        if (scratch_buffer[i].base < prev_end) {
            return -1;
        }
    }
    if (merge_adjacent_regions(scratch_buffer, write_count, &merged_count) != 0) {
        return -1;
    }
    if (vibeos_bootloader_build_boot_info(boot_info, scratch_buffer, merged_count) != 0) {
        return -1;
    }
    *out_sanitized_count = merged_count;
    return vibeos_bootloader_validate_boot_info(boot_info);
}
