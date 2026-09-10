/* PS/2 keyboard driver (IRQ1) with a small input ring buffer (image-only).
 *
 * Translates set-1 scancodes to ASCII and buffers keystrokes so a blocking
 * read() on stdin can consume them. This is the input half of a real console.
 */

#include <stdint.h>

#include "vibeos/arch_x86_64.h"

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

/* Called from the IRQ1 handler: read the scancode and enqueue any ASCII. */
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

uint64_t vibeos_x86_64_keyboard_dropped(void) { return g_dropped; }
uint64_t vibeos_x86_64_keyboard_inject_truncated(void) { return g_inject_truncated; }

void vibeos_x86_64_keyboard_irq(void) {
    uint8_t sc = kbd_inb(KBD_DATA);
    char c;
    uint32_t next;

    if (sc & 0x80u) {
        if ((sc & 0x7Fu) == KBD_SC_LCTRL) {
            g_ctrl_down = 0;
        }
        return; /* break code (key release) */
    }
    if (sc == KBD_SC_LCTRL) {
        g_ctrl_down = 1;
        return;
    }
    if (g_ctrl_down && sc == KBD_SC_C) {
        /* Control-C is not a character. Putting it in the input ring would
         * hand the shell a byte to echo; it has to become a signal, which is
         * the whole difference between a terminal and a pipe. */
        vibeos_x86_64_console_interrupt();
        return;
    }
    if (sc >= 0x3Au) {
        return; /* outside the simple map */
    }
    c = g_map[sc];
    if (c == 0) {
        return;
    }
    next = (g_head + 1u) % KBD_RING;
    if (next != g_tail) {
        g_ring[g_head] = c;
        g_head = next;
    } else {
        g_dropped++; /* the ring was full; the keystroke is gone */
    }
}

/* Inject characters as if typed - used by the boot self-test so the read path
 * can be exercised on the non-interactive CI console. */
void vibeos_x86_64_keyboard_inject(const char *s) {
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
            return;
        }
        g_ring[g_head] = *s++;
        g_head = next;
    }
}

/* Non-blocking: pop one buffered character, or -1 if the buffer is empty. */
int vibeos_x86_64_keyboard_getc(void) {
    char c;
    if (g_tail == g_head) {
        return -1;
    }
    c = g_ring[g_tail];
    g_tail = (g_tail + 1u) % KBD_RING;
    return (int)(unsigned char)c;
}
