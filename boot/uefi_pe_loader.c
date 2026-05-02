#include "uefi_pe_loader.h"
#include "uefi_serial.h"

int uefi_kernel_plan_load(const uint8_t *kernel_image, uint64_t image_size, 
                           uefi_kernel_load_plan_t *out_plan) {
    vibeos_boot_image_plan_t image_plan;
    uint32_t i;
    
    if (!kernel_image || !out_plan || image_size == 0) {
        uefi_serial_puts("[ERROR] uefi_kernel_plan_load: invalid parameters\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] Parsing kernel image (ELF64 primary, PE32+ fallback)...\n");

    if (vibeos_bootloader_plan_elf_image(kernel_image, image_size, &image_plan) == 0) {
        uefi_serial_puts("[BOOT] Kernel ELF64 image parsed successfully\n");
    } else {
        uefi_serial_puts("[WARN] ELF64 parse failed, trying PE32+ fallback\n");
        if (vibeos_bootloader_plan_pe_image(kernel_image, image_size, &image_plan) != 0) {
            uefi_serial_puts("[ERROR] Kernel image parsing failed for both ELF64 and PE32+\n");
            return -1;
        }
        uefi_serial_puts("[BOOT] Kernel PE32+ fallback parsed successfully\n");
    }

    uefi_serial_puts("[BOOT] Kernel entry point: parsed\n");
    uefi_serial_puts("[BOOT] Kernel image base: computed\n");
    uefi_serial_puts("[BOOT] Segments: parsed\n");
    
    out_plan->kernel_entry_point = image_plan.entry_point;
    out_plan->kernel_image_base = image_plan.image_base;
    out_plan->segment_count = image_plan.segment_count;
    
    /* Copy segments */
    for (i = 0; i < image_plan.segment_count && i < VIBEOS_BOOT_IMAGE_MAX_SEGMENTS; i++) {
        out_plan->segments[i] = image_plan.segments[i];
    }
    
    return 0;
}

int uefi_kernel_load_segments(EFI_SYSTEM_TABLE *st, const uint8_t *kernel_image,
                               const uefi_kernel_load_plan_t *plan) {
    uint32_t i;
    uint32_t alloc_type = 2; /* AllocateAddress */
    
    if (!st || !st->BootServices || !kernel_image || !plan) {
        uefi_serial_puts("[ERROR] uefi_kernel_load_segments: invalid parameters\n");
        return -1;
    }
    
    if (!st->BootServices->AllocatePages) {
        uefi_serial_puts("[ERROR] AllocatePages not available\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] Loading kernel segments to memory...\n");
    
    /* Load each segment */
    for (i = 0; i < plan->segment_count; i++) {
        const vibeos_boot_image_segment_t *seg = &plan->segments[i];
        uint64_t alloc_addr = seg->image_address;
        uint64_t num_pages = (seg->mem_size + 4095) / 4096;
        EFI_STATUS status;
        
        if (seg->file_size == 0 && seg->mem_size == 0) {
            continue;
        }
        
        /* Allocate pages at specified address */
        status = st->BootServices->AllocatePages(alloc_type, 7, num_pages, &alloc_addr);
        if (status != 0) {
            uefi_serial_puts("[ERROR] Failed to allocate segment ");
            uefi_serial_putc('0' + i);
            uefi_serial_puts("\n");
            return -1;
        }
        
        /* Copy segment data */
        if (seg->file_size > 0) {
            uint64_t j = 0;
            uint8_t *dst = (uint8_t *)seg->image_address;
            const uint8_t *src = kernel_image + seg->file_offset;
            
            for (j = 0; j < seg->file_size; j++) {
                dst[j] = src[j];
            }
        }
        
        /* Zero-fill remaining memory (BSS) */
        if (seg->mem_size > seg->file_size) {
            uint64_t j = seg->file_size;
            uint8_t *dst = (uint8_t *)seg->image_address;
            
            while (j < seg->mem_size) {
                dst[j] = 0;
                j++;
            }
        }
    }
    
    uefi_serial_puts("[BOOT] Kernel segments loaded successfully\n");
    return 0;
}
