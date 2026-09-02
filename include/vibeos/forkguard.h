#ifndef VIBEOS_FORKGUARD_H
#define VIBEOS_FORKGUARD_H

#include <stdint.h>

/* May this task create another one? Phase S-P6 of docs/sched/.
 *
 * The failure it exists for is not "the machine runs out of slots" - that
 * happens and is fine, and fork returns EAGAIN as it should. It is that running
 * out of slots today makes the machine **unadministrable**: the task table is
 * first-come, so a program that forks in a loop takes every slot, and then init
 * cannot start a service, the shell cannot run a command, and nothing can be
 * done about it from inside. The machine is alive and useless, which is worse
 * than a machine that refuses.
 *
 * Two rules, and they answer different halves:
 *
 *   reserved slots     - a floor under the system, so the last few slots stay
 *                        available to whoever is allowed to fix things.
 *   per-task children  - a ceiling on any one task, so one program cannot take
 *                        everything *up to* that floor either.
 *
 * The reserve alone is not enough: without the ceiling, a bomb still fills
 * every unreserved slot and every ordinary program stops working. The ceiling
 * alone is not enough either: enough cooperating processes reach the same
 * place. Neither rule is interesting on its own, which is why both are here.
 *
 * A pure decision, like the rest of kernel/sched/: it is told the counts and
 * returns a verdict, so the rules are settled by host tests rather than by
 * trying to write a fork bomb that reproduces on demand.
 */

typedef enum vibeos_fork_verdict {
    VIBEOS_FORK_OK = 0,
    VIBEOS_FORK_NO_SLOTS,        /* nothing free at all                    */
    VIBEOS_FORK_RESERVED,        /* only the system's reserve is left      */
    VIBEOS_FORK_TOO_MANY_KIDS    /* this task has had its share            */
} vibeos_fork_verdict_t;

typedef struct vibeos_forkguard_stats {
    uint64_t allowed;
    uint64_t refused_no_slots;
    uint64_t refused_reserved;
    uint64_t refused_children;
} vibeos_forkguard_stats_t;

/* `reserved` slots are kept for privileged tasks; `max_children` bounds any one
 * task's live children. Returns 0, or negative if the reserve would leave
 * nothing for anybody - a guard that refuses every fork is not a guard. */
int vibeos_forkguard_init(uint32_t slots_total, uint32_t reserved,
                          uint32_t max_children);

/* `privileged` is the kernel's own work and init: the tasks that must still be
 * able to act when a program has taken everything it is allowed to. */
vibeos_fork_verdict_t vibeos_forkguard_check(uint32_t slots_in_use,
                                             uint32_t requester_children,
                                             int privileged);

const char *vibeos_fork_verdict_name(vibeos_fork_verdict_t v);
const vibeos_forkguard_stats_t *vibeos_forkguard_stats(void);

#endif /* VIBEOS_FORKGUARD_H */
