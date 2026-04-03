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
