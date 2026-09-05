/* Early serial port I/O for x86_64 boot logging.
 * Targets COM1 (I/O port 0x3F8) for early diagnostics.
 */

#include "vibeos/arch_x86_64.h"

#define VIBEOS_X86_64_COM1_BASE 0x3F8

/* UART register offsets (when DLAB=0) */
#define UART_RBR 0x00  /* Receiver Buffer Register */
#define UART_THR 0x00  /* Transmitter Holding Register */
#define UART_LSR 0x05  /* Line Status Register */
#define UART_LSR_DR 0x01  /* Data Ready */
#define UART_LSR_THRE 0x20  /* Transmitter Holding Register Empty */

static int g_serial_io_available = -1;

static int vibeos_x86_64_detect_io_privilege(void) {
    unsigned long cs = 0;
#if defined(__x86_64__) || defined(__i386__)
    /* Read CS register to detect privilege level.
     * In kernel mode (ring 0), CS & 0x3 == 0
     * Use constraint "=&r" (early clobber) to prevent optimization issues.
     * We use 'unsigned long' to match register size on both 32/64-bit.
     */
    __asm__ __volatile__("mov %%cs, %0" : "=&r"(cs) : : "memory");
    return ((cs & 0x3u) == 0u) ? 1 : 0;
#else
    /* Non-x86 platforms: assume no I/O privilege (fail safe) */
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
    if (!vibeos_x86_64_serial_can_io()) {
        return;
    }
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8_t vibeos_x86_64_inb(uint16_t port) {
    uint8_t value;
    if (!vibeos_x86_64_serial_can_io()) {
        return 0;
    }
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

/* The graphical console, when there is one. Weak so this file still links in
 * builds and tests that have no framebuffer at all. */
__attribute__((weak)) void vibeos_x86_64_gui_putc(char c) { (void)c; }

void vibeos_x86_64_serial_putc(char c) {
    /* Everything the system says on the serial line also appears in the
     * on-screen terminal, so a machine with a monitor and no serial cable
     * shows the same thing. Done here rather than at each call site: there is
     * one console, and it should not be possible to write to half of it. */
    vibeos_x86_64_gui_putc(c);

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

/* Once more than one CPU is running, two cores writing the console byte by byte
 * would interleave mid-line. The lock below serializes them.
 *
 * It is recursive so a caller can bracket a whole multi-part message
 * (vibeos_x86_64_serial_lock/unlock) while the individual puts/print_hex calls
 * inside still take it. Ownership is by CPU id, which the arch layer provides;
 * the weak default keeps host builds (single-threaded, no per-CPU block)
 * working unchanged. */
__attribute__((weak)) uint32_t vibeos_x86_64_cpu_id(void) { return 0; }

/* Interrupts must stay masked for the whole critical section - see below. The
 * weak defaults do nothing so host tests, which run in ring 3 where cli would
 * fault, link and behave unchanged. */
__attribute__((weak)) uint64_t vibeos_x86_64_irq_save(void) { return 0; }
__attribute__((weak)) void vibeos_x86_64_irq_restore(uint64_t flags) { (void)flags; }

#define SERIAL_NO_OWNER 0xFFFFFFFFu

static volatile int g_serial_lock;
static volatile uint32_t g_serial_owner = SERIAL_NO_OWNER;
static volatile int g_serial_depth;
/* Unlock calls from a core that did not hold the lock. Should be zero; the
 * boot gate fails if it is not. */
static volatile uint64_t g_serial_bad_unlocks;
static uint64_t g_serial_flags;   /* saved by the outermost acquire */

/* How often the acquire above gave up waiting and took the lock anyway, and
 * who was holding it when that happened. Both should be zero; the boot gate
 * fails if the count is not. */
static volatile uint64_t g_serial_stuck;
static volatile uint32_t g_serial_stuck_owner;

uint64_t vibeos_x86_64_serial_stuck(void) {
    return g_serial_stuck;
}

uint32_t vibeos_x86_64_serial_stuck_owner(void) {
    return g_serial_stuck_owner;
}

/* Recursion is keyed to the CPU, so the holder must not move while it holds
 * the lock - and a task does move. With interrupts left enabled, the timer
 * could preempt a task inside the critical section and resume it on another
 * core; its next nested acquire then saw a different owner and spun forever on
 * a lock it already held. Three cores deadlocked exactly that way, with
 * interleaved bytes in the output as the first symptom. Masking interrupts
 * removes both the interleaving and the migration. */
void vibeos_x86_64_serial_lock(void) {
    uint64_t flags = vibeos_x86_64_irq_save();
    uint32_t me = vibeos_x86_64_cpu_id();

    /* g_serial_lock is checked as well as the owner: the recursive branch is
     * only reachable when somebody actually holds the lock, so a stale or
     * duplicated identity can no longer walk in behind a real owner. That is
     * belt and braces - cpu ids are unique now - but this is the path whose
     * failure mode is a boot gate reporting things that did not happen. */
    if (g_serial_lock && g_serial_depth > 0 && g_serial_owner == me) {
        /* Already ours: keep interrupts masked, the outer release restores. */
        g_serial_depth++;
        return;
    }
    {
        /* A bound, because the alternative here is silence.
         *
         * Every other lock in this kernel panics when its bound fires. This
         * one cannot: a panic prints, and printing goes through the very lock
         * whose state is in question - the comment on bad_unlocks below makes
         * the same point. So it takes the lock instead, records that it did,
         * and lets the machine carry on talking.
         *
         * That trades a correct line for a possibly interleaved one, and it is
         * the right trade twice over. Interleaving is *detected*: the boot
         * gate's first check is interleaved_lines, so a garbled line is caught
         * and named. Silence is not detectable at all - it is the failure this
         * project has spent the most time on, and the one where every piece of
         * evidence is missing rather than wrong.
         *
         * The count is asserted zero by the gate, like bad_unlocks. A machine
         * that says "the console lock was stuck, held by cpu 2" is one
         * somebody can fix; a machine that says nothing is where four
         * investigations have ended. */
        uint64_t spins = 0;

        while (__sync_lock_test_and_set(&g_serial_lock, 1)) {
            while (g_serial_lock) {
                if (++spins > 200000000ull) {
                    g_serial_stuck++;
                    g_serial_stuck_owner = g_serial_owner;
                    goto taken;
                }
                __asm__ __volatile__("pause" ::: "memory");
            }
        }
    }
taken:
    g_serial_owner = me;
    g_serial_depth = 1;
    g_serial_flags = flags;
}

/* Counted, not panicked on: this is the console lock, so a panic here would
 * try to print through the very lock whose state is in question. The count is
 * asserted on by the boot gate instead, which is the same red light one step
 * further out. */
uint64_t vibeos_x86_64_serial_bad_unlocks(void) {
    return g_serial_bad_unlocks;
}

void vibeos_x86_64_serial_unlock(void) {
    uint64_t flags;

    /* Releasing a lock this core does not hold is not a tidiness problem. It
     * frees the lock out from under whichever core *is* mid-line, lets a third
     * writer into that critical section, and restores somebody else's
     * interrupt flag on the way out. One missing serial_lock() in hw_log_emit
     * did exactly that: output interleaved mid-word, split the markers the
     * boot gate matches on, and the gate reported failures that had never
     * happened - which cost two full investigations into crashes that were a
     * marker cut in half.
     *
     * So: refuse, and count it. Returning without touching anything leaves the
     * real owner's critical section intact, which is the safe direction. */
    if (g_serial_depth <= 0 || g_serial_owner != vibeos_x86_64_cpu_id()) {
        g_serial_bad_unlocks++;
        return;
    }

    if (g_serial_depth > 1) {
        g_serial_depth--;
        return;
    }
    flags = g_serial_flags;
    g_serial_depth = 0;
    g_serial_owner = SERIAL_NO_OWNER;
    __sync_lock_release(&g_serial_lock);
    vibeos_x86_64_irq_restore(flags);
}

#define serial_lock() vibeos_x86_64_serial_lock()
#define serial_unlock() vibeos_x86_64_serial_unlock()

void vibeos_x86_64_serial_puts(const char *s) {
    if (!s) {
        return;
    }
    if (!vibeos_x86_64_serial_can_io()) {
        return;
    }

    serial_lock();
    for (const char *p = s; *p; ++p) {
        if (*p == '\n') {
            vibeos_x86_64_serial_putc('\r');
        }
        vibeos_x86_64_serial_putc(*p);
    }
    serial_unlock();
}

void vibeos_x86_64_serial_print_hex(uint64_t value) {
    const char *hex_chars = "0123456789abcdef";
    serial_lock();
    for (int i = 60; i >= 0; i -= 4) {
        vibeos_x86_64_serial_putc(hex_chars[(value >> i) & 0xF]);
    }
    serial_unlock();
}

int vibeos_x86_64_serial_available(void) {
    return vibeos_x86_64_serial_can_io();
}

int vibeos_x86_64_serial_can_read(void) {
    if (!vibeos_x86_64_serial_can_io()) {
        return 0;
    }
    return (vibeos_x86_64_inb(VIBEOS_X86_64_COM1_BASE + UART_LSR) & UART_LSR_DR) ? 1 : 0;
}

int vibeos_x86_64_serial_readc(void) {
    if (!vibeos_x86_64_serial_can_read()) {
        return -1;
    }
    return (int)vibeos_x86_64_inb(VIBEOS_X86_64_COM1_BASE + UART_RBR);
}
