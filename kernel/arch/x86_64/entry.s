.section .text
.global _start
.align 4

# x86_64 kernel entry point for QEMU q35
# Stack and kernel data are set up by QEMU/bootloader
# This minimal entry point initializes the kernel

_start:
    # Set up minimal stack (QEMU provides one, but let's be safe)
    mov $stack_top, %rsp
    
    # Align stack to 16 bytes as required by System V AMD64 ABI
    and $~15, %rsp
    
    # Zero argc/argv for C runtime
    xor %rdi, %rdi       # argc = 0
    xor %rsi, %rsi       # argv = NULL
    xor %rdx, %rdx       # environ = NULL
    
    # Call kernel main
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
