#include "uefi_protocol.h"

/* Stub implementations for Fase 1 - will be expanded in Fase 2 */

static EFI_SYSTEM_TABLE *g_system_table = NULL;
static EFI_SERIAL_IO_PROTOCOL *g_serial_protocol __attribute__((unused)) = NULL;

int uefi_acquire_memory_map(EFI_SYSTEM_TABLE *st, vibeos_memory_region_t *out_regions, uint64_t max_regions, uint64_t *out_count) {
    if (!st || !out_regions || !out_count || max_regions == 0) {
        return -1;
    }
    /* Placeholder: will implement in Fase 2 */
    *out_count = 0;
    return -1;
}

int uefi_discover_firmware_tables(EFI_SYSTEM_TABLE *st, uint64_t *out_acpi_rsdp, uint64_t *out_smbios_entry) {
    if (!st || !out_acpi_rsdp || !out_smbios_entry) {
        return -1;
    }
    /* Placeholder: will implement in Fase 2 */
    *out_acpi_rsdp = 0;
    *out_smbios_entry = 0;
    return 0;
}

int uefi_get_framebuffer(EFI_SYSTEM_TABLE *st, uint64_t *out_buffer_base, uint32_t *out_width, uint32_t *out_height) {
    if (!st || !out_buffer_base || !out_width || !out_height) {
        return -1;
    }
    /* Placeholder: will implement in Fase 2 */
    *out_buffer_base = 0;
    *out_width = 0;
    *out_height = 0;
    return 0;
}

int uefi_serial_init(EFI_SYSTEM_TABLE *st) {
    if (!st) {
        return -1;
    }
    g_system_table = st;
    /* Placeholder: will implement in Fase 2 */
    return 0;
}

int uefi_serial_puts(const char *str) {
    /* Placeholder: will implement in Fase 2 */
    if (!str) {
        return -1;
    }
    return 0;
}

int uefi_serial_putc(int c) {
    /* Placeholder: will implement in Fase 2 */
    return c;
}
