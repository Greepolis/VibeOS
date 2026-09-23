/* PS/2 keyboard driver (IRQ1) with a small input ring buffer (image-only).
 *
 * Translates set-1 scancodes to ASCII and buffers keystrokes so a blocking
 * read() on stdin can consume them. This is the input half of a real console.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"
#include "vibeos/device.h"

#define KBD_DATA 0x60u

static inline uint8_t kbd_inb(uint16_t p) {
    uint8_t v;
    __asm__ __volatile__("inb %1,%0" : "=a"(v) : "Nd"(p));
    return v;
}

/* US set-1 scancode -> ASCII (unshifted), for make codes 0x00..0x39. */
static const char g_map[0x3A] = {
    0,   27,  '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b','\t',
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', 0,  'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'','`', 0,   '\\','z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0,   '*', 0,   ' '
};

/* Large enough for the whole boot self-test script, which is injected in one
 * go before anything reads it. Sized at 512 once, and the script outgrew it -
 * the symptom was the last command silently never running, which looks like
 * the shell hanging rather than like input being dropped. */
#define KBD_RING 2048u
static volatile char g_ring[KBD_RING];
static volatile uint32_t g_head; /* producer (IRQ) */
static volatile uint32_t g_tail; /* consumer (read) */


/* Raised by the console when the interrupt key is typed. Weak so this file
 * still links where there is no process to signal. */
__attribute__((weak)) void vibeos_x86_64_console_interrupt(void) { }

/* Control is a modifier, so it has to be tracked across keystrokes: the scan
 * code for C is the same whether or not Control is down, and the difference is
 * entirely in what arrived before it. */
static int g_ctrl_down;
#define KBD_SC_LCTRL 0x1Du
#define KBD_SC_C     0x2Eu

/* Two ways a keystroke disappears here, and they are not the same harm.
 *
 * g_dropped: the ring was full when an interrupt arrived. That is a property
 * of how fast somebody types against the ring, not a defect, so it is
 * reported and NOT must-be-zero. It has never been anything but zero in CI,
 * where nobody types - which is exactly why it is worth printing: if it is
 * ever non-zero on a boot with no human present, something is injecting.
 *
 * g_inject_truncated: MUSTBEZERO. The boot self-test injects a fixed string
 * into a ring that is empty by construction, to exercise read() on a console
 * with no human. If that injection is truncated, the self-test goes on to
 * check a *prefix* of what it meant to check, and passes. This project has
 * six recorded instances of a test that was right about the outcome and wrong
 * about the mechanism; a silently shortened input is how a seventh would
 * arrive, and nothing else in the tree could see it. */
static uint64_t g_dropped;
static uint64_t g_inject_truncated;

/* Interrupts taken, and whether the boot proved one can arrive at all.
 *
 * Nobody types in CI. The boot's own script is *injected* into the ring, which
 * raises no interrupt - so the whole IRQ1 path, the line's routing, the
 * registry's dispatch and the wake-up of whoever is blocked in read(), was
 * never exercised by any boot. Two sabotages (drop the routing; drop the wake)
 * went green because of it. The probe below makes the controller raise IRQ1 for
 * real, and the gate asserts it arrived. */
static volatile uint32_t g_irqs;
static uint64_t g_irq_proved;

static inline void kbd_outb(uint16_t p, uint8_t v) {
    __asm__ __volatile__("outb %0,%1" : : "a"(v), "Nd"(p));
}

static int kbd_irq(void) {
    uint8_t sc = kbd_inb(KBD_DATA);

    g_irqs++;
    char c;
    uint32_t next;

    if (sc & 0x80u) {
        if ((sc & 0x7Fu) == KBD_SC_LCTRL) {
            g_ctrl_down = 0;
        }
        return VIBEOS_DEV_IRQ_INPUT; /* break code (key release) */
    }
    if (sc == KBD_SC_LCTRL) {
        g_ctrl_down = 1;
        return VIBEOS_DEV_IRQ_INPUT;
    }
    if (g_ctrl_down && sc == KBD_SC_C) {
        /* Control-C is not a character. Putting it in the input ring would
         * hand the shell a byte to echo; it has to become a signal, which is
         * the whole difference between a terminal and a pipe. */
        vibeos_x86_64_console_interrupt();
        return VIBEOS_DEV_IRQ_INPUT;
    }
    if (sc >= 0x3Au) {
        return VIBEOS_DEV_IRQ_INPUT; /* outside the simple map */
    }
    c = g_map[sc];
    if (c == 0) {
        return VIBEOS_DEV_IRQ_INPUT;
    }
    next = (g_head + 1u) % KBD_RING;
    if (next != g_tail) {
        g_ring[g_head] = c;
        g_head = next;
    } else {
        g_dropped++; /* the ring was full; the keystroke is gone */
    }
    return VIBEOS_DEV_IRQ_INPUT;
}

