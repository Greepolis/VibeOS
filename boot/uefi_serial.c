#include "uefi_serial.h"
#include "uefi_protocol.h"
#include "uefi_boot.h"
#include <stdarg.h>

#define SERIAL_BUFFER_SIZE 1024

static EFI_SYSTEM_TABLE *g_system_table = NULL;
static EFI_SERIAL_IO_PROTOCOL *g_serial_protocol = NULL;

int uefi_serial_init(EFI_SYSTEM_TABLE *st) {
    if (!st) {
        return -1;
    }
    g_system_table = st;
    
    /* Try to locate EFI_SERIAL_IO_PROTOCOL via GUID */
    if (st->BootServices && st->BootServices->LocateProtocol) {
        EFI_GUID serial_guid = {
            0xe0c6084e, 0x51a3, 0x4d55,
            {0xb2, 0x67, 0x4e, 0xaa, 0x2f, 0xe7, 0x6d, 0xcd}
        };
        if (st->BootServices->LocateProtocol(&serial_guid, NULL, (void **)&g_serial_protocol) == EFI_SUCCESS) {
            /* Configure: 115200 baud, 8N1 */
            if (g_serial_protocol && g_serial_protocol->SetAttributes) {
                g_serial_protocol->SetAttributes(g_serial_protocol, 115200, 0, 0, 0, 8, 1);
            }
            return 0;
        }
    }
    
    /* Serial protocol not found, but continue (may use video console fallback) */
    return 0;
}

int uefi_serial_putc(int c) {
    if (!g_serial_protocol || !g_serial_protocol->Write) {
        return c;
    }
    
    uint8_t ch = (uint8_t)c;
    size_t size = 1;
    g_serial_protocol->Write(g_serial_protocol, &size, &ch);
    
    return c;
}

int uefi_serial_puts(const char *str) {
    if (!str) {
        return -1;
    }
    
    if (!g_serial_protocol || !g_serial_protocol->Write) {
        /* Fallback: no serial, silent fail */
        return 0;
    }
    
    size_t len = 0;
    while (str[len] && len < SERIAL_BUFFER_SIZE - 1) {
        len++;
    }
    
    if (len > 0) {
        g_serial_protocol->Write(g_serial_protocol, &len, (void *)str);
    }
    
    return 0;
}

int uefi_serial_printf(const char *fmt, ...) {
    /* Placeholder for formatted output */
    char buffer[SERIAL_BUFFER_SIZE];
    va_list args;
    
    if (!fmt) {
        return -1;
    }
    
    /* Simple implementation: just print format string, ignore varargs */
    uefi_serial_puts(fmt);
    
    return 0;
}
