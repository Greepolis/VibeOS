#ifndef VIBEOS_UEFI_SERIAL_H
#define VIBEOS_UEFI_SERIAL_H

#include "uefi_protocol.h"
#include <stdio.h>

/* Simple serial logging for bootloader */

int uefi_serial_init(EFI_SYSTEM_TABLE *st);
int uefi_serial_puts(const char *str);
int uefi_serial_putc(int c);
int uefi_serial_printf(const char *fmt, ...);

#endif
