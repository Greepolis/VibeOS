#ifndef VIBEOS_ACCOUNT_H
#define VIBEOS_ACCOUNT_H

#include <stdint.h>

/* L4 of the scheduler: what each task actually costs. Phase S-P4 of docs/sched/.
 *
 * Nothing above this phase is possible without it. A policy decides which of
 * several correct choices to make, and every way of deciding - priorities,
 * weights, whether anybody is starving - is a statement about time that has
 * already been spent. Today the kernel cannot answer "how much CPU has this
 * task had", so any policy built on top of it would be tuned by guesswork.
 *
 * Charged by tick, not by timestamp. A tick is charged to exactly one place -
 * one task, or one core's idle - so accounted plus idle equals ticks observed,
 * to within the one tick per core that can be in flight while the totals are
 * read. That bound is a fact about the counters rather than a tolerance chosen
 * to make a check pass, which is what lets the boot gate assert it.
 */

#define VIBEOS_ACCOUNT_MAX_SLOTS 64u
#define VIBEOS_ACCOUNT_MAX_CPUS 8u

typedef struct vibeos_task_account {
    uint64_t ticks;      /* ticks this task was the one running        */
    uint64_t switches;   /* times it was picked to run                 */
    uint64_t last_ran;   /* tick at which it most recently started     */
    uint64_t max_wait;   /* longest gap between being ready and running */
} vibeos_task_account_t;

/* Start accounting for `slots` tasks over `cpus` cores. Clears everything.
 * Returns 0, or negative if asked for more than this layer holds. */
int vibeos_account_init(uint32_t slots, uint32_t cpus);

/* One timer tick on `cpu`, with `slot` the task it interrupted.
 *
 * `slot_is_idle` is what separates "this core had nothing to do" from "this
 * core was running the idle task", which are the same event and must not be the
 * same number: a machine reported as fully busy because its idle task is a task
 * is a machine nobody can size.
 *
 * A negative slot is a core with no current task at all - early boot, or a core
 * between tasks - and is charged to idle. */
void vibeos_account_tick(uint32_t cpu, int slot, int slot_is_idle);

/* A task has just been picked to run on `cpu`. Records the stamp and the wait,
 * where `ready_since` is the tick it became runnable (0 if unknown, which
 * simply records no wait rather than a wrong one). */
void vibeos_account_switch(uint32_t cpu, int slot, uint64_t ready_since);

const vibeos_task_account_t *vibeos_account_task(uint32_t slot);
uint64_t vibeos_account_idle(uint32_t cpu);

/* The identity this layer exists to keep.
 *
 * `*out_charged` is every tick charged to a task, `*out_idle` every tick
 * charged to a core's idleness, and `*out_seen` the number of ticks this layer
 * was told about.
 *
 * The first two add up to the third **to within the number of cores**, and the
 * bound is the honest one rather than a hedge: a tick increments the total and
 * then one of the parts, so a core caught between those two adds leaves the
 * parts one short, and at most one tick per core can be in that state. No read
 * order removes it - the reader is not atomic with respect to the writers.
 *
 * Anything beyond that bound is a tick counted twice or lost, which
 * concurrency does not explain and which makes every number above this layer
 * wrong.
 *
 * Returns 0 when they balance within the bound, negative when they do not. */
int vibeos_account_balance(uint64_t *out_charged, uint64_t *out_idle,
                           uint64_t *out_seen);

/* Ticks since accounting started, as this layer counted them. */
uint64_t vibeos_account_ticks(void);

/* Ticks this layer refused: a core index it was not sized for.
 *
 * It must be zero, and it needs its own counter because a refused tick is
 * *invisible* to the balance above - charged plus idle still equals seen, since
 * a refused tick was never seen. An accounting layer told about four cores and
 * sized for one balances perfectly while reporting a quarter of the machine. */
uint64_t vibeos_account_dropped(void);

#endif /* VIBEOS_ACCOUNT_H */
