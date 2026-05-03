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

int uefi_firmware_discover_security_settings(EFI_SYSTEM_TABLE *st __attribute__((unused)), 
                                              vibeos_firmware_tag_t *out_tags,
                                              uint32_t max_tags,
                                              uint32_t *out_tag_count) {
    uint32_t tag_index = 0;
    
    if (!out_tags || !out_tag_count || max_tags == 0) {
        return -1;
    }
    
    *out_tag_count = 0;
    
    uefi_serial_puts("[BOOT] === PHASE 4: Security Settings Discovery ===\n");
    
    /* Create SECURE_BOOT tag */
    if (tag_index < max_tags) {
        out_tags[tag_index].type = VIBEOS_FIRMWARE_TAG_SECURE_BOOT;
        out_tags[tag_index].reserved = 0;
        /* Phase 4: Default value (0 = not enabled) */
        /* TODO: Future integration with GetVariable("SecureBoot") */
        out_tags[tag_index].value = 0;
        uefi_serial_puts("[BOOT] SECURE_BOOT tag: value=");
        uefi_serial_putc('0' + (out_tags[tag_index].value ? 1 : 0));
        uefi_serial_puts(" (phase 4: default, GetVariable pending)\n");
        tag_index++;
    } else {
        uefi_serial_puts("[WARN] Out of tag space for SECURE_BOOT\n");
        return -1;
    }
    
    /* Create MEASURED_BOOT tag */
    if (tag_index < max_tags) {
        out_tags[tag_index].type = VIBEOS_FIRMWARE_TAG_MEASURED_BOOT;
        out_tags[tag_index].reserved = 0;
        /* Phase 4: Default value (0 = not enabled) */
        /* TODO: Future integration with TCG GetVariable("MeasuredBoot") */
        out_tags[tag_index].value = 0;
        uefi_serial_puts("[BOOT] MEASURED_BOOT tag: value=");
        uefi_serial_putc('0' + (out_tags[tag_index].value ? 1 : 0));
        uefi_serial_puts(" (phase 4: default, GetVariable pending)\n");
        tag_index++;
    } else {
        uefi_serial_puts("[WARN] Out of tag space for MEASURED_BOOT\n");
        return -1;
    }
    
    *out_tag_count = tag_index;
    uefi_serial_puts("[BOOT] Security settings discovered: ");
    {
        char count_str[8];
        int j = 0;
        uint32_t temp = *out_tag_count;
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
    uefi_serial_puts(" tags\n");
    
    return 0;
}
