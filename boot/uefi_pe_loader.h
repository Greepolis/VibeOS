#ifndef VIBEOS_UEFI_PE_LOADER_H
#define VIBEOS_UEFI_PE_LOADER_H

#include "vibeos/bootloader.h"
#include "uefi_boot.h"
#include "uefi_protocol.h"

/* PE32+ kernel image loading and parsing */

typedef struct {
    uint64_t kernel_entry_point;
    uint64_t kernel_image_base;
    uint32_t segment_count;
    vibeos_boot_image_segment_t segments[VIBEOS_BOOT_IMAGE_MAX_SEGMENTS];
} uefi_kernel_load_plan_t;

/* Load kernel image from memory buffer and parse PE format */
int uefi_kernel_plan_load(const uint8_t *kernel_image, uint64_t image_size, 
                           uefi_kernel_load_plan_t *out_plan);

/* Allocate and load kernel segments to physical memory */
int uefi_kernel_load_segments(EFI_SYSTEM_TABLE *st, const uint8_t *kernel_image,
                               const uefi_kernel_load_plan_t *plan);

#endif
