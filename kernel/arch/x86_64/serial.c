/* Early serial port I/O for x86_64 boot logging.
 * Targets COM1 (I/O port 0x3F8) for early diagnostics.
 */

#include "vibeos/arch_x86_64.h"

#define VIBEOS_X86_64_COM1_BASE 0x3F8

/* UART register offsets (when DLAB=0) */
#define UART_THR 0x00  /* Transmitter Holding Register */
#define UART_LSR 0x05  /* Line Status Register */
#define UART_LSR_THRE 0x20  /* Transmitter Holding Register Empty */

static int g_serial_io_available = -1;

static int vibeos_x86_64_detect_io_privilege(void) {
    uint16_t cs = 0;
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    return ((cs & 0x3u) == 0u) ? 1 : 0;
#else
    return 0;
#endif
}

static int vibeos_x86_64_serial_can_io(void) {
    if (g_serial_io_available < 0) {
        g_serial_io_available = vibeos_x86_64_detect_io_privilege();
    }
    return g_serial_io_available;
}

static inline void vibeos_x86_64_outb(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t vibeos_x86_64_inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

int vibeos_x86_64_serial_init(void) {
    /* Initialize COM1 to basic defaults (9600 baud, 8N1).
     * For M2, we assume bootloader has already initialized UART.
     * This is a placeholder for future hardware-specific init.
     */
    (void)vibeos_x86_64_serial_can_io();
    return 0;
}

void vibeos_x86_64_serial_putc(char c) {
    if (!vibeos_x86_64_serial_can_io()) {
        return;
    }

    /* Wait for Transmitter Holding Register to be empty */
    while ((vibeos_x86_64_inb(VIBEOS_X86_64_COM1_BASE + UART_LSR) & UART_LSR_THRE) == 0) {
        /* Spin: busy-wait for tx ready */
    }
    /* Send character */
    vibeos_x86_64_outb(VIBEOS_X86_64_COM1_BASE + UART_THR, c);
}

void vibeos_x86_64_serial_puts(const char *s) {
    if (!s) {
        return;
    }
    if (!vibeos_x86_64_serial_can_io()) {
        return;
    }

    for (const char *p = s; *p; ++p) {
        if (*p == '\n') {
            vibeos_x86_64_serial_putc('\r');
        }
        vibeos_x86_64_serial_putc(*p);
    }
}

void vibeos_x86_64_serial_print_hex(uint64_t value) {
    const char *hex_chars = "0123456789abcdef";
    for (int i = 60; i >= 0; i -= 4) {
        vibeos_x86_64_serial_putc(hex_chars[(value >> i) & 0xF]);
    }
}
