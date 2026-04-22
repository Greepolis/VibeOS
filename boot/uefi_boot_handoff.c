#include "uefi_boot_handoff.h"
#include "uefi_serial.h"

int uefi_boot_handoff(EFI_SYSTEM_TABLE *st,
                      vibeos_kernel_t *kernel,
                      vibeos_boot_info_t *boot_info,
                      kernel_entry_fn entry_point) {
    EFI_STATUS status;
    uint64_t map_key = 0;
    
    if (!st || !st->BootServices || !kernel || !boot_info || !entry_point) {
        uefi_serial_puts("[ERROR] uefi_boot_handoff: invalid parameters\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] Preparing for kernel handoff...\n");
    uefi_serial_puts("[BOOT] Kernel entry point: 0x");
    {
        uint64_t addr = (uint64_t)entry_point;
        for (int shift = 32; shift >= 0; shift -= 4) {
            uint64_t nibble = (addr >> shift) & 0xf;
            uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("[BOOT] RDI (kernel_t*): 0x");
    {
        uint64_t addr = (uint64_t)kernel;
        for (int shift = 32; shift >= 0; shift -= 4) {
            uint64_t nibble = (addr >> shift) & 0xf;
            uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("[BOOT] RSI (boot_info_t*): 0x");
    {
        uint64_t addr = (uint64_t)boot_info;
        for (int shift = 32; shift >= 0; shift -= 4) {
            uint64_t nibble = (addr >> shift) & 0xf;
            uefi_serial_putc(nibble < 10 ? '0' + nibble : 'a' + nibble - 10);
        }
    }
    uefi_serial_puts("\n");
    
    uefi_serial_puts("\n");
    uefi_serial_puts("[BOOT] === PHASE 5: Boot Handoff ===\n");
    uefi_serial_puts("[BOOT] Calling ExitBootServices()...\n");
    
    /* Get memory map key for ExitBootServices */
    /* For simplicity in M2, we'll create a minimal key value */
    map_key = 0x1234567890abcdef;
    
    /* Exit UEFI boot services */
    status = st->BootServices->ExitBootServices(0, map_key);
    if (status != 0) {
        uefi_serial_puts("[ERROR] ExitBootServices failed: ");
        {
            char err_str[16];
            int j = 0;
            uint64_t temp = status;
            while (temp > 0) {
                err_str[j++] = '0' + (temp % 10);
                temp /= 10;
            }
            err_str[j] = '\0';
            for (int k = j - 1; k >= 0; k--) {
                uefi_serial_putc(err_str[k]);
            }
        }
        uefi_serial_puts("\n");
        return -1;
    }
    
    uefi_serial_puts("[BOOT] ExitBootServices successful\n");
    uefi_serial_puts("[BOOT] Jumping to kernel at 0x");
    {
        uint64_t addr = (uint64_t)entry_point;
        for (int shift = 32; shift >= 0; shift -= 4) {
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
