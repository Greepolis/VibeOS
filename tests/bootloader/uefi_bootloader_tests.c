#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "vibeos/boot.h"
#include "vibeos/kernel.h"
#include "vibeos/bootloader.h"
#include "uefi_boot.h"
#include "uefi_protocol.h"
#include "uefi_memory.h"
#include "uefi_boot_info.h"
#include "uefi_boot_handoff.h"
#include "uefi_firmware.h"

typedef struct mock_uefi_ctx {
    EFI_BOOT_SERVICES boot_services;
    EFI_SYSTEM_TABLE system_table;
    EFI_MEMORY_DESCRIPTOR descriptors[4];
    size_t descriptor_count;
    size_t descriptor_size;
    size_t required_map_size;
    size_t map_keys[8];
    uint32_t map_key_count;
    uint32_t map_key_index;
    EFI_STATUS exit_statuses[8];
    uint32_t exit_status_count;
    uint32_t exit_status_index;
    uint32_t get_memory_map_calls;
    uint32_t exit_boot_services_calls;
    uint32_t alloc_address_calls;
    uint32_t alloc_any_calls;
    uint32_t free_pages_calls;
    uint32_t force_first_buffer_too_small;
    uint32_t allocate_address_fail_budget;
    size_t alloc_pool_offset;
    uint8_t alloc_pool[64u * 4096u];
} mock_uefi_ctx_t;

static mock_uefi_ctx_t *g_mock_ctx = NULL;

/* Serial stubs for host tests. */
int uefi_serial_init(EFI_SYSTEM_TABLE *st) {
    (void)st;
    return 0;
}

int uefi_serial_puts(const char *str) {
    (void)str;
    return 0;
}

int uefi_serial_putc(int c) {
    return c;
}

int uefi_serial_printf(const char *fmt, ...) {
    (void)fmt;
    return 0;
}

