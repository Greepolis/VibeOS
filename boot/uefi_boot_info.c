#include "uefi_boot_info.h"
#include "uefi_serial.h"

int uefi_boot_info_allocate(EFI_SYSTEM_TABLE *st, 
                             const vibeos_memory_region_t *memory_regions,
                             uint32_t memory_count,
                             uint64_t acpi_rsdp,
                             uint64_t smbios_entry,
                             const uefi_kernel_load_plan_t *kernel_plan __attribute__((unused)),
                             vibeos_kernel_t **out_kernel,
                             vibeos_boot_info_t **out_boot_info) {
    EFI_STATUS status;
    vibeos_kernel_t *kernel = NULL;
    vibeos_boot_info_t *boot_info = NULL;
    vibeos_memory_region_t *boot_memory_map = NULL;
    uint64_t kernel_addr = 0;
    uint64_t boot_info_addr = 0;
    uint64_t memory_map_addr = 0;
    uint32_t i;
    
    if (!st || !st->BootServices || !memory_regions || !out_kernel || !out_boot_info) {
        uefi_serial_puts("[ERROR] uefi_boot_info_allocate: invalid parameters\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] Allocating boot structures...\n");
    
    /* Allocate kernel_t structure (1 page) */
    kernel_addr = 0x200000;  /* Fixed address for M2 */
    uint64_t kernel_pages = 1;
    status = st->BootServices->AllocatePages(2, 7, kernel_pages, &kernel_addr);
    if (status != 0) {
        uefi_serial_puts("[ERROR] Failed to allocate kernel_t structure\n");
        return -1;
    }
    kernel = (vibeos_kernel_t *)kernel_addr;
    
    /* Allocate boot_info_t structure (1 page) */
    boot_info_addr = 0x201000;  /* Fixed address for M2 */
    uint64_t boot_info_pages = 1;
    status = st->BootServices->AllocatePages(2, 7, boot_info_pages, &boot_info_addr);
    if (status != 0) {
        uefi_serial_puts("[ERROR] Failed to allocate boot_info_t structure\n");
        return -1;
    }
    boot_info = (vibeos_boot_info_t *)boot_info_addr;
    
    /* Allocate memory_map array (after boot_info) */
    memory_map_addr = 0x202000;  /* Fixed address for M2 */
    uint64_t memory_map_pages = 1;
    status = st->BootServices->AllocatePages(2, 7, memory_map_pages, &memory_map_addr);
    if (status != 0) {
        uefi_serial_puts("[ERROR] Failed to allocate memory_map array\n");
        return -1;
    }
    boot_memory_map = (vibeos_memory_region_t *)memory_map_addr;
    
    uefi_serial_puts("[BOOT] kernel_t allocated at 0x");
    uint64_t temp = kernel_addr;
    for (int shift = 32; shift >= 0; shift -= 4) {
        uint64_t nibble = (temp >> shift) & 0xf;
        uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("[BOOT] boot_info_t allocated at 0x");
    temp = boot_info_addr;
    for (int shift = 32; shift >= 0; shift -= 4) {
        uint64_t nibble = (temp >> shift) & 0xf;
        uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("[BOOT] memory_map allocated at 0x");
    temp = memory_map_addr;
    for (int shift = 32; shift >= 0; shift -= 4) {
        uint64_t nibble = (temp >> shift) & 0xf;
        uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    uefi_serial_puts("\n");
    
    /* Zero-initialize structures */
    {
        uint8_t *ptr = (uint8_t *)kernel;
        for (uint64_t j = 0; j < 4096; j++) {
            ptr[j] = 0;
        }
    }
    {
        uint8_t *ptr = (uint8_t *)boot_info;
        for (uint64_t j = 0; j < 4096; j++) {
            ptr[j] = 0;
        }
    }
    {
        uint8_t *ptr = (uint8_t *)boot_memory_map;
        for (uint64_t j = 0; j < 4096; j++) {
            ptr[j] = 0;
        }
    }
    
    /* Populate boot_info with basic fields */
    boot_info->version = VIBEOS_BOOTINFO_VERSION;
    boot_info->flags = 0;
    
    /* Copy and populate memory map */
    boot_info->memory_map_entries = memory_count;
    if (memory_count > (4096 / sizeof(vibeos_memory_region_t))) {
        uefi_serial_puts("[WARN] Memory region count truncated\n");
        boot_info->memory_map_entries = 4096 / sizeof(vibeos_memory_region_t);
    }
    
    for (i = 0; i < boot_info->memory_map_entries; i++) {
        boot_memory_map[i] = memory_regions[i];
    }
    boot_info->memory_map = boot_memory_map;
    
    /* Populate boot_info with firmware tables */
    boot_info->acpi_rsdp = acpi_rsdp;
    boot_info->smbios_entry = smbios_entry;
    
    uefi_serial_puts("[BOOT] boot_info populated: ");
    {
        char count_str[16];
        int j = 0;
        uint64_t temp = boot_info->memory_map_entries;
        while (temp > 0) {
            count_str[j++] = '0' + (temp % 10);
            temp /= 10;
        }
        count_str[j] = '\0';
        for (int k = j - 1; k >= 0; k--) {
            uefi_serial_putc(count_str[k]);
        }
    }
    uefi_serial_puts(" memory regions, ACPI=");
    if (acpi_rsdp != 0) {
        uefi_serial_puts("yes");
    } else {
        uefi_serial_puts("no");
    }
    uefi_serial_puts(", SMBIOS=");
    if (smbios_entry != 0) {
        uefi_serial_puts("yes");
    } else {
        uefi_serial_puts("no");
    }
    uefi_serial_puts("\n");
    
    *out_kernel = kernel;
    *out_boot_info = boot_info;
    return 0;
}

int uefi_boot_info_finalize(vibeos_kernel_t *kernel, vibeos_boot_info_t *boot_info) {
    if (!kernel || !boot_info) {
        uefi_serial_puts("[ERROR] uefi_boot_info_finalize: invalid parameters\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] Validating boot_info...\n");
    
    /* Validate boot_info structure */
    if (vibeos_bootloader_validate_boot_info(boot_info) != 0) {
        uefi_serial_puts("[ERROR] boot_info validation failed\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] boot_info validation passed\n");
    
    uefi_serial_puts("[BOOT] Boot structures finalized and ready for kernel handoff\n");
    return 0;
}
