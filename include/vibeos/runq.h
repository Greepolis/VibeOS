#ifndef VIBEOS_RUNQ_H
#define VIBEOS_RUNQ_H

#include <stdint.h>

/* S1: which task runs next on this CPU. Phase S-P2 of docs/sched/.
 *
 * A data structure, and therefore portable. The value of moving it is not the
 * algorithm - it is round-robin here and stays round-robin until S-P5 - but
 * that "two CPUs picked the same task", a defect this project has actually had,
 * becomes a unit test instead of a boot.
 *
 * The queue does not own task state. It is asked whether a slot is runnable
 * through a callback, so the state machine stays the single source of truth and
 * this layer cannot develop a second opinion about what READY means.
 */

#define VIBEOS_RUNQ_MAX_CPUS 8u

/* Is this slot a candidate for `cpu`? Supplied by the caller because the
 * answer involves the task table, which this layer deliberately cannot see. */
typedef int (*vibeos_runq_runnable_fn)(void *ctx, uint32_t slot, uint32_t cpu);

typedef struct vibeos_runq {
    uint32_t slots;
    uint32_t cpus;
    /* Where each CPU's search left off. Round-robin is only fair if it
     * continues rather than restarting: a queue that always scans from zero
     * gives the low slots every timeslice and starves the high ones, which is
     * the failure the fairness test below exists to catch. */
    uint32_t cursor[VIBEOS_RUNQ_MAX_CPUS];
    int idle[VIBEOS_RUNQ_MAX_CPUS];
    vibeos_runq_runnable_fn runnable;
    void *ctx;
} vibeos_runq_t;

int vibeos_runq_init(vibeos_runq_t *q, uint32_t slots, uint32_t cpus,
                     vibeos_runq_runnable_fn runnable, void *ctx);

/* Which slot this CPU should run next.
 *
 * Returns the slot, or the CPU's idle task when nothing is runnable, or -1 to
 * mean "keep running what you have" - the caller passes `current` so that a
 * task which is still runnable is not preempted for no reason.
 *
 * The caller holds whatever lock protects the task table: this layer takes
 * none, because a lock here would be a second lock over the same state. */
int vibeos_runq_pick(vibeos_runq_t *q, uint32_t cpu, int current);

/* Each CPU's fallback when nothing else can run. */
void vibeos_runq_set_idle(vibeos_runq_t *q, uint32_t cpu, int slot);

#endif /* VIBEOS_RUNQ_H */