static EFI_STATUS mock_get_memory_map(size_t *memory_map_size,
                                      EFI_MEMORY_DESCRIPTOR *memory_map,
                                      size_t *map_key,
                                      size_t *descriptor_size,
                                      uint32_t *descriptor_version) {
    size_t needed_size;
    if (!g_mock_ctx || !memory_map_size || !map_key || !descriptor_size || !descriptor_version) {
        return EFI_INVALID_PARAMETER;
    }

    g_mock_ctx->get_memory_map_calls++;
    needed_size = g_mock_ctx->required_map_size;

    if (g_mock_ctx->force_first_buffer_too_small && g_mock_ctx->get_memory_map_calls == 1u) {
        *memory_map_size = needed_size;
        return EFI_BUFFER_TOO_SMALL;
    }

    if (!memory_map || *memory_map_size < needed_size) {
        *memory_map_size = needed_size;
        return EFI_BUFFER_TOO_SMALL;
    }

    memcpy(memory_map, g_mock_ctx->descriptors, needed_size);
    *memory_map_size = needed_size;
    *descriptor_size = g_mock_ctx->descriptor_size;
    *descriptor_version = 1u;
    if (g_mock_ctx->map_key_index < g_mock_ctx->map_key_count) {
        *map_key = g_mock_ctx->map_keys[g_mock_ctx->map_key_index++];
    } else {
        *map_key = 0x55u;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS mock_allocate_pages(uint32_t type,
                                      EFI_MEMORY_TYPE memory_type,
                                      size_t pages,
                                      uint64_t *memory) {
    size_t bytes;
    size_t new_offset;
    uint8_t *base;
    (void)memory_type;
    if (!g_mock_ctx || !memory || pages == 0) {
        return EFI_INVALID_PARAMETER;
    }

    if (type == AllocateAddress) {
        g_mock_ctx->alloc_address_calls++;
        if (g_mock_ctx->allocate_address_fail_budget > 0u) {
            g_mock_ctx->allocate_address_fail_budget--;
            return EFI_OUT_OF_RESOURCES;
        }
        bytes = pages * 4096u;
        new_offset = g_mock_ctx->alloc_pool_offset + bytes;
        if (new_offset > sizeof(g_mock_ctx->alloc_pool)) {
            return EFI_OUT_OF_RESOURCES;
        }
        base = &g_mock_ctx->alloc_pool[g_mock_ctx->alloc_pool_offset];
        g_mock_ctx->alloc_pool_offset = new_offset;
        *memory = (uint64_t)(uintptr_t)base;
        return EFI_SUCCESS;
    }

    if (type == AllocateAnyPages) {
        g_mock_ctx->alloc_any_calls++;
        bytes = pages * 4096u;
        new_offset = g_mock_ctx->alloc_pool_offset + bytes;
        if (new_offset > sizeof(g_mock_ctx->alloc_pool)) {
            return EFI_OUT_OF_RESOURCES;
        }
        base = &g_mock_ctx->alloc_pool[g_mock_ctx->alloc_pool_offset];
        g_mock_ctx->alloc_pool_offset = new_offset;
        *memory = (uint64_t)(uintptr_t)base;
        return EFI_SUCCESS;
    }

    return EFI_UNSUPPORTED;
}

static EFI_STATUS mock_free_pages(uint64_t memory, size_t pages) {
    (void)memory;
    (void)pages;
    if (!g_mock_ctx) {
        return EFI_INVALID_PARAMETER;
    }
    g_mock_ctx->free_pages_calls++;
    return EFI_SUCCESS;
}

static EFI_STATUS mock_exit_boot_services(EFI_HANDLE image_handle, size_t map_key) {
    (void)image_handle;
    (void)map_key;
    if (!g_mock_ctx) {
        return EFI_INVALID_PARAMETER;
    }
    g_mock_ctx->exit_boot_services_calls++;
    if (g_mock_ctx->exit_status_index < g_mock_ctx->exit_status_count) {
        return g_mock_ctx->exit_statuses[g_mock_ctx->exit_status_index++];
    }
    return EFI_SUCCESS;
}

static void init_mock(mock_uefi_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->descriptor_size = sizeof(EFI_MEMORY_DESCRIPTOR);
    ctx->required_map_size = ctx->descriptor_size;

    ctx->boot_services.GetMemoryMap = mock_get_memory_map;
    ctx->boot_services.AllocatePages = mock_allocate_pages;
    ctx->boot_services.FreePages = mock_free_pages;
    ctx->boot_services.ExitBootServices = mock_exit_boot_services;

    ctx->system_table.BootServices = &ctx->boot_services;
    g_mock_ctx = ctx;
}

static int test_memory_map_retry_and_conversion(void) {
    mock_uefi_ctx_t ctx;
    vibeos_memory_region_t regions[8];
    uint64_t out_count = 0;
    int rc;

    init_mock(&ctx);
    ctx.force_first_buffer_too_small = 1u;
    ctx.descriptor_count = 2u;
    ctx.required_map_size = ctx.descriptor_count * ctx.descriptor_size;
    ctx.map_keys[0] = 101u;
    ctx.map_key_count = 1u;

    ctx.descriptors[0].Type = EfiConventionalMemory;
    ctx.descriptors[0].PhysicalStart = 0x100000ull;
    ctx.descriptors[0].NumberOfPages = 16u;
    ctx.descriptors[1].Type = EfiACPIReclaimMemory;
    ctx.descriptors[1].PhysicalStart = 0x200000ull;
    ctx.descriptors[1].NumberOfPages = 2u;

    rc = uefi_memory_map_acquire(&ctx.system_table, regions, 8u, &out_count);
    if (rc != 0) {
        return -1;
    }
    if (ctx.get_memory_map_calls < 2u) {
        return -1;
    }
    if (out_count != 2u) {
        return -1;
    }
    if (regions[0].type != VIBEOS_MEMORY_REGION_USABLE || regions[1].type != VIBEOS_MEMORY_REGION_ACPI_RECLAIMABLE) {
        return -1;
    }
    return 0;
}

static int test_boot_info_allocation_fallback(void) {
    mock_uefi_ctx_t ctx;
    vibeos_memory_region_t in_regions[3];
    vibeos_kernel_t *kernel = NULL;
    vibeos_boot_info_t *boot_info = NULL;
    int rc;

    init_mock(&ctx);
    ctx.allocate_address_fail_budget = 3u;
    ctx.descriptor_count = 1u;
    ctx.required_map_size = ctx.descriptor_size;

    memset(in_regions, 0, sizeof(in_regions));
    in_regions[0].base = 0x100000ull;
    in_regions[0].length = 0x1000ull;
    in_regions[0].type = VIBEOS_MEMORY_REGION_USABLE;
    in_regions[1].base = 0x200000ull;
    in_regions[1].length = 0x1000ull;
    in_regions[1].type = VIBEOS_MEMORY_REGION_ACPI_RECLAIMABLE;
    in_regions[2].base = 0x90000000ull;
    in_regions[2].length = 0x2000ull;
    in_regions[2].type = VIBEOS_MEMORY_REGION_MMIO;

    rc = uefi_boot_info_allocate(&ctx.system_table,
                                 in_regions,
                                 3u,
                                 0x1234000ull,
                                 0x5678000ull,
                                 NULL,
                                 &kernel,
                                 &boot_info);
    if (rc != 0 || !kernel || !boot_info) {
        return -1;
    }
    if (ctx.alloc_address_calls < 3u || ctx.alloc_any_calls < 3u) {
        return -1;
    }
    if (boot_info->memory_map_entries != 3u) {
        return -1;
    }
    if (boot_info->memory_map[2].base != in_regions[2].base) {
        return -1;
    }
    if (boot_info->acpi_rsdp != 0x1234000ull || boot_info->smbios_entry != 0x5678000ull) {
        return -1;
    }
    return 0;
}

static int test_exit_boot_services_retry_policy(void) {
    mock_uefi_ctx_t ctx;
    int rc;

    init_mock(&ctx);
    ctx.descriptor_count = 1u;
    ctx.required_map_size = ctx.descriptor_size;
    ctx.map_keys[0] = 77u;
    ctx.map_keys[1] = 78u;
    ctx.map_key_count = 2u;
    ctx.exit_statuses[0] = EFI_INVALID_PARAMETER;
    ctx.exit_statuses[1] = EFI_SUCCESS;
    ctx.exit_status_count = 2u;

    rc = uefi_boot_exit_services(&ctx.system_table, (EFI_HANDLE)(uintptr_t)0x1u, 3u);
    if (rc != 0) {
        return -1;
    }
    if (ctx.exit_boot_services_calls != 2u) {
        return -1;
    }
    if (ctx.get_memory_map_calls < 2u) {
        return -1;
    }
    return 0;
}

static int test_exit_boot_services_non_retryable_failure(void) {
    mock_uefi_ctx_t ctx;
    int rc;

    init_mock(&ctx);
    ctx.descriptor_count = 1u;
    ctx.required_map_size = ctx.descriptor_size;
    ctx.map_keys[0] = 77u;
    ctx.map_key_count = 1u;
    ctx.exit_statuses[0] = EFI_DEVICE_ERROR;
    ctx.exit_status_count = 1u;

    rc = uefi_boot_exit_services(&ctx.system_table, (EFI_HANDLE)(uintptr_t)0x1u, 3u);
    if (rc == 0) {
        return -1;
    }
    if (ctx.exit_boot_services_calls != 1u) {
        return -1;
    }
    return 0;
}

static int test_firmware_table_discovery_paths(void) {
    mock_uefi_ctx_t ctx;
    EFI_CONFIGURATION_TABLE_ENTRY tables[3];
    uint64_t acpi_rsdp = 0;
    uint64_t smbios_entry = 0;

    static const EFI_GUID acpi_guid = {
        0x8868e871u, 0xe4f1u, 0x11d3u, {0xbcu, 0x22u, 0x00u, 0x80u, 0xc7u, 0x3cu, 0x88u, 0x81u}
    };
    static const EFI_GUID acpi2_guid = {
        0x8cc346f4u, 0x6ceeu, 0x4c14u, {0x9bu, 0x85u, 0xcdu, 0xbau, 0xb8u, 0xcau, 0x5cu, 0x16u}
    };
    static const EFI_GUID smbios_guid = {
        0xeb9d2d31u, 0x2d88u, 0x11d3u, {0x9au, 0x16u, 0x00u, 0x90u, 0x27u, 0x3fu, 0xc1u, 0x4du}
    };

    init_mock(&ctx);
    memset(tables, 0, sizeof(tables));
    tables[0].VendorGuid = acpi_guid;
    tables[0].VendorTable = (void *)(uintptr_t)0x1000u;
    tables[1].VendorGuid = smbios_guid;
    tables[1].VendorTable = (void *)(uintptr_t)0x2000u;
    tables[2].VendorGuid = acpi2_guid;
    tables[2].VendorTable = (void *)(uintptr_t)0x3000u;

    ctx.system_table.ConfigurationTable = tables;
    ctx.system_table.NumberOfTableEntries = 3u;

    if (uefi_firmware_discover_tables(&ctx.system_table, &acpi_rsdp, &smbios_entry) != 0) {
        return -1;
    }
    if (acpi_rsdp != 0x3000u) {
        return -1;
    }
    if (smbios_entry != 0x2000u) {
        return -1;
    }
    return 0;
}

int main(void) {
    int failures = 0;

#define RUN_TEST(fn) do { if ((fn)() != 0) { failures++; printf("FAIL:%s\n", #fn); } } while (0)
    RUN_TEST(test_memory_map_retry_and_conversion);
    RUN_TEST(test_boot_info_allocation_fallback);
    RUN_TEST(test_exit_boot_services_retry_policy);
    RUN_TEST(test_exit_boot_services_non_retryable_failure);
    RUN_TEST(test_firmware_table_discovery_paths);
#undef RUN_TEST

    if (failures != 0) {
        printf("bootloader-test-failures=%d\n", failures);
        return 1;
    }

    printf("bootloader-tests=pass\n");
    return 0;
}
