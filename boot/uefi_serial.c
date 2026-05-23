#include "uefi_serial.h"
#include "uefi_protocol.h"
#include "uefi_boot.h"
#include <stdarg.h>

#define SERIAL_BUFFER_SIZE 1024
#define UEFI_COM1_BASE 0x3F8u
#define UEFI_UART_RBR 0x00u
#define UEFI_UART_THR 0x00u
#define UEFI_UART_IER 0x01u
#define UEFI_UART_FCR 0x02u
#define UEFI_UART_LCR 0x03u
#define UEFI_UART_MCR 0x04u
#define UEFI_UART_LSR 0x05u
#define UEFI_UART_LSR_THRE 0x20u

static EFI_SERIAL_IO_PROTOCOL *g_serial_protocol = NULL;
static int g_direct_serial_ready = 0;

static inline void uefi_outb(uint16_t port, uint8_t value) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
#else
    (void)port;
    (void)value;
#endif
}

static inline uint8_t uefi_inb(uint16_t port) {
#if defined(__x86_64__) || defined(__i386__)
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
#else
    (void)port;
    return 0;
#endif
}

static void uefi_direct_serial_init(void) {
    uefi_outb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_IER), 0x00u);
    uefi_outb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_LCR), 0x80u);
    uefi_outb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_THR), 0x03u);
    uefi_outb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_IER), 0x00u);
    uefi_outb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_LCR), 0x03u);
    uefi_outb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_FCR), 0xC7u);
    uefi_outb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_MCR), 0x0Bu);
    g_direct_serial_ready = 1;
}

static void uefi_direct_serial_putc(uint8_t ch) {
    if (!g_direct_serial_ready) {
        return;
    }
    while ((uefi_inb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_LSR)) & UEFI_UART_LSR_THRE) == 0u) {
        /* Wait for transmitter to become ready. */
    }
    uefi_outb((uint16_t)(UEFI_COM1_BASE + UEFI_UART_THR), ch);
}

int uefi_serial_init(EFI_SYSTEM_TABLE *st) {
    if (!st) {
        return -1;
    }
    g_serial_protocol = NULL;
    uefi_direct_serial_init();
    return 0;
}

int uefi_serial_putc(int c) {
    if (c == '\n') {
        uefi_direct_serial_putc('\r');
    }
    uefi_direct_serial_putc((uint8_t)c);
    if (!g_serial_protocol || !g_serial_protocol->Write) {
        return c;
    }
    {
        uint8_t ch = (uint8_t)c;
        size_t size = 1;
        (void)g_serial_protocol->Write(g_serial_protocol, &size, &ch);
    }
    return c;
}

int uefi_serial_puts(const char *str) {
    if (!str) {
        return -1;
    }
    
    for (const char *p = str; *p; ++p) {
        (void)uefi_serial_putc((int)*p);
    }

    if (!g_serial_protocol || !g_serial_protocol->Write) {
        return 0;
    }
    {
        size_t len = 0;
        while (str[len] && len < SERIAL_BUFFER_SIZE - 1) {
            len++;
        }
        if (len > 0) {
            (void)g_serial_protocol->Write(g_serial_protocol, &len, (void *)str);
        }
    }
    return 0;
}

int uefi_serial_printf(const char *fmt, ...) {
    /* Placeholder for formatted output */
    char buffer[SERIAL_BUFFER_SIZE];
    va_list args;
    
    (void)buffer;  /* Suppress unused variable warning */
    (void)args;
    
    if (!fmt) {
        return -1;
    }
    
    /* Simple implementation: just print format string, ignore varargs */
    uefi_serial_puts(fmt);
    
    return 0;
}
