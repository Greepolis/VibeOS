#ifndef VIBEOS_UEFI_FIRMWARE_H
#define VIBEOS_UEFI_FIRMWARE_H

#include "uefi_protocol.h"

/* UEFI firmware table discovery (ACPI, SMBIOS) */

int uefi_firmware_discover_tables(EFI_SYSTEM_TABLE *st, uint64_t *out_acpi_rsdp, 
                                   uint64_t *out_smbios_entry);

#endif
