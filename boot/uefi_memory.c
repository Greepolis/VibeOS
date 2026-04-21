#include "uefi_memory.h"
#include "uefi_serial.h"
#include "uefi_boot.h"

/* Convert UEFI memory type to vibeos memory region type */
static uint32_t uefi_type_to_vibeos(uint32_t uefi_type) {
    switch (uefi_type) {
        case EfiConventionalMemory:
            return VIBEOS_MEMORY_REGION_USABLE;
        case EfiACPIReclaimMemory:
            return VIBEOS_MEMORY_REGION_ACPI_RECLAIMABLE;
        case EfiACPIMemoryNVS:
            return VIBEOS_MEMORY_REGION_ACPI_NVS;
        case EfiMemoryMappedIO:
            return VIBEOS_MEMORY_REGION_MMIO;
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiRuntimeServicesCode:
        case EfiRuntimeServicesData:
        case EfiReservedMemoryType:
        case EfiUnusableMemory:
        case EfiPalCode:
        default:
            return VIBEOS_MEMORY_REGION_RESERVED;
    }
}

int uefi_memory_map_acquire(EFI_SYSTEM_TABLE *st, vibeos_memory_region_t *out_regions, 
                             uint64_t max_regions, uint64_t *out_count) {
    EFI_MEMORY_DESCRIPTOR *efi_map = NULL;
    static EFI_MEMORY_DESCRIPTOR efi_map_buffer[256]; /* Stack-allocated buffer for ~256 descriptors max */
    size_t efi_map_size = sizeof(efi_map_buffer);
    size_t descriptor_size = 0;
    uint32_t descriptor_version = 0;
    size_t map_key = 0;
    uint64_t vibeos_count = 0;
    uint64_t i = 0;
    EFI_STATUS status;
    
    if (!st || !st->BootServices || !out_regions || !out_count || max_regions == 0) {
        uefi_serial_puts("[ERROR] uefi_memory_map_acquire: invalid parameters\n");
        return -1;
    }
    
    if (!st->BootServices->GetMemoryMap) {
        uefi_serial_puts("[ERROR] uefi_memory_map_acquire: GetMemoryMap not available\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] Acquiring UEFI memory map...\n");
    
    efi_map = efi_map_buffer;
    
    /* Get actual memory map */
    size_t actual_size = efi_map_size;
    status = st->BootServices->GetMemoryMap(&actual_size, efi_map, &map_key, &descriptor_size, &descriptor_version);
    if (status != EFI_SUCCESS && status != EFI_BUFFER_TOO_SMALL) {
        uefi_serial_puts("[ERROR] GetMemoryMap failed\n");
        return -1;
    }
    
    if (descriptor_size == 0) {
        uefi_serial_puts("[ERROR] Invalid descriptor size\n");
        return -1;
    }
    
    /* Convert EFI descriptors to vibeos regions */
    uint64_t efi_entry_count = actual_size / descriptor_size;
    if (efi_entry_count > 256) {
        efi_entry_count = 256; /* Cap to our buffer size */
    }
    
    for (i = 0; i < efi_entry_count && vibeos_count < max_regions; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR *)((uint8_t *)efi_map + (i * descriptor_size));
        
        if (desc->NumberOfPages == 0) {
            continue; /* Skip zero-length regions */
        }
        
        vibeos_memory_region_t *region = &out_regions[vibeos_count];
        region->base = desc->PhysicalStart;
        region->length = desc->NumberOfPages * 4096; /* 4KB per page */
        region->type = uefi_type_to_vibeos(desc->Type);
        region->reserved = 0;
        
        vibeos_count++;
    }
    
    if (vibeos_count == 0) {
        uefi_serial_puts("[ERROR] No memory regions found\n");
        return -1;
    }
    
    *out_count = vibeos_count;
    return 0;
}
