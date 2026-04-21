#include "uefi_firmware.h"
#include "uefi_serial.h"
#include "uefi_boot.h"

/* ACPI Table GUID: 8868e871-e4f1-11d3-bc22-0080c73c8881 */
static const EFI_GUID acpi_table_guid = {
    0x8868e871,
    0xe4f1,
    0x11d3,
    {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}
};

/* ACPI 2.0 Table GUID: 8cc346f4-6cee-4c14-9b85-cdbab8ca5c16 */
static const EFI_GUID acpi2_table_guid = {
    0x8cc346f4,
    0x6cee,
    0x4c14,
    {0x9b, 0x85, 0xcd, 0xba, 0xb8, 0xca, 0x5c, 0x16}
};

/* SMBIOS Table GUID: eb9d2d31-2d88-11d3-9a16-0090273fc14d */
static const EFI_GUID smbios_table_guid = {
    0xeb9d2d31,
    0x2d88,
    0x11d3,
    {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}
};

static int guid_equal(const EFI_GUID *a, const EFI_GUID *b) {
    if (!a || !b) {
        return 0;
    }
    return a->Data1 == b->Data1 &&
           a->Data2 == b->Data2 &&
           a->Data3 == b->Data3 &&
           a->Data4[0] == b->Data4[0] &&
           a->Data4[1] == b->Data4[1] &&
           a->Data4[2] == b->Data4[2] &&
           a->Data4[3] == b->Data4[3] &&
           a->Data4[4] == b->Data4[4] &&
           a->Data4[5] == b->Data4[5] &&
           a->Data4[6] == b->Data4[6] &&
           a->Data4[7] == b->Data4[7];
}

int uefi_firmware_discover_tables(EFI_SYSTEM_TABLE *st, uint64_t *out_acpi_rsdp, 
                                   uint64_t *out_smbios_entry) {
    size_t i = 0;
    
    if (!st || !out_acpi_rsdp || !out_smbios_entry) {
        return -1;
    }
    
    *out_acpi_rsdp = 0;
    *out_smbios_entry = 0;
    
    if (!st->ConfigurationTable || st->NumberOfTableEntries == 0) {
        uefi_serial_puts("[BOOT] No configuration tables present\n");
        return 0;
    }
    
    uefi_serial_puts("[BOOT] Scanning configuration tables...\n");
    
    for (i = 0; i < st->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE_ENTRY *entry = &st->ConfigurationTable[i];
        
        if (!entry) {
            continue;
        }
        
        if (guid_equal(&entry->VendorGuid, &acpi2_table_guid)) {
            *out_acpi_rsdp = (uint64_t)(uintptr_t)entry->VendorTable;
            uefi_serial_puts("[BOOT] Found ACPI 2.0 RSDP\n");
        } else if (guid_equal(&entry->VendorGuid, &acpi_table_guid)) {
            if (*out_acpi_rsdp == 0) { /* Prefer ACPI 2.0 */
                *out_acpi_rsdp = (uint64_t)(uintptr_t)entry->VendorTable;
            }
            uefi_serial_puts("[BOOT] Found ACPI 1.0 RSDP\n");
        } else if (guid_equal(&entry->VendorGuid, &smbios_table_guid)) {
            *out_smbios_entry = (uint64_t)(uintptr_t)entry->VendorTable;
            uefi_serial_puts("[BOOT] Found SMBIOS entry\n");
        }
    }
    
    if (*out_acpi_rsdp == 0) {
        uefi_serial_puts("[WARN] ACPI RSDP not found\n");
    }
    
    if (*out_smbios_entry == 0) {
        uefi_serial_puts("[WARN] SMBIOS entry not found\n");
    }
    
    return 0;
}
