#include "uefi_boot_handoff.h"
#include "uefi_serial.h"
#include "uefi_memory.h"

#define UEFI_BOOT_EXIT_MAX_ATTEMPTS 3u

static const char *uefi_status_to_string(EFI_STATUS status) {
    switch (status) {
        case EFI_SUCCESS:
            return "EFI_SUCCESS";
        case EFI_LOAD_ERROR:
            return "EFI_LOAD_ERROR";
        case EFI_INVALID_PARAMETER:
            return "EFI_INVALID_PARAMETER";
        case EFI_UNSUPPORTED:
            return "EFI_UNSUPPORTED";
        case EFI_BAD_BUFFER_SIZE:
            return "EFI_BAD_BUFFER_SIZE";
        case EFI_BUFFER_TOO_SMALL:
            return "EFI_BUFFER_TOO_SMALL";
        case EFI_NOT_READY:
            return "EFI_NOT_READY";
        case EFI_DEVICE_ERROR:
            return "EFI_DEVICE_ERROR";
        case EFI_WRITE_PROTECTED:
            return "EFI_WRITE_PROTECTED";
        case EFI_OUT_OF_RESOURCES:
            return "EFI_OUT_OF_RESOURCES";
        case EFI_VOLUME_CORRUPTED:
            return "EFI_VOLUME_CORRUPTED";
        default:
            return "EFI_UNKNOWN_STATUS";
    }
}

static void uefi_serial_put_hex64(uint64_t value) {
    int shift;
    uefi_serial_puts("0x");
    for (shift = 60; shift >= 0; shift -= 4) {
        uint64_t nibble = (value >> (uint32_t)shift) & 0xfull;
        uefi_serial_putc((int)(nibble < 10 ? ('0' + nibble) : ('a' + nibble - 10)));
    }
}

int uefi_boot_exit_services(EFI_SYSTEM_TABLE *st, EFI_HANDLE image_handle, uint32_t max_attempts) {
    uint32_t attempt = 0;

    if (!st || !st->BootServices || !st->BootServices->ExitBootServices || !st->BootServices->GetMemoryMap) {
        uefi_serial_puts("[ERROR] uefi_boot_exit_services: missing boot services\n");
        return -1;
    }

    if (max_attempts == 0) {
        max_attempts = UEFI_BOOT_EXIT_MAX_ATTEMPTS;
    }

    for (attempt = 0; attempt < max_attempts; attempt++) {
        size_t map_key = 0;
        EFI_STATUS status;

        if (uefi_memory_map_fetch_key(st, &map_key) != 0) {
            uefi_serial_puts("[ERROR] Failed to fetch memory map key before ExitBootServices\n");
            return -1;
        }

        status = st->BootServices->ExitBootServices(image_handle, map_key);
        if (status == EFI_SUCCESS) {
            uefi_serial_puts("[BOOT] ExitBootServices successful\n");
            return 0;
        }

        uefi_serial_puts("[WARN] ExitBootServices failed, status=");
        uefi_serial_puts(uefi_status_to_string(status));
        uefi_serial_puts(" (");
        uefi_serial_put_hex64((uint64_t)status);
        uefi_serial_puts(")\n");

        if (status != EFI_INVALID_PARAMETER) {
            return -1;
        }

        uefi_serial_puts("[BOOT] Retrying ExitBootServices after refreshing memory map key\n");
    }

    uefi_serial_puts("[ERROR] ExitBootServices retry budget exhausted\n");
    return -1;
}

int uefi_boot_handoff(EFI_SYSTEM_TABLE *st,
                      EFI_HANDLE image_handle,
                      vibeos_kernel_t *kernel,
                      vibeos_boot_info_t *boot_info,
                      kernel_entry_fn entry_point) {
    if (!st || !st->BootServices || !kernel || !boot_info || !entry_point) {
        uefi_serial_puts("[ERROR] uefi_boot_handoff: invalid parameters\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] Preparing for kernel handoff...\n");
    uefi_serial_puts("[BOOT] Kernel entry point: 0x");
    {
        uint64_t addr = (uint64_t)entry_point;
        for (int shift = 60; shift >= 0; shift -= 4) {
            uint64_t nibble = (addr >> shift) & 0xf;
            uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("[BOOT] RDI (kernel_t*): 0x");
    {
        uint64_t addr = (uint64_t)kernel;
        for (int shift = 60; shift >= 0; shift -= 4) {
            uint64_t nibble = (addr >> shift) & 0xf;
            uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("[BOOT] RSI (boot_info_t*): 0x");
    {
        uint64_t addr = (uint64_t)boot_info;
        for (int shift = 60; shift >= 0; shift -= 4) {
            uint64_t nibble = (addr >> shift) & 0xf;
            uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("\n");
    uefi_serial_puts("[BOOT] === PHASE 5: Boot Handoff ===\n");
    uefi_serial_puts("[BOOT] Calling ExitBootServices()...\n");

    if (uefi_boot_exit_services(st, image_handle, UEFI_BOOT_EXIT_MAX_ATTEMPTS) != 0) {
        return -1;
    }

    uefi_serial_puts("[BOOT] Jumping to kernel at 0x");
    {
        uint64_t addr = (uint64_t)entry_point;
        for (int shift = 60; shift >= 0; shift -= 4) {
            uint64_t nibble = (addr >> shift) & 0xf;
            uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
    }
    uefi_serial_puts("...\n");
    uefi_serial_puts("[BOOT] === PHASE 5 COMPLETE ===\n");
    uefi_serial_puts("[BOOT] Handoff in progress\n\n");
    
    /* Jump to kernel with RDI=kernel, RSI=boot_info */
    /* Implemented via inline asm to set registers and call */
    __asm__ __volatile__(
        "mov %0, %%rdi\n\t"      /* RDI = kernel */
        "mov %1, %%rsi\n\t"      /* RSI = boot_info */
        "call *%2\n\t"           /* call entry_point */
        :
        : "r"(kernel), "r"(boot_info), "r"(entry_point)
        : "rdi", "rsi", "rax", "rcx", "rdx", "r8", "r9", "r10", "r11", "memory"
    );
    
    /* Should not reach here */
    uefi_serial_puts("[ERROR] Kernel returned unexpectedly\n");
    return 1;
}
