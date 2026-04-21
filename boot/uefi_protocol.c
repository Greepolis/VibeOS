#include "uefi_protocol.h"
#include "uefi_memory.h"
#include "uefi_firmware.h"

/* Protocol wrappers delegate to specialized modules */

int uefi_acquire_memory_map(EFI_SYSTEM_TABLE *st, vibeos_memory_region_t *out_regions, uint64_t max_regions, uint64_t *out_count) {
    return uefi_memory_map_acquire(st, out_regions, max_regions, out_count);
}

int uefi_discover_firmware_tables(EFI_SYSTEM_TABLE *st, uint64_t *out_acpi_rsdp, uint64_t *out_smbios_entry) {
    return uefi_firmware_discover_tables(st, out_acpi_rsdp, out_smbios_entry);
}

int uefi_get_framebuffer(EFI_SYSTEM_TABLE *st, uint64_t *out_buffer_base, uint32_t *out_width, uint32_t *out_height) {
    if (!st || !out_buffer_base || !out_width || !out_height) {
        return -1;
    }
    /* Placeholder: will implement in Fase 3+ */
    *out_buffer_base = 0;
    *out_width = 0;
    *out_height = 0;
    return 0;
}