/* Inject characters as if typed - used by the boot self-test so the read path
 * can be exercised on the non-interactive CI console. */
static int kbd_inject(const char *s) {
    uint32_t next;
    while (*s) {
        next = (g_head + 1u) % KBD_RING;
        if (next == g_tail) {
            /* Count what was left, not that it happened: "the self-test's
             * input was cut short by one character" and "by forty" are
             * different sizes of lie about what the test covered. */
            while (*s++) {
                g_inject_truncated++;
            }
            return -1;
        }
        g_ring[g_head] = *s++;
        g_head = next;
    }
    return 0;
}

/* Non-blocking: pop one buffered character, or -1 if the buffer is empty. */
static int kbd_getc(void) {
    char c;
    if (g_tail == g_head) {
        return -1;
    }
    c = g_ring[g_tail];
    g_tail = (g_tail + 1u) % KBD_RING;
    return (int)(unsigned char)c;
}

/* The two counters, on a line of their own. They used to be the middle of a
 * line kmain.c printed, next to the disk drivers' - which is how giving the
 * keyboard its first counter cost two files that had nothing to do with it
 * (check-blast-radius.py, "input device"). The gate's pattern is not anchored to
 * what surrounds it, so the move changes no assertion. */
static void kbd_report(vibeos_dev_write_fn out, void *ctx) {
    vibeos_devline_t l;

    vibeos_devline_start(&l, "[KBD] kbd_dropped=");
    vibeos_devline_hex(&l, g_dropped);
    vibeos_devline_str(&l, " MUSTBEZERO kbd_inject_truncated=");
    vibeos_devline_hex(&l, g_inject_truncated);
    vibeos_devline_str(&l, " irqs=");
    vibeos_devline_hex(&l, g_irqs);
    vibeos_devline_str(&l, " irq_proved=");
    vibeos_devline_hex(&l, g_irq_proved);
    vibeos_devline_end(&l, out, ctx);
}

#define KBD_STATUS 0x64u
#define KBD_STATUS_OUTPUT_FULL 0x01u
#define KBD_STATUS_INPUT_FULL  0x02u
/* Controller command: put the next data byte in the output buffer as though the
 * keyboard had sent it - which raises IRQ1 exactly as a keystroke does. */
#define KBD_CCMD_WRITE_OBUF 0xD2u
/* Space, released: a break code the driver reads and ignores, so the proof
 * leaves no character in the ring and no modifier state behind. */
#define KBD_SC_SPACE_RELEASE 0xB9u

static int kbd_wait_writable(void) {
    uint32_t spins;
    for (spins = 0; spins < 1000000u; spins++) {
        if ((kbd_inb(KBD_STATUS) & KBD_STATUS_INPUT_FULL) == 0u) {
            return 0;
        }
    }
    return -1;
}

/* Present regardless - the legacy controller is assumed, as it always has been.
 * What the probe finds out is whether an interrupt from it can reach this
 * driver, and it records the answer for the gate rather than acting on it.
 *
 * Runs after the lines are routed and interrupts are on. Bounded: a line that
 * never fires costs a moment of boot, not the boot. If nothing arrived the byte
 * is still in the controller's buffer, and it is taken off here - otherwise the
 * mouse's probe, next, reads a keyboard byte as its own acknowledgement. */
static int kbd_probe(const vibeos_dev_env_t *env) {
    uint32_t before = g_irqs;
    uint32_t spins;

    (void)env;
    if (kbd_wait_writable() == 0) {
        kbd_outb(KBD_STATUS, KBD_CCMD_WRITE_OBUF);
        if (kbd_wait_writable() == 0) {
            kbd_outb(KBD_DATA, KBD_SC_SPACE_RELEASE);
        }
    }
    for (spins = 0; spins < 50000000u && g_irqs == before; spins++) {
        __asm__ __volatile__("pause" ::: "memory");
    }
    g_irq_proved = (g_irqs != before) ? 1u : 0u;
    if (!g_irq_proved && (kbd_inb(KBD_STATUS) & KBD_STATUS_OUTPUT_FULL) != 0u) {
        (void)kbd_inb(KBD_DATA);
    }
    return 0;
}

static const vibeos_input_ops_t g_kbd_ops = {
    .getc = kbd_getc,
    .inject = kbd_inject,
    .pointer = 0,
};

/* IRQ1. The probe always succeeds; it exists to prove the interrupt path. */
static const vibeos_device_t g_kbd_device = {
    .name = "ps2-keyboard",
    .cls = VIBEOS_DEV_INPUT,
    .isa_irq = 1,
    .probe = kbd_probe,
    .irq = kbd_irq,
    .selftest = 0,
    .report = kbd_report,
    .ops = &g_kbd_ops,
};
VIBEOS_DEVICE(g_kbd_device);
