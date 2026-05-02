#ifndef VIBEOS_BOOTLOADER_H
#define VIBEOS_BOOTLOADER_H

#include "vibeos/boot.h"

#define VIBEOS_BOOT_IMAGE_MAX_SEGMENTS 16u

typedef enum vibeos_firmware_tag_type {
    VIBEOS_FIRMWARE_TAG_ACPI_RSDP = 1,
    VIBEOS_FIRMWARE_TAG_SMBIOS = 2,
    VIBEOS_FIRMWARE_TAG_SECURE_BOOT = 3,
    VIBEOS_FIRMWARE_TAG_MEASURED_BOOT = 4
} vibeos_firmware_tag_type_t;

typedef struct vibeos_firmware_tag {
    uint32_t type;
    uint32_t reserved;
    uint64_t value;
} vibeos_firmware_tag_t;

typedef enum vibeos_boot_image_segment_flags {
    VIBEOS_BOOT_IMAGE_SEGMENT_READ = 1u << 0,
    VIBEOS_BOOT_IMAGE_SEGMENT_WRITE = 1u << 1,
    VIBEOS_BOOT_IMAGE_SEGMENT_EXEC = 1u << 2
} vibeos_boot_image_segment_flags_t;

typedef struct vibeos_boot_image_segment {
    uint64_t file_offset;
    uint64_t image_address;
    uint64_t file_size;
    uint64_t mem_size;
    uint32_t flags;
} vibeos_boot_image_segment_t;

typedef struct vibeos_boot_image_plan {
    uint64_t image_base;
    uint64_t entry_point;
    uint32_t segment_count;
    vibeos_boot_image_segment_t segments[VIBEOS_BOOT_IMAGE_MAX_SEGMENTS];
} vibeos_boot_image_plan_t;

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
int vibeos_bootloader_extract_firmware_tags(const vibeos_firmware_tag_t *tags, uint32_t tag_count, uint64_t *out_acpi_rsdp, uint64_t *out_smbios_entry, uint32_t *out_secure_boot, uint32_t *out_measured_boot);
int vibeos_bootloader_apply_firmware_tags(vibeos_boot_info_t *boot_info, const vibeos_firmware_tag_t *tags, uint32_t tag_count);
int vibeos_bootloader_plan_elf_image(const uint8_t *image, uint64_t image_size, vibeos_boot_image_plan_t *out_plan);
int vibeos_bootloader_plan_pe_image(const uint8_t *image, uint64_t image_size, vibeos_boot_image_plan_t *out_plan);

#endif
