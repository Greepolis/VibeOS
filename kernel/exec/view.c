/* The exec refusal counters, printed. Phase X-P0 of docs/exec/.
 *
 * Deciding what to say is portable; only reaching the serial port is not. The
 * same split as kernel/sched/view.c, and for the same reason: every loader
 * defect this project has had was diagnosed by adding a print statement to an
 * 8000-line file and reading serial output.
 */

#include "vibeos/exec_stats.h"
#include "vibeos/arch_x86_64.h"

void vibeos_exec_print_stats(void) {
    const vibeos_exec_stats_t *s = vibeos_exec_stats();
    uint32_t i;

    /* One line, one critical section. A summary assembled from a dozen
     * serial_puts calls is a dozen of them, and the boot gate parses this. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[EXEC] loaded=0x");
    vibeos_x86_64_serial_print_hex(s->loaded);
    vibeos_x86_64_serial_puts(" pages_from_cache=0x");
    vibeos_x86_64_serial_print_hex(s->pages_from_cache);
    vibeos_x86_64_serial_puts(" pages_copied=0x");
    vibeos_x86_64_serial_print_hex(s->pages_copied);
    /* The refusals are announced, not just appended.
     *
     * The gate matched them with a bare `name=0x...` pattern, which worked
     * only while nothing else on the line had that shape. Adding two counters
     * broke it silently - `pages_from_cache=0x..` would have been read as a
     * refusal called "cache". A marker costs six characters and makes the
     * boundary a fact rather than a coincidence. */
    vibeos_x86_64_serial_puts(" refused:");
    for (i = 1; i < (uint32_t)VIBEOS_EXEC_REASON_COUNT; i++) {
        /* Only the reasons that fired. A row of zeroes is noise, and the gate
         * asserts on which names are present rather than on their values -
         * so a reason that never happened must not appear at all. */
        if (s->refused[i] == 0ull) {
            continue;
        }
        vibeos_x86_64_serial_puts(" ");
        vibeos_x86_64_serial_puts(vibeos_exec_fail_name((vibeos_exec_fail_t)i));
        vibeos_x86_64_serial_puts("=0x");
        vibeos_x86_64_serial_print_hex(s->refused[i]);
    }
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
}
