#include "vibeos/bootloader.h"

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)(p[0] |
        ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24));
}

static uint64_t read_le64(const uint8_t *p) {
    return (uint64_t)p[0] |
        ((uint64_t)p[1] << 8) |
        ((uint64_t)p[2] << 16) |
        ((uint64_t)p[3] << 24) |
        ((uint64_t)p[4] << 32) |
        ((uint64_t)p[5] << 40) |
        ((uint64_t)p[6] << 48) |
        ((uint64_t)p[7] << 56);
}

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

static int range_end(uint64_t base, uint64_t size, uint64_t *out_end) {
    if (!out_end || size == 0) {
        return -1;
    }
    if (base > UINT64_MAX - size) {
        return -1;
    }
    *out_end = base + size;
    return 0;
}

static int find_region_type_for_range(const vibeos_boot_info_t *boot_info, uint64_t base, uint64_t size, uint32_t *out_region_type) {
    uint64_t i;
    uint64_t target_end = 0;
    if (!boot_info || !out_region_type || size == 0) {
        return -1;
    }
    if (range_end(base, size, &target_end) != 0) {
        return -1;
    }
    for (i = 0; i < boot_info->memory_map_entries; i++) {
        uint64_t region_finish = 0;
        if (region_end(&boot_info->memory_map[i], &region_finish) != 0) {
            return -1;
        }
        if (base >= boot_info->memory_map[i].base && target_end <= region_finish) {
            *out_region_type = boot_info->memory_map[i].type;
            return 0;
        }
    }
    return -1;
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
    boot_info->acpi_rsdp = 0;
    boot_info->smbios_entry = 0;
    boot_info->initrd_base = 0;
    boot_info->initrd_size = 0;
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
    if ((boot_info->acpi_rsdp == 0) != (boot_info->smbios_entry == 0)) {
        return -1;
    }
    if (boot_info->acpi_rsdp != 0) {
        uint32_t table_region_type = 0;
        if (find_region_type_for_range(boot_info, boot_info->acpi_rsdp, 16, &table_region_type) != 0) {
            return -1;
        }
        if (table_region_type == VIBEOS_MEMORY_REGION_MMIO) {
            return -1;
        }
        if (find_region_type_for_range(boot_info, boot_info->smbios_entry, 16, &table_region_type) != 0) {
            return -1;
        }
        if (table_region_type == VIBEOS_MEMORY_REGION_MMIO) {
            return -1;
        }
    }
    if (boot_info->initrd_size > 0) {
        uint32_t initrd_region_type = 0;
        uint64_t initrd_end = 0;
        if (boot_info->initrd_base == 0) {
            return -1;
        }
        if (range_end(boot_info->initrd_base, boot_info->initrd_size, &initrd_end) != 0) {
            return -1;
        }
        if (find_region_type_for_range(boot_info, boot_info->initrd_base, boot_info->initrd_size, &initrd_region_type) != 0) {
            return -1;
        }
        if (initrd_region_type == VIBEOS_MEMORY_REGION_MMIO) {
            return -1;
        }
    }
    if (boot_info->framebuffer_base == 0) {
        if (boot_info->framebuffer_width != 0 || boot_info->framebuffer_height != 0) {
            return -1;
        }
    } else {
        uint32_t fb_region_type = 0;
        if (boot_info->framebuffer_width == 0 || boot_info->framebuffer_height == 0) {
            return -1;
        }
        if (find_region_type_for_range(boot_info, boot_info->framebuffer_base, 4096, &fb_region_type) != 0) {
            return -1;
        }
        if (!(fb_region_type == VIBEOS_MEMORY_REGION_MMIO || fb_region_type == VIBEOS_MEMORY_REGION_RESERVED)) {
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

int vibeos_bootloader_set_firmware_tables(vibeos_boot_info_t *boot_info, uint64_t acpi_rsdp, uint64_t smbios_entry) {
    if (!boot_info) {
        return -1;
    }
    if ((acpi_rsdp == 0) != (smbios_entry == 0)) {
        return -1;
    }
    boot_info->acpi_rsdp = acpi_rsdp;
    boot_info->smbios_entry = smbios_entry;
    return 0;
}

int vibeos_bootloader_set_initrd(vibeos_boot_info_t *boot_info, uint64_t initrd_base, uint64_t initrd_size) {
    if (!boot_info) {
        return -1;
    }
    if (initrd_size > 0) {
        if (initrd_base == 0 || initrd_base > UINT64_MAX - initrd_size) {
            return -1;
        }
    }
    boot_info->initrd_base = initrd_base;
    boot_info->initrd_size = initrd_size;
    return 0;
}

int vibeos_bootloader_set_framebuffer(vibeos_boot_info_t *boot_info, uint64_t framebuffer_base, uint32_t width, uint32_t height) {
    if (!boot_info) {
        return -1;
    }
    if (framebuffer_base == 0) {
        if (width != 0 || height != 0) {
            return -1;
        }
    } else if (width == 0 || height == 0) {
        return -1;
    }
    boot_info->framebuffer_base = framebuffer_base;
    boot_info->framebuffer_width = width;
    boot_info->framebuffer_height = height;
    return 0;
}

int vibeos_bootloader_find_region_type_for_range(const vibeos_boot_info_t *boot_info, uint64_t base, uint64_t size, uint32_t *out_region_type) {
    if (!boot_info || !out_region_type) {
        return -1;
    }
    if (vibeos_bootloader_validate_boot_info(boot_info) != 0) {
        return -1;
    }
    return find_region_type_for_range(boot_info, base, size, out_region_type);
}

int vibeos_bootloader_extract_firmware_tags(const vibeos_firmware_tag_t *tags, uint32_t tag_count, uint64_t *out_acpi_rsdp, uint64_t *out_smbios_entry, uint32_t *out_secure_boot, uint32_t *out_measured_boot) {
    uint32_t i;
    uint64_t acpi = 0;
    uint64_t smbios = 0;
    uint32_t secure_boot = 0;
    uint32_t measured_boot = 0;
    if (!tags || !out_acpi_rsdp || !out_smbios_entry || !out_secure_boot || !out_measured_boot) {
        return -1;
    }
    for (i = 0; i < tag_count; i++) {
        switch (tags[i].type) {
            case VIBEOS_FIRMWARE_TAG_ACPI_RSDP:
                acpi = tags[i].value;
                break;
            case VIBEOS_FIRMWARE_TAG_SMBIOS:
                smbios = tags[i].value;
                break;
            case VIBEOS_FIRMWARE_TAG_SECURE_BOOT:
                secure_boot = tags[i].value ? 1u : 0u;
                break;
            case VIBEOS_FIRMWARE_TAG_MEASURED_BOOT:
                measured_boot = tags[i].value ? 1u : 0u;
                break;
            default:
                break;
        }
    }
    if ((acpi == 0) != (smbios == 0)) {
        return -1;
    }
    *out_acpi_rsdp = acpi;
    *out_smbios_entry = smbios;
    *out_secure_boot = secure_boot;
    *out_measured_boot = measured_boot;
    return 0;
}

int vibeos_bootloader_apply_firmware_tags(vibeos_boot_info_t *boot_info, const vibeos_firmware_tag_t *tags, uint32_t tag_count) {
    uint64_t acpi = 0;
    uint64_t smbios = 0;
    uint32_t secure_boot = 0;
    uint32_t measured_boot = 0;
    if (!boot_info || !tags) {
        return -1;
    }
    if (vibeos_bootloader_extract_firmware_tags(tags, tag_count, &acpi, &smbios, &secure_boot, &measured_boot) != 0) {
        return -1;
    }
    if (vibeos_bootloader_set_firmware_tables(boot_info, acpi, smbios) != 0) {
        return -1;
    }
    if (secure_boot) {
        boot_info->flags |= VIBEOS_BOOT_FLAG_SECURE_BOOT;
    } else {
        boot_info->flags &= ~VIBEOS_BOOT_FLAG_SECURE_BOOT;
    }
    if (measured_boot) {
        boot_info->flags |= VIBEOS_BOOT_FLAG_MEASURED_BOOT;
    } else {
        boot_info->flags &= ~VIBEOS_BOOT_FLAG_MEASURED_BOOT;
    }
    return vibeos_bootloader_validate_boot_info(boot_info);
}

int vibeos_bootloader_plan_pe_image(const uint8_t *image, uint64_t image_size, vibeos_boot_image_plan_t *out_plan) {
    uint32_t pe_offset;
    uint16_t num_sections;
    uint16_t optional_header_size;
    uint32_t entry_rva;
    uint64_t image_base;
    uint64_t section_table_offset;
    uint32_t i;
    if (!image || !out_plan || image_size < 0x100) {
        return -1;
    }
    if (image[0] != 'M' || image[1] != 'Z') {
        return -1;
    }
    pe_offset = read_le32(&image[0x3c]);
    if ((uint64_t)pe_offset + 24 > image_size) {
        return -1;
    }
    if (image[pe_offset] != 'P' || image[pe_offset + 1] != 'E' || image[pe_offset + 2] != 0 || image[pe_offset + 3] != 0) {
        return -1;
    }
    num_sections = read_le16(&image[pe_offset + 6]);
    optional_header_size = read_le16(&image[pe_offset + 20]);
    if (num_sections == 0 || num_sections > VIBEOS_BOOT_IMAGE_MAX_SEGMENTS) {
        return -1;
    }
    if ((uint64_t)pe_offset + 24 + optional_header_size > image_size) {
        return -1;
    }
    if (read_le16(&image[pe_offset + 24]) != 0x20Bu) {
        return -1;
    }
    entry_rva = read_le32(&image[pe_offset + 24 + 16]);
    image_base = read_le64(&image[pe_offset + 24 + 24]);
    section_table_offset = (uint64_t)pe_offset + 24u + (uint64_t)optional_header_size;
    if (section_table_offset + ((uint64_t)num_sections * 40u) > image_size) {
        return -1;
    }

    out_plan->image_base = image_base;
    out_plan->entry_point = image_base + (uint64_t)entry_rva;
    out_plan->segment_count = 0;

    for (i = 0; i < num_sections; i++) {
        uint64_t sh = section_table_offset + ((uint64_t)i * 40u);
        uint32_t virtual_size = read_le32(&image[sh + 8]);
        uint32_t virtual_address = read_le32(&image[sh + 12]);
        uint32_t raw_size = read_le32(&image[sh + 16]);
        uint32_t raw_offset = read_le32(&image[sh + 20]);
        uint32_t characteristics = read_le32(&image[sh + 36]);
        uint64_t mem_size = (virtual_size != 0) ? (uint64_t)virtual_size : (uint64_t)raw_size;
        vibeos_boot_image_segment_t *seg;
        if (mem_size == 0) {
            continue;
        }
        if (raw_size > 0 && ((uint64_t)raw_offset + (uint64_t)raw_size > image_size)) {
            return -1;
        }
        if (out_plan->segment_count >= VIBEOS_BOOT_IMAGE_MAX_SEGMENTS) {
            return -1;
        }
        seg = &out_plan->segments[out_plan->segment_count++];
        seg->file_offset = (uint64_t)raw_offset;
        seg->image_address = image_base + (uint64_t)virtual_address;
        seg->file_size = (uint64_t)raw_size;
        seg->mem_size = mem_size;
        seg->flags = 0;
        if ((characteristics & 0x40000000u) != 0) {
            seg->flags |= VIBEOS_BOOT_IMAGE_SEGMENT_READ;
        }
        if ((characteristics & 0x80000000u) != 0) {
            seg->flags |= VIBEOS_BOOT_IMAGE_SEGMENT_WRITE;
        }
        if ((characteristics & 0x20000000u) != 0) {
            seg->flags |= VIBEOS_BOOT_IMAGE_SEGMENT_EXEC;
        }
    }
    if (out_plan->segment_count == 0) {
        return -1;
    }
    return 0;
}
