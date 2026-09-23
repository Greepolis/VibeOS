#ifndef VIBEOS_GUI_H
#define VIBEOS_GUI_H

/* The graphical shell (kernel/io/gui.c): a desktop, a panel, one window that is
 * a terminal for the console, and a pointer.
 *
 * A registered display (VIBEOS_DEV_DISPLAY, include/vibeos/device.h) since C7.
 * Before that it was wired in: arch_hw.c declared five of its functions, called
 * its init from the framebuffer setup and its repaint from the timer, printed
 * its counters, and kept its canary; serial.c named its putc through a weak
 * default. It had no lock, and two of its three callers run on any core.
 *
 * The seven parts (docs/core/architecture.md):
 *   1. this header;
 *   2. every piece of state in gui.c;
 *   3. its own lock: a vibeos_dev_lock_t, from the lock operations the machine
 *      registers once for every driver (vibeos_device_set_lock_ops);
 *   4. vibeos_gui_stats_t, with two must-be-zero counters;
 *   5. an init that refuses by name: vibeos_gui_why;
 *   6. the device registry's DISPLAY class;
 *   7. scripts/dev/cases/io-gui.txt and io-gui-boot.txt.
 */

#include <stdint.h>

/* The terminal's grid. Public because the torture's model has to agree with it,
 * not because anything should size a buffer from it. */
#define VIBEOS_GUI_TERM_COLS 72u
#define VIBEOS_GUI_TERM_ROWS 24u

/* The largest screen the back buffer is sized for, and the smallest the layout
 * can be drawn on. The window and its text area are computed by subtraction
 * from the screen's size; below the minimum those subtractions went negative in
 * unsigned arithmetic, and the clamp that followed wrapped past the check it
 * was meant to satisfy. On a screen 30 pixels high the text area is 25 - 26
 * rows tall, which is 0xFFFFFFFF, `y + h` wraps back under the screen's height,
 * and the fill walks off the end of the buffer. Refused now, by name - and the
 * clamps no longer add before they compare. */
#define VIBEOS_GUI_MAX_W 1920u
#define VIBEOS_GUI_MAX_H 1200u
#define VIBEOS_GUI_MIN_W 160u
#define VIBEOS_GUI_MIN_H 120u

/* Bytes of canary the GUI writes directly after the pixels it composes into.
 * The caller's buffer must hold width * height * 4 plus this. */
#define VIBEOS_GUI_GUARD_BYTES 4096u

typedef struct {
    uint64_t frames;           /* pointer repaints                              */
    uint64_t chars;            /* characters that reached the terminal          */
    uint64_t scrolls;          /* times the grid moved up a row                 */
    uint64_t term_chars;       /* characters on screen now                      */
    uint64_t reentered;        /* characters written by a core that already held
                                * the GUI's lock - a panic printing from inside
                                * the GUI - dropped rather than deadlocked       */
    uint64_t ticks_overlapped; /* repaints that found another one in progress   */
    uint64_t guard_checks;     /* times the canary was examined                 */
    uint64_t term_overrun;     /* MUST BE ZERO: a grid write out of bounds,
                                * refused. Zero by construction under the lock;
                                * the count is what says whether it is         */
    uint64_t guard_broken;     /* MUST BE ZERO: canary words found wrong - the
                                * compositor wrote past its buffer, or somebody
                                * else wrote into it                           */
} vibeos_gui_stats_t;

/* The lock (part 3) is the GUI's own vibeos_dev_lock_t, taken through the
 * operations the machine registers for every driver. They must mask
 * interrupts: the repaint runs from the timer, and a timer that interrupts a
 * console write on the same core and then waits for its lock waits forever.
 *
 * The console writes into the GUI, and a panic raised while the GUI holds its
 * lock prints. The lock provider refuses a lock its caller already holds
 * (vibeos_dev_lock returns -1), and the GUI then drops the character and counts
 * it as `reentered` instead of waiting on its own core. */

/* Bring the desktop up on a framebuffer. `back` is the caller's buffer, at
 * least width * height * 4 + VIBEOS_GUI_GUARD_BYTES bytes: this file has no
 * allocator, and a screen-sized static array would be megabytes of .bss in
 * every image whether or not a screen exists. Returns 0, or -1 with the reason
 * in vibeos_gui_why(). */
int vibeos_gui_init(uint64_t fb_base, uint32_t width, uint32_t height,
                    void *back, uint64_t back_bytes);

/* Why the last init refused, or "ok". Never null. */
const char *vibeos_gui_why(void);

int vibeos_gui_active(void);

/* One character to the terminal. From the console write path, on any core. */
void vibeos_gui_putc(char c);

/* Repaint what changed. From the timer. */
void vibeos_gui_tick(void);

/* A copy of the counters, taken under the lock. */
void vibeos_gui_stats(vibeos_gui_stats_t *out);

/* Row `row` of the terminal, NUL-terminated, into `out` (at least
 * VIBEOS_GUI_TERM_COLS + 1 bytes). For the torture's model and for nothing
 * that draws. Returns the number of characters, or -1 for a bad row. */
int vibeos_gui_term_row(uint32_t row, char *out);

/* Forget everything. For tests. */
void vibeos_gui_reset(void);

#endif
