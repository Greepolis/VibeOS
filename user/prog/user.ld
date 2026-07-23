/* Link the VibeOS user program at a fixed low virtual address that is inside
 * the kernel's identity-mapped, user-accessible region and clear of the kernel
 * image (0x4000000) and boot structures (0x200000). */
ENTRY(_start)

SECTIONS
{
    . = 0x1000000;

    .text ALIGN(0x1000) : { *(.text) *(.text.*) }
    .rodata ALIGN(0x1000) : { *(.rodata) *(.rodata.*) }
    .data ALIGN(0x1000) : { *(.data) *(.data.*) }
    .bss ALIGN(0x1000) : { *(COMMON) *(.bss) *(.bss.*) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
