#ifndef VIBEOS_UEFI_MEMORY_H
#define VIBEOS_UEFI_MEMORY_H

#include "vibeos/boot.h"
#include "uefi_protocol.h"

/* UEFI Memory acquisition and conversion */

int uefi_memory_map_acquire(EFI_SYSTEM_TABLE *st, vibeos_memory_region_t *out_regions, 
                             uint64_t max_regions, uint64_t *out_count);
int uefi_memory_map_fetch_key(EFI_SYSTEM_TABLE *st, size_t *out_map_key);

#endif
