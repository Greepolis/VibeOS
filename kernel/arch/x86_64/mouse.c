/* PS/2 mouse driver (IRQ12).
 *
 * The controller multiplexes two devices onto one pair of I/O ports, so the
 * mouse is reached by prefixing each command with 0xD4 - "the next byte is for
 * the auxiliary device" - and its data arrives on the same port as the
 * keyboard's, distinguished only by which interrupt fired.
 *
 * The device reports movement as a three-byte packet, and the packets are a
 * stream with no framing: if a byte is dropped or an extra one appears, every
 * packet after it is misread as a shifted window of the previous two. Bit 3 of
 * the first byte is always set, which is the only resynchronisation point
 * there is, so it is checked rather than assumed.
 *
 * Movement is signed and delivered in a sign-and-magnitude-ish encoding: the
 * magnitude is in the byte, and the sign is a bit in the status byte. Reading
 * the byte as a signed char without consulting that bit produces a cursor that
 * moves the right amount in the wrong direction for half of all movements.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

#define PS2_DATA 0x60u
#define PS2_CMD  0x64u
#define PS2_STATUS 0x64u

#define PS2_STATUS_OUTPUT_FULL 0x01u
#define PS2_STATUS_INPUT_FULL  0x02u
#define PS2_STATUS_FROM_AUX    0x20u

static uint8_t ms_inb(uint16_t port) {
    uint8_t v;
    __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void ms_outb(uint16_t port, uint8_t value) {
    __asm__ __volatile__("outb %0, %1" : : "a"(value), "Nd"(port));
}

/* The controller is slow relative to the CPU and has no interrupt for "ready
 * to accept a command", so each command waits for the input buffer to drain.
 * Bounded: a missing or wedged controller must not hang the boot. */
static int ps2_wait_writable(void) {
    uint32_t spins;
    for (spins = 0; spins < 100000u; spins++) {
        if ((ms_inb(PS2_STATUS) & PS2_STATUS_INPUT_FULL) == 0u) {
            return 0;
        }
    }
    return -1;
}

static int ps2_wait_readable(void) {
    uint32_t spins;
    for (spins = 0; spins < 100000u; spins++) {
        if ((ms_inb(PS2_STATUS) & PS2_STATUS_OUTPUT_FULL) != 0u) {
            return 0;
        }
    }
    return -1;
}

/* Send a byte to the mouse rather than to the keyboard, and collect its ACK. */
static int mouse_command(uint8_t byte) {
    if (ps2_wait_writable() != 0) {
        return -1;
    }
    ms_outb(PS2_CMD, 0xD4u);
    if (ps2_wait_writable() != 0) {
        return -1;
    }
    ms_outb(PS2_DATA, byte);
    if (ps2_wait_readable() != 0) {
        return -1;
    }
    return (ms_inb(PS2_DATA) == 0xFAu) ? 0 : -1;   /* 0xFA is ACK */
}

/* Current pointer state. Read by whatever draws the cursor; written only from
 * the interrupt, so it is volatile and each field is independently consistent
 * even though the triple is not atomic - a cursor that is one packet stale is
 * not a defect worth a lock on the interrupt path. */
static volatile int32_t g_x, g_y;
static volatile uint32_t g_buttons;
static volatile uint32_t g_packets;
static int32_t g_max_x = 639, g_max_y = 479;
static int g_ready;

static uint8_t g_packet[3];
static uint32_t g_phase;

/* Provided by the interrupt layer; weak so this file also builds alone. */
__attribute__((weak)) uint64_t vibeos_x86_64_irq_save(void) { return 0; }
__attribute__((weak)) void vibeos_x86_64_irq_restore(uint64_t flags) { (void)flags; }

