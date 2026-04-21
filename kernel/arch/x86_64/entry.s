.section .text
.global _start
.align 4

# x86_64 kernel entry point for QEMU q35
# Bootloader sets up:
#   RDI = vibeos_kernel_t *kernel
#   RSI = const vibeos_boot_info_t *boot_info
# Stack and data are set up by bootloader
# This entry point initializes the kernel

_start:
    # Set up minimal stack (bootloader may provide one, but align our own)
    mov $stack_top, %rsp
    
    # Align stack to 16 bytes as required by System V AMD64 ABI
    and $~15, %rsp
    
    # RDI and RSI are already set by bootloader with kernel struct and boot_info
    # RDX is unused (set to 0 by bootloader)
    xor %rdx, %rdx
    
    # Call kernel main with boot_info in RSI and kernel in RDI
    call vibeos_kmain
    
    # If kmain returns (it shouldn't), halt
    cli
    hlt
    jmp .

# Minimal BSS section for stack
.section .bss
.align 4096
stack_base:
    .space 4096
stack_top:
