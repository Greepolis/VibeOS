/* Task counters. Phase S-P0 of docs/sched/.
 *
 * One structure, no lock, for the same reason the memory manager's is: these
 * are counters, not state. A lost increment under contention costs a number
 * that is one too low; a lock on every task transition costs the thing being
 * measured. The three that must be zero are the ones that matter, and they are
 * incremented on paths that are already serialised by the scheduler lock.
 */

#include "vibeos/task_stats.h"

static vibeos_task_stats_t g_stats;

vibeos_task_stats_t *vibeos_task_stats(void) {
    return &g_stats;
}

void vibeos_task_stats_reset(void) {
    uint64_t *p = (uint64_t *)(void *)&g_stats;
    uint32_t i;

    for (i = 0; i < sizeof(g_stats) / sizeof(uint64_t); i++) {
        p[i] = 0;
    }
}

/* Weak defaults, so the portable kernel and the host tests link without the
 * architecture layer. A machine with no task table answers "no slots" rather
 * than failing to build - and answering honestly is better than a stub that
 * invents a plausible table. */
__attribute__((weak)) uint32_t vibeos_task_slots(void) {
    return 0;
}

__attribute__((weak)) int vibeos_task_describe(uint32_t slot,
                                               vibeos_task_desc_t *out) {
    (void)slot;
    (void)out;
    return -1;
}
