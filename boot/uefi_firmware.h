#ifndef VIBEOS_UEFI_FIRMWARE_H
#define VIBEOS_UEFI_FIRMWARE_H

#include "uefi_protocol.h"
#include "vibeos/bootloader.h"

/* UEFI firmware table discovery (ACPI, SMBIOS) */
int uefi_firmware_discover_tables(EFI_SYSTEM_TABLE *st, uint64_t *out_acpi_rsdp, 
                                   uint64_t *out_smbios_entry);

/* UEFI firmware security settings discovery (Secure Boot, Measured Boot) */
int uefi_firmware_discover_security_settings(EFI_SYSTEM_TABLE *st, 
                                              vibeos_firmware_tag_t *out_tags,
                                              uint32_t max_tags,
                                              uint32_t *out_tag_count);

#endif
