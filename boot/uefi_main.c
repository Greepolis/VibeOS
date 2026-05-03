#include "vibeos/boot.h"
#include "vibeos/bootloader.h"
#include "uefi_boot.h"
#include "uefi_protocol.h"
#include "uefi_serial.h"
#include "uefi_memory.h"
#include "uefi_firmware.h"
#include "uefi_pe_loader.h"
#include "uefi_file_io.h"
#include "uefi_boot_info.h"
#include "uefi_boot_handoff.h"

#define BOOT_INFO_MAX_REGIONS 32
#define MEMORY_MAP_MAX_ATTEMPTS 3u

static const uint16_t VIBEOS_KERNEL_PATH_PRIMARY[] = {
    '\\', 'E', 'F', 'I', '\\', 'B', 'O', 'O', 'T', '\\', 'V', 'I', 'B', 'E', 'O', 'S', 'K', 'R', '.', 'E', 'L', 'F', 0
};
static const uint16_t VIBEOS_KERNEL_PATH_FALLBACK[] = {
    '\\', 'V', 'I', 'B', 'E', 'O', 'S', 'K', 'R', '.', 'E', 'L', 'F', 0
};

static void uefi_log_u64(uint64_t value) {
    int shift;
    for (shift = 60; shift >= 0; shift -= 4) {
        uint64_t nibble = (value >> (uint32_t)shift) & 0xf;
        uefi_serial_putc((int)(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10)));
    }
}

static int uefi_load_kernel_image(EFI_HANDLE image_handle, EFI_SYSTEM_TABLE *st, void **out_image, uint64_t *out_size) {
    const uint16_t *paths[2] = {
        VIBEOS_KERNEL_PATH_PRIMARY,
        VIBEOS_KERNEL_PATH_FALLBACK
    };
    uint32_t i;
    if (!out_image || !out_size) {
        return -1;
    }
    *out_image = 0;
    *out_size = 0;
    for (i = 0; i < 2u; i++) {
        if (uefi_file_read_all(image_handle, st, paths[i], out_image, out_size) == 0) {
            uefi_serial_puts("[BOOT] Kernel image loaded from EFI filesystem\n");
            return 0;
        }
        if (i == 0) {
            uefi_serial_puts("[WARN] Primary kernel path not available, trying fallback path\n");
        }
    }
    return -1;
}

