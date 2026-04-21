#include "vibeos/boot.h"
#include "vibeos/bootloader.h"
#include "uefi_boot.h"
#include "uefi_protocol.h"
#include "uefi_serial.h"
#include "uefi_memory.h"
#include "uefi_firmware.h"

#define BOOT_INFO_MAX_REGIONS 32

/* Entry point for UEFI bootloader */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle __attribute__((unused)), EFI_SYSTEM_TABLE *SystemTable) {
    vibeos_memory_region_t memory_regions[BOOT_INFO_MAX_REGIONS];
    uint64_t memory_count = 0;
    uint64_t acpi_rsdp = 0;
    uint64_t smbios_entry = 0;
    uint32_t i = 0;
    
    if (!SystemTable || !SystemTable->BootServices) {
        return EFI_INVALID_PARAMETER;
    }
    
    /* Initialize serial for debugging */
    uefi_serial_init(SystemTable);
    uefi_serial_puts("\n");
    uefi_serial_puts("========================================\n");
    uefi_serial_puts("[BOOT] VibeOS UEFI Bootloader M2\n");
    uefi_serial_puts("========================================\n");
    uefi_serial_puts("[BOOT] Entry: efi_main()\n");
    uefi_serial_puts("\n");
    
    /* PHASE 2: Acquire UEFI Memory Map */
    uefi_serial_puts("[BOOT] === PHASE 2: Memory Discovery ===\n");
    if (uefi_acquire_memory_map(SystemTable, memory_regions, BOOT_INFO_MAX_REGIONS, &memory_count) != 0) {
        uefi_serial_puts("[ERROR] Failed to acquire UEFI memory map\n");
        return EFI_LOAD_ERROR;
    }
    
    uefi_serial_puts("[BOOT] Memory regions acquired: ");
    char count_str[16];
    int j = 0;
    uint64_t temp = memory_count;
    while (temp > 0) {
        count_str[j++] = '0' + (temp % 10);
        temp /= 10;
    }
    count_str[j] = '\0';
    for (int k = j - 1; k >= 0; k--) {
        uefi_serial_putc(count_str[k]);
    }
    uefi_serial_puts(" regions\n");
    
    /* Print memory map summary */
    uint64_t total_bytes = 0;
    for (i = 0; i < memory_count; i++) {
        vibeos_memory_region_t *region = &memory_regions[i];
        const char *type_str = "UNKNOWN";
        switch (region->type) {
            case VIBEOS_MEMORY_REGION_USABLE: type_str = "USABLE"; break;
            case VIBEOS_MEMORY_REGION_RESERVED: type_str = "RESERVED"; break;
            case VIBEOS_MEMORY_REGION_ACPI_RECLAIMABLE: type_str = "ACPI_RECLM"; break;
            case VIBEOS_MEMORY_REGION_ACPI_NVS: type_str = "ACPI_NVS"; break;
            case VIBEOS_MEMORY_REGION_MMIO: type_str = "MMIO"; break;
        }
        
        if (region->type == VIBEOS_MEMORY_REGION_USABLE) {
            total_bytes += region->length;
        }
    }
    
    uefi_serial_puts("[BOOT] Total usable memory: ");
    uefi_serial_puts("(calculation in next phase)\n");
    
    /* PHASE 2: Discover Firmware Tables */
    uefi_serial_puts("\n[BOOT] === PHASE 2: Firmware Tables ===\n");
    if (uefi_discover_firmware_tables(SystemTable, &acpi_rsdp, &smbios_entry) != 0) {
        uefi_serial_puts("[WARN] Firmware table discovery had issues\n");
    }
    
    uefi_serial_puts("[BOOT] ACPI RSDP: ");
    if (acpi_rsdp != 0) {
        uefi_serial_puts("found\n");
    } else {
        uefi_serial_puts("not found\n");
    }
    
    uefi_serial_puts("[BOOT] SMBIOS: ");
    if (smbios_entry != 0) {
        uefi_serial_puts("found\n");
    } else {
        uefi_serial_puts("not found\n");
    }
    
    uefi_serial_puts("\n");
    uefi_serial_puts("[BOOT] === PHASE 2 COMPLETE ===\n");
    uefi_serial_puts("[BOOT] Memory and firmware discovery successful\n");
    uefi_serial_puts("[BOOT] TODO: Phases 3-5 (kernel load, boot_info, handoff)\n");
    uefi_serial_puts("\n");
    
    return EFI_SUCCESS;
}
