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

/* The view's data source, registered by whoever has a task table.
 *
 * Defined here rather than as weak symbols: see task_stats.h. A machine with
 * nothing registered answers "no slots", which the view prints honestly
 * instead of inventing a plausible table. */
static vibeos_task_slots_fn g_slots_fn;
static vibeos_task_describe_fn g_describe_fn;

void vibeos_task_view_set_source(vibeos_task_slots_fn slots,
                                 vibeos_task_describe_fn describe) {
    g_slots_fn = slots;
    g_describe_fn = describe;
}

uint32_t vibeos_task_slots(void) {
    return g_slots_fn ? g_slots_fn() : 0u;
}

int vibeos_task_describe(uint32_t slot, vibeos_task_desc_t *out) {
    return g_describe_fn ? g_describe_fn(slot, out) : -1;
}
