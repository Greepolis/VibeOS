#include "uefi_boot_info.h"
#include "uefi_serial.h"

#define UEFI_PAGE_SIZE 4096ull
#define UEFI_KERNEL_STRUCT_PREFERRED 0x200000ull
#define UEFI_BOOT_INFO_PREFERRED 0x201000ull
#define UEFI_BOOT_MAP_PREFERRED 0x202000ull

static void uefi_zero_pages(void *base, uint64_t pages) {
    uint8_t *ptr = (uint8_t *)base;
    uint64_t i;
    if (!base || pages == 0) {
        return;
    }
    for (i = 0; i < pages * UEFI_PAGE_SIZE; i++) {
        ptr[i] = 0;
    }
}

static EFI_STATUS uefi_allocate_pages_with_fallback(EFI_SYSTEM_TABLE *st,
                                                    uint64_t preferred_address,
                                                    uint64_t pages,
                                                    uint64_t *out_address,
                                                    const char *label) {
    uint64_t address;
    EFI_STATUS status;

    if (!st || !st->BootServices || !st->BootServices->AllocatePages || !out_address) {
        return EFI_INVALID_PARAMETER;
    }

    address = preferred_address;
    status = st->BootServices->AllocatePages(AllocateAddress, EfiLoaderData, (size_t)pages, &address);
    if (status == EFI_SUCCESS) {
        *out_address = address;
        return EFI_SUCCESS;
    }

    if (label) {
        uefi_serial_puts("[WARN] Preferred allocation failed for ");
        uefi_serial_puts(label);
        uefi_serial_puts(", retrying AllocateAnyPages\n");
    }

    address = 0;
    status = st->BootServices->AllocatePages(AllocateAnyPages, EfiLoaderData, (size_t)pages, &address);
    if (status == EFI_SUCCESS) {
        *out_address = address;
    }
    return status;
}

static void uefi_free_pages_if_possible(EFI_SYSTEM_TABLE *st, uint64_t address, uint64_t pages) {
    if (!st || !st->BootServices || !st->BootServices->FreePages || address == 0 || pages == 0) {
        return;
    }
    (void)st->BootServices->FreePages(address, (size_t)pages);
}

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
    uint64_t kernel_pages = 1;
    uint64_t boot_info_pages = 1;
    uint64_t memory_map_pages = 0;
    uint64_t memory_map_entries_capacity = 0;
    uint64_t entries_to_copy = 0;
    uint32_t i;
    
    if (!st || !st->BootServices || !st->BootServices->AllocatePages || !memory_regions || !out_kernel || !out_boot_info) {
        uefi_serial_puts("[ERROR] uefi_boot_info_allocate: invalid parameters\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] Allocating boot structures...\n");

    if (memory_count == 0) {
        uefi_serial_puts("[ERROR] No memory regions provided for boot_info\n");
        return -1;
    }

    memory_map_pages = (((uint64_t)memory_count * sizeof(vibeos_memory_region_t)) + (UEFI_PAGE_SIZE - 1ull)) / UEFI_PAGE_SIZE;
    if (memory_map_pages == 0) {
        memory_map_pages = 1;
    }

    status = uefi_allocate_pages_with_fallback(st, UEFI_KERNEL_STRUCT_PREFERRED, kernel_pages, &kernel_addr, "kernel_t");
    if (status != 0) {
        uefi_serial_puts("[ERROR] Failed to allocate kernel_t structure\n");
        return -1;
    }
    kernel = (vibeos_kernel_t *)kernel_addr;
    
    status = uefi_allocate_pages_with_fallback(st, UEFI_BOOT_INFO_PREFERRED, boot_info_pages, &boot_info_addr, "boot_info_t");
    if (status != 0) {
        uefi_serial_puts("[ERROR] Failed to allocate boot_info_t structure\n");
        uefi_free_pages_if_possible(st, kernel_addr, kernel_pages);
        return -1;
    }
    boot_info = (vibeos_boot_info_t *)boot_info_addr;
    
    status = uefi_allocate_pages_with_fallback(st, UEFI_BOOT_MAP_PREFERRED, memory_map_pages, &memory_map_addr, "boot memory map");
    if (status != 0) {
        uefi_serial_puts("[ERROR] Failed to allocate memory_map array\n");
        uefi_free_pages_if_possible(st, boot_info_addr, boot_info_pages);
        uefi_free_pages_if_possible(st, kernel_addr, kernel_pages);
        return -1;
    }
    boot_memory_map = (vibeos_memory_region_t *)memory_map_addr;
    
    uefi_serial_puts("[BOOT] kernel_t allocated at 0x");
    uint64_t temp = kernel_addr;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint64_t nibble = (temp >> shift) & 0xf;
        uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("[BOOT] boot_info_t allocated at 0x");
    temp = boot_info_addr;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint64_t nibble = (temp >> shift) & 0xf;
        uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("[BOOT] memory_map allocated at 0x");
    temp = memory_map_addr;
    for (int shift = 60; shift >= 0; shift -= 4) {
        uint64_t nibble = (temp >> shift) & 0xf;
        uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
    }
    uefi_serial_puts("\n");
    
    uefi_zero_pages(kernel, kernel_pages);
    uefi_zero_pages(boot_info, boot_info_pages);
    uefi_zero_pages(boot_memory_map, memory_map_pages);

    memory_map_entries_capacity = (memory_map_pages * UEFI_PAGE_SIZE) / sizeof(vibeos_memory_region_t);
    entries_to_copy = memory_count;
    if (entries_to_copy > memory_map_entries_capacity) {
        uefi_serial_puts("[WARN] Memory region count truncated\n");
        entries_to_copy = memory_map_entries_capacity;
    }
    
    for (i = 0; i < (uint32_t)entries_to_copy; i++) {
        boot_memory_map[i] = memory_regions[i];
    }

    if (vibeos_bootloader_build_boot_info(boot_info, boot_memory_map, entries_to_copy) != 0) {
        uefi_serial_puts("[ERROR] Failed to initialize boot_info contract\n");
        uefi_free_pages_if_possible(st, memory_map_addr, memory_map_pages);
        uefi_free_pages_if_possible(st, boot_info_addr, boot_info_pages);
        uefi_free_pages_if_possible(st, kernel_addr, kernel_pages);
        return -1;
    }
    
    /* Populate boot_info with firmware tables */
    boot_info->acpi_rsdp = acpi_rsdp;
    boot_info->smbios_entry = smbios_entry;
    
    uefi_serial_puts("[BOOT] boot_info populated: ");
    {
        char count_str[16];
        int j = 0;
        uint64_t temp = boot_info->memory_map_entries;
        if (temp == 0) {
            count_str[j++] = '0';
        }
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
