#include "vibeos/boot.h"
#include "vibeos/bootloader.h"
#include "uefi_boot.h"
#include "uefi_protocol.h"
#include "uefi_serial.h"
#include "uefi_memory.h"
#include "uefi_firmware.h"
#include "uefi_pe_loader.h"

#define BOOT_INFO_MAX_REGIONS 32
#define KERNEL_LOAD_ADDR 0x400000

/* Entry point for UEFI bootloader */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle __attribute__((unused)), EFI_SYSTEM_TABLE *SystemTable) {
    vibeos_memory_region_t memory_regions[BOOT_INFO_MAX_REGIONS];
    uint64_t memory_count = 0;
    uint64_t acpi_rsdp = 0;
    uint64_t smbios_entry = 0;
    uefi_kernel_load_plan_t kernel_plan;
    const uint8_t *kernel_image = (const uint8_t *)KERNEL_LOAD_ADDR;
    
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
    uefi_serial_puts("[BOOT] === PHASE 2: Memory & Firmware ===\n");
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
    
    /* Discover Firmware Tables */
    if (uefi_discover_firmware_tables(SystemTable, &acpi_rsdp, &smbios_entry) != 0) {
        uefi_serial_puts("[WARN] Firmware table discovery had issues\n");
    }
    
    uefi_serial_puts("[BOOT] ACPI/SMBIOS: ");
    if (acpi_rsdp != 0 && smbios_entry != 0) {
        uefi_serial_puts("found\n");
    } else {
        uefi_serial_puts("partial/not found\n");
    }
    
    /* PHASE 3: Load and Parse Kernel PE32+ */
    uefi_serial_puts("\n[BOOT] === PHASE 3: Kernel Loading ===\n");
    
    /* M2 stub: kernel image expected at hardcoded address after UEFI firmware setup */
    uefi_serial_puts("[BOOT] Kernel image location: 0x");
    uint64_t addr = KERNEL_LOAD_ADDR;
    for (int shift = 32; shift >= 0; shift -= 4) {
        uint64_t nibble = (addr >> shift) & 0xf;
        uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    uefi_serial_puts("\n");
    
    /* Attempt to parse kernel PE image */
    if (uefi_kernel_plan_load(kernel_image, 16 * 1024 * 1024, &kernel_plan) == 0) {
        uefi_serial_puts("[BOOT] Kernel PE image parsed\n");
        uefi_serial_puts("[BOOT] Kernel entry point: computed\n");
        
        /* Load kernel segments to physical memory */
        if (uefi_kernel_load_segments(SystemTable, kernel_image, &kernel_plan) == 0) {
            uefi_serial_puts("[BOOT] Kernel segments loaded to memory\n");
        } else {
            uefi_serial_puts("[WARN] Kernel segment loading had issues\n");
        }
    } else {
        uefi_serial_puts("[WARN] Kernel PE parsing failed (expected in M2 stub)\n");
        uefi_serial_puts("[WARN] Continuing to Phase 4 anyway\n");
    }
    
    uefi_serial_puts("\n");
    uefi_serial_puts("[BOOT] === PHASE 3 COMPLETE ===\n");
    uefi_serial_puts("[BOOT] Kernel loading attempted\n");
    uefi_serial_puts("[BOOT] TODO: Phases 4-5 (boot_info allocation, handoff)\n");
    uefi_serial_puts("\n");
    
    return EFI_SUCCESS;
}