int vibeos_x86_64_mouse_init(uint32_t width, uint32_t height) {
    uint8_t status;
    /* The whole sequence runs with interrupts masked.
     *
     * Every command the mouse acknowledges also raises IRQ12, and by the time
     * this runs that line is already unmasked - so the handler reads the ACK
     * out of the port before the polling loop below can see it, every command
     * appears to fail, and the driver concludes there is no mouse attached.
     * That is exactly what happened. */
    uint64_t flags = vibeos_x86_64_irq_save();

    if (width > 0u && height > 0u) {
        g_max_x = (int32_t)width - 1;
        g_max_y = (int32_t)height - 1;
        g_x = (int32_t)(width / 2u);
        g_y = (int32_t)(height / 2u);
    }

    /* Enable the auxiliary port. */
    if (ps2_wait_writable() != 0) {
        vibeos_x86_64_irq_restore(flags);
        return -1;
    }
    ms_outb(PS2_CMD, 0xA8u);

    /* Turn on interrupts for it in the controller's configuration byte. The
     * byte has to be read, modified and written back: overwriting it wholesale
     * would switch off the keyboard, which is how a machine ends up with a
     * working mouse and no way to type. */
    if (ps2_wait_writable() != 0) {
        vibeos_x86_64_irq_restore(flags);
        return -1;
    }
    ms_outb(PS2_CMD, 0x20u);
    if (ps2_wait_readable() != 0) {
        vibeos_x86_64_irq_restore(flags);
        return -1;
    }
    status = ms_inb(PS2_DATA);
    status |= 0x02u;    /* second-port interrupt */
    status &= (uint8_t)~0x20u;   /* clear second-port clock disable */
    if (ps2_wait_writable() != 0) {
        vibeos_x86_64_irq_restore(flags);
        return -1;
    }
    ms_outb(PS2_CMD, 0x60u);
    if (ps2_wait_writable() != 0) {
        vibeos_x86_64_irq_restore(flags);
        return -1;
    }
    ms_outb(PS2_DATA, status);

    if (mouse_command(0xF6u) != 0) {   /* defaults */
        vibeos_x86_64_irq_restore(flags);
        return -1;
    }
    if (mouse_command(0xF4u) != 0) {   /* enable reporting */
        vibeos_x86_64_irq_restore(flags);
        return -1;
    }
    g_phase = 0;
    g_ready = 1;
    vibeos_x86_64_irq_restore(flags);
    return 0;
}

/* Called from the IRQ12 handler. */
void vibeos_x86_64_mouse_irq(void) {
    uint8_t status = ms_inb(PS2_STATUS);
    uint8_t byte;

    if ((status & PS2_STATUS_OUTPUT_FULL) == 0u) {
        return;
    }
    byte = ms_inb(PS2_DATA);
    if ((status & PS2_STATUS_FROM_AUX) == 0u) {
        return;   /* keyboard byte arriving on the mouse vector; not ours */
    }
    if (!g_ready) {
        return;
    }

    /* Bit 3 of the first byte is always set. Using it to resynchronise is what
     * keeps a single dropped byte from corrupting every packet that follows. */
    if (g_phase == 0u && (byte & 0x08u) == 0u) {
        return;
    }
    g_packet[g_phase++] = byte;
    if (g_phase < 3u) {
        return;
    }
    g_phase = 0;

    {
        uint8_t flags = g_packet[0];
        int32_t dx = (int32_t)g_packet[1];
        int32_t dy = (int32_t)g_packet[2];

        /* Overflow bits mean the device gave up counting; the magnitude is
         * meaningless, so the packet is dropped rather than turned into a
         * large jump in an arbitrary direction. */
        if ((flags & 0xC0u) != 0u) {
            return;
        }
        /* The sign lives in the flags byte, not in the data byte. */
        if (flags & 0x10u) {
            dx -= 256;
        }
        if (flags & 0x20u) {
            dy -= 256;
        }

        /* The device's Y grows upwards; a screen's grows downwards. */
        g_x += dx;
        g_y -= dy;
        if (g_x < 0) {
            g_x = 0;
        }
        if (g_y < 0) {
            g_y = 0;
        }
        if (g_x > g_max_x) {
            g_x = g_max_x;
        }
        if (g_y > g_max_y) {
            g_y = g_max_y;
        }
        g_buttons = flags & 0x07u;
        g_packets++;
    }
}

int vibeos_x86_64_mouse_ready(void) {
    return g_ready;
}

void vibeos_x86_64_mouse_state(int32_t *out_x, int32_t *out_y, uint32_t *out_buttons) {
    if (out_x) {
        *out_x = g_x;
    }
    if (out_y) {
        *out_y = g_y;
    }
    if (out_buttons) {
        *out_buttons = g_buttons;
    }
}

uint32_t vibeos_x86_64_mouse_packets(void) {
    return g_packets;
}
