/* Which task runs next. Phase S-P2 of docs/sched/.
 *
 * Round-robin over the runnable slots, one cursor per CPU. The algorithm is
 * the one this kernel already had; what is new is that it can be tested.
 */

#include "vibeos/runq.h"

int vibeos_runq_init(vibeos_runq_t *q, uint32_t slots, uint32_t cpus,
                     vibeos_runq_runnable_fn runnable, void *ctx) {
    uint32_t i;

    if (!q || slots == 0u || cpus == 0u || cpus > VIBEOS_RUNQ_MAX_CPUS ||
        !runnable) {
        return -1;
    }
    q->slots = slots;
    q->cpus = cpus;
    q->runnable = runnable;
    q->ctx = ctx;
    for (i = 0; i < VIBEOS_RUNQ_MAX_CPUS; i++) {
        q->cursor[i] = 0;
        q->idle[i] = -1;
    }
    return 0;
}

void vibeos_runq_set_idle(vibeos_runq_t *q, uint32_t cpu, int slot) {
    if (q && cpu < q->cpus) {
        q->idle[cpu] = slot;
    }
}

int vibeos_runq_pick(vibeos_runq_t *q, uint32_t cpu, int current) {
    uint32_t n;

    if (!q || cpu >= q->cpus) {
        return -1;
    }

    /* Continue from where this CPU left off, not from zero.
     *
     * Restarting the scan every time hands the low-numbered slots every
     * timeslice and starves the high ones. The previous version scanned from
     * the *current* task, which is the same idea and breaks when the current
     * task is the idle one - the scan then always begins at the same place. */
    for (n = 0; n < q->slots; n++) {
        uint32_t slot = (q->cursor[cpu] + n) % q->slots;

        if ((int)slot == current) {
            continue;   /* considered below, and only if nothing else will do */
        }
        if (q->runnable(q->ctx, slot, cpu)) {
            q->cursor[cpu] = (slot + 1u) % q->slots;
            return (int)slot;
        }
    }

    /* Nothing else is runnable. Keeping the current task is right when it can
     * still run, and is the difference between a scheduler and a machine that
     * switches to idle every tick. */
    if (current >= 0 && q->runnable(q->ctx, (uint32_t)current, cpu)) {
        return -1;
    }
    return q->idle[cpu];
}
