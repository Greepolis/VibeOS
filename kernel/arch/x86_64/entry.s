# Multiboot2 header for QEMU direct kernel loading
.section .multiboot_header, "a"
.align 8
multiboot2_header:
    # Multiboot2 magic number
    .long 0xE85250D6
    
    # Architecture (0 = i386)
    .long 0
    
    # Header length
    .long (multiboot2_header_end - multiboot2_header)
    
    # Checksum: -(magic + architecture + header_length)
    # Calculated to make sum of all fields = 0
    .long -(0xE85250D6 + 0 + (multiboot2_header_end - multiboot2_header))
    
    # Optional tags would go here
    # For now, just the end tag
    
    # End tag (type=0, flags=0, size=8)
    .short 0    # type=0 (end tag)
    .short 0    # flags=0
    .long 8     # size=8
multiboot2_header_end:

.section .text
.global _start
.align 4

# x86_64 kernel entry point for QEMU q35
# Bootloader/QEMU sets up:
#   RDI = vibeos_kernel_t *kernel (or 0 if QEMU)
#   RSI = const vibeos_boot_info_t *boot_info (or 0 if QEMU)
# Stack and data are set up by bootloader/QEMU
# This entry point initializes the kernel

_start:
    # Set up stack first (before any function calls)
    mov $stack_top, %rsp
    
    # Align stack to 16 bytes as required by System V AMD64 ABI
    and $~15, %rsp
    
    # Handle Multiboot2 entry (QEMU)
    # QEMU sets: EBX = magic (0x36d76289), EAX = pointer to multiboot info
    cmp $0x36d76289, %ebx
    
    # If loaded by multiboot2, clear RDI/RSI (not set by QEMU)
    jne .bootloader_mode
    xor %rdi, %rdi
    xor %rsi, %rsi
    jmp .call_kmain
    
.bootloader_mode:
    # RDI and RSI are already set by bootloader with kernel struct and boot_info
    # RDX is unused (set to 0 by bootloader)
    
.call_kmain:
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
