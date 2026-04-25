#include "uefi_memory.h"
#include "uefi_serial.h"
#include "uefi_boot.h"

#define UEFI_MEMORY_MAP_SCRATCH_SIZE (32u * 1024u)

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

static EFI_STATUS uefi_memory_map_read(EFI_SYSTEM_TABLE *st,
                                       EFI_MEMORY_DESCRIPTOR *buffer,
                                       size_t buffer_size,
                                       size_t *out_map_size,
                                       size_t *out_map_key,
                                       size_t *out_descriptor_size,
                                       uint32_t *out_descriptor_version) {
    size_t map_size = buffer_size;
    size_t map_key = 0;
    size_t descriptor_size = 0;
    uint32_t descriptor_version = 0;
    EFI_STATUS status;

    if (!st || !st->BootServices || !st->BootServices->GetMemoryMap ||
        !buffer || !out_map_size || !out_map_key || !out_descriptor_size || !out_descriptor_version) {
        return EFI_INVALID_PARAMETER;
    }

    status = st->BootServices->GetMemoryMap(&map_size, buffer, &map_key, &descriptor_size, &descriptor_version);
    if (status == EFI_BUFFER_TOO_SMALL) {
        if (map_size > buffer_size) {
            return EFI_BUFFER_TOO_SMALL;
        }
        status = st->BootServices->GetMemoryMap(&map_size, buffer, &map_key, &descriptor_size, &descriptor_version);
    }

    if (status == EFI_SUCCESS) {
        *out_map_size = map_size;
        *out_map_key = map_key;
        *out_descriptor_size = descriptor_size;
        *out_descriptor_version = descriptor_version;
    }

    return status;
}

int uefi_memory_map_acquire(EFI_SYSTEM_TABLE *st, vibeos_memory_region_t *out_regions, 
                             uint64_t max_regions, uint64_t *out_count) {
    uint8_t efi_map_scratch[UEFI_MEMORY_MAP_SCRATCH_SIZE];
    EFI_MEMORY_DESCRIPTOR *efi_map = (EFI_MEMORY_DESCRIPTOR *)efi_map_scratch;
    size_t efi_map_size = 0;
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
    
    status = uefi_memory_map_read(st,
                                  efi_map,
                                  sizeof(efi_map_scratch),
                                  &efi_map_size,
                                  &map_key,
                                  &descriptor_size,
                                  &descriptor_version);
    if (status == EFI_BUFFER_TOO_SMALL) {
        uefi_serial_puts("[ERROR] GetMemoryMap requires more scratch buffer\n");
        return -1;
    }
    if (status != EFI_SUCCESS) {
        uefi_serial_puts("[ERROR] GetMemoryMap failed\n");
        return -1;
    }
    
    if (descriptor_size == 0) {
        uefi_serial_puts("[ERROR] Invalid descriptor size\n");
        return -1;
    }
    
    /* Convert EFI descriptors to vibeos regions */
    uint64_t efi_entry_count = efi_map_size / descriptor_size;
    
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
    
    (void)map_key;
    (void)descriptor_version;
    *out_count = vibeos_count;
    return 0;
}

int uefi_memory_map_fetch_key(EFI_SYSTEM_TABLE *st, size_t *out_map_key) {
    uint8_t efi_map_scratch[UEFI_MEMORY_MAP_SCRATCH_SIZE];
    size_t efi_map_size = 0;
    size_t descriptor_size = 0;
    uint32_t descriptor_version = 0;
    size_t map_key = 0;
    EFI_STATUS status;

    if (!st || !out_map_key) {
        return -1;
    }

    status = uefi_memory_map_read(st,
                                  (EFI_MEMORY_DESCRIPTOR *)efi_map_scratch,
                                  sizeof(efi_map_scratch),
                                  &efi_map_size,
                                  &map_key,
                                  &descriptor_size,
                                  &descriptor_version);
    if (status != EFI_SUCCESS) {
        return -1;
    }

    (void)efi_map_size;
    (void)descriptor_size;
    (void)descriptor_version;
    *out_map_key = map_key;
    return 0;
}
