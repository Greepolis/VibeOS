#include "vibeos/boot.h"
#include "vibeos/bootloader.h"
#include "uefi_boot.h"
#include "uefi_protocol.h"

/* Entry point for UEFI bootloader */
EFI_STATUS efi_main(EFI_HANDLE ImageHandle __attribute__((unused)), EFI_SYSTEM_TABLE *SystemTable) {
    if (!SystemTable || !SystemTable->BootServices) {
        return EFI_INVALID_PARAMETER;
    }
    
    /* Initialize serial for debugging (Fase 2+: implement properly) */
    uefi_serial_init(SystemTable);
    uefi_serial_puts("[BOOT] VibeOS UEFI Bootloader\n");
    uefi_serial_puts("[BOOT] Entry: efi_main()\n");
    
    /* TODO: Implement boot phases
     * 
     * Fase 1 (done): Infrastructure only, print banner
     * Fase 2: UEFI memory discovery
     *   - GetMemoryMap()
     *   - Discover ACPI/SMBIOS
     *   - Get framebuffer info
     * 
     * Fase 3: Load and parse kernel PE32+ image
     *   - Read kernel from disk
     *   - Parse PE header with vibeos_bootloader_plan_pe_image()
     *   - Allocate pages for kernel segments
     * 
     * Fase 4: Build boot_info structure
     *   - Allocate kernel_t struct
     *   - Allocate boot_info with memory_map
     *   - Populate and validate with vibeos_bootloader_validate_boot_info()
     * 
     * Fase 5: Handoff to kernel
     *   - ExitBootServices()
     *   - Setup CPU state
     *   - Jump to kernel entry point with parameters in RDI/RSI
     */
    
    uefi_serial_puts("[BOOT] Bootloader stub loaded (Fase 1)\n");
    uefi_serial_puts("[BOOT] TODO: implement memory discovery and kernel loading\n");
    
    /* Placeholder: return success for now */
    return EFI_SUCCESS;
}
