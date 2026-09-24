ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x4000000;
    __kernel_image_start = .;

    .text ALIGN(4096) : {
        KEEP(*(.text))
        *(.text.*)
    } :text

    .rodata ALIGN(4096) : {
        *(.rodata)
        *(.rodata.*)
        /* Every driver's descriptor pointer (VIBEOS_DEVICE, include/vibeos/
         * device.h), between two symbols arch_hw.c walks. Nothing names the
         * entries - that is the point. KEEP does nothing today, because the
         * image is not linked with --gc-sections; it is here for the day it is,
         * when an unreferenced section is exactly what the linker drops, and the
         * kernel would boot with no devices. The boot prints `[DEV] registered=`
         * so that day is visible either way. */
        . = ALIGN(8);
        __start_vibeos_devices = .;
        KEEP(*(vibeos_devices))
        __stop_vibeos_devices = .;
        /* The filesystem drivers' descriptors (VIBEOS_FS_DRIVER,
         * include/vibeos/storage.h), the same arrangement. */
        . = ALIGN(8);
        __start_vibeos_fs_drivers = .;
        KEEP(*(vibeos_fs_drivers))
        __stop_vibeos_fs_drivers = .;
    } :rodata

    .data ALIGN(4096) : {
        *(.data)
        *(.data.*)
    } :data

    .bss ALIGN(4096) : {
        *(COMMON)
        *(.bss)
        *(.bss.*)
    } :data
    . = ALIGN(4096);
    __kernel_image_end = .;

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
