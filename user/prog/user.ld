/* Link the VibeOS user program into the dedicated user virtual-address region
 * (PML4 slot 1, base 512 GiB). User space lives in its own top-level slot so the
 * kernel's identity mapping can stay supervisor-only (US=0) while user pages are
 * mapped US=1 per process. */
ENTRY(_start)

SECTIONS
{
    /* Start past the ELF header and program headers rather than at a page
     * boundary, so the headers fall inside the first PT_LOAD segment and are
     * therefore mapped into the process. This is how every real executable is
     * laid out, and it is what makes AT_PHDR meaningful: a C runtime is handed
     * the address of the program header table and reads it, so the table has
     * to exist in the address space, not only in the file. */
    . = 0x8000000000 + SIZEOF_HEADERS;

    .text : { *(.text) *(.text.*) }
    .rodata ALIGN(0x1000) : { *(.rodata) *(.rodata.*) }
    .data ALIGN(0x1000) : { *(.data) *(.data.*) }
    .bss ALIGN(0x1000) : { *(COMMON) *(.bss) *(.bss.*) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
