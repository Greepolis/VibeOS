/* Link the VibeOS user program into the dedicated user virtual-address region
 * (PML4 slot 1, base 512 GiB). User space lives in its own top-level slot so the
 * kernel's identity mapping can stay supervisor-only (US=0) while user pages are
 * mapped US=1 per process. */
ENTRY(_start)

SECTIONS
{
    . = 0x8000000000;

    .text ALIGN(0x1000) : { *(.text) *(.text.*) }
    .rodata ALIGN(0x1000) : { *(.rodata) *(.rodata.*) }
    .data ALIGN(0x1000) : { *(.data) *(.data.*) }
    .bss ALIGN(0x1000) : { *(COMMON) *(.bss) *(.bss.*) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
