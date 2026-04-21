/* UEFI x86_64 PE32+ Linker Script for VibeOS Bootloader */

ENTRY(efi_main)

SECTIONS
{
    /* PE32+ entry point */
    .text 0x0 : {
        *(.text)
        *(.text.*)
    }
    
    /* Read-only data */
    .rodata : {
        *(.rodata)
        *(.rodata.*)
        *(.data.rel.ro)
    }
    
    /* Initialized data */
    .data : {
        *(.data)
        *(.data.*)
    }
    
    /* Uninitialized data */
    .bss : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
    }
    
    /* Debug information (optional, stripped in release) */
    .debug_info 0 : { *(.debug_info) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_line 0 : { *(.debug_line) }
    .debug_frame 0 : { *(.debug_frame) }
    .debug_str 0 : { *(.debug_str) }
    .debug_loc 0 : { *(.debug_loc) }
    
    /* Discard unused sections */
    /DISCARD/ : {
        *(.gnu.hash)
        *(.gnu.version*)
        *(.note.GNU-stack)
    }
}