/* Entry point for UEFI bootloader */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    vibeos_memory_region_t memory_regions[BOOT_INFO_MAX_REGIONS];
    uint64_t memory_count = 0;
    uint64_t acpi_rsdp = 0;
    uint64_t smbios_entry = 0;
    uefi_kernel_load_plan_t kernel_plan;
    void *kernel_image_buffer = 0;
    const uint8_t *kernel_image = 0;
    uint64_t kernel_image_size = 0;
    vibeos_kernel_t *kernel_struct = NULL;
    vibeos_boot_info_t *boot_info = NULL;
    
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
    {
        uint32_t attempt;
        int memory_map_ok = -1;
        for (attempt = 0; attempt < MEMORY_MAP_MAX_ATTEMPTS; attempt++) {
            memory_map_ok = uefi_acquire_memory_map(SystemTable, memory_regions, BOOT_INFO_MAX_REGIONS, &memory_count);
            if (memory_map_ok == 0) {
                break;
            }
            uefi_serial_puts("[WARN] Memory map acquisition failed, retrying\n");
        }
        if (memory_map_ok != 0) {
            uefi_serial_puts("[ERROR] Failed to acquire UEFI memory map\n");
            return EFI_LOAD_ERROR;
        }
    }
    
    uefi_serial_puts("[BOOT] Memory regions acquired: ");
    {
        char count_str[24];
        int j = 0;
        uint64_t temp = memory_count;
        if (temp == 0) {
            count_str[j++] = '0';
        }
        while (temp > 0 && j < (int)(sizeof(count_str) - 1u)) {
            count_str[j++] = (char)('0' + (temp % 10u));
            temp /= 10u;
        }
        while (j > 0) {
            j--;
            uefi_serial_putc(count_str[j]);
        }
    }
    uefi_serial_puts(" regions\n");
    
    /* Discover Firmware Tables */
    if (uefi_discover_firmware_tables(SystemTable, &acpi_rsdp, &smbios_entry) != 0) {
        uefi_serial_puts("[WARN] Firmware table discovery had issues\n");
    }
    
    /* Discover Security Settings (Secure Boot, Measured Boot) */
    vibeos_firmware_tag_t security_tags[4];
    uint32_t security_tag_count = 0;
    if (uefi_firmware_discover_security_settings(SystemTable, security_tags, 4u, &security_tag_count) != 0) {
        uefi_serial_puts("[WARN] Security settings discovery had issues\n");
    }
    
    uefi_serial_puts("[BOOT] ACPI/SMBIOS: ");
    if (acpi_rsdp != 0 && smbios_entry != 0) {
        uefi_serial_puts("found\n");
    } else {
        uefi_serial_puts("partial/not found\n");
    }
    
    /* PHASE 3: Load and Parse Kernel Image */
    uefi_serial_puts("\n[BOOT] === PHASE 3: Kernel Loading ===\n");

    if (uefi_load_kernel_image(ImageHandle, SystemTable, &kernel_image_buffer, &kernel_image_size) != 0) {
        uefi_serial_puts("[ERROR] Failed to load kernel image from EFI filesystem\n");
        return EFI_LOAD_ERROR;
    }
    kernel_image = (const uint8_t *)kernel_image_buffer;
    uefi_serial_puts("[BOOT] Kernel image size (bytes): 0x");
    uefi_log_u64(kernel_image_size);
    uefi_serial_puts("\n");

    if (uefi_kernel_plan_load(kernel_image, kernel_image_size, &kernel_plan) != 0) {
        uefi_serial_puts("[ERROR] Kernel image parsing failed\n");
        uefi_file_free_buffer(SystemTable, kernel_image_buffer, kernel_image_size);
        return EFI_LOAD_ERROR;
    }

    if (uefi_kernel_load_segments(SystemTable, kernel_image, kernel_image_size, &kernel_plan) != 0) {
        uefi_serial_puts("[ERROR] Kernel segment loading failed\n");
        uefi_file_free_buffer(SystemTable, kernel_image_buffer, kernel_image_size);
        return EFI_LOAD_ERROR;
    }
    uefi_serial_puts("[BOOT] Kernel segments loaded to memory\n");
    uefi_file_free_buffer(SystemTable, kernel_image_buffer, kernel_image_size);
    kernel_image_buffer = 0;
    
    uefi_serial_puts("\n");
    uefi_serial_puts("[BOOT] === PHASE 3 COMPLETE ===\n");
    uefi_serial_puts("[BOOT] Kernel loading attempted\n");
    uefi_serial_puts("\n");
    
    /* PHASE 4: Allocate and Build boot_info */
    uefi_serial_puts("[BOOT] === PHASE 4: Boot Info Building ===\n");
    
    if (uefi_boot_info_allocate(SystemTable, memory_regions, memory_count, 
                                 acpi_rsdp, smbios_entry, &kernel_plan,
                                 &kernel_struct, &boot_info) != 0) {
        uefi_serial_puts("[ERROR] Boot info allocation failed\n");
        return EFI_LOAD_ERROR;
    }
    
    /* Finalize and validate boot_info */
    if (uefi_boot_info_finalize(kernel_struct, boot_info) != 0) {
        uefi_serial_puts("[WARN] Boot info finalization had issues\n");
    }
    
    /* Apply security settings to boot_info */
    if (vibeos_bootloader_apply_firmware_tags(boot_info, security_tags, security_tag_count) != 0) {
        uefi_serial_puts("[WARN] Failed to apply security tags to boot_info\n");
    }
    
    /* Phase 4 telemetry: Log security settings summary */
    {
        uint32_t secure_boot_enabled = (boot_info->flags & VIBEOS_BOOT_FLAG_SECURE_BOOT) ? 1u : 0u;
        uint32_t measured_boot_enabled = (boot_info->flags & VIBEOS_BOOT_FLAG_MEASURED_BOOT) ? 1u : 0u;
        uefi_serial_puts("[BOOT] Security telemetry: SecureBoot=");
        uefi_serial_puts(secure_boot_enabled ? "enabled" : "disabled");
        uefi_serial_puts(", MeasuredBoot=");
        uefi_serial_puts(measured_boot_enabled ? "enabled" : "disabled");
        uefi_serial_puts("\n");
    }
    
    uefi_serial_puts("\n");
    uefi_serial_puts("[BOOT] === PHASE 4 COMPLETE ===\n");
    uefi_serial_puts("[BOOT] Boot structures ready for kernel handoff\n");
    uefi_serial_puts("\n");
    
    /* PHASE 5: Boot Handoff to Kernel */
    if (kernel_plan.kernel_entry_point == 0) {
        uefi_serial_puts("[ERROR] Kernel entry point not available\n");
        return EFI_LOAD_ERROR;
    }
    
    kernel_entry_fn entry = (kernel_entry_fn)kernel_plan.kernel_entry_point;
    
    if (uefi_boot_handoff(SystemTable, ImageHandle, kernel_struct, boot_info, entry) != 0) {
        uefi_serial_puts("[ERROR] Boot handoff failed\n");
        return EFI_LOAD_ERROR;
    }
    
    /* Should not reach here - kernel should take over */
    uefi_serial_puts("[ERROR] Returned from kernel (should not happen)\n");
    return EFI_SUCCESS;
}
