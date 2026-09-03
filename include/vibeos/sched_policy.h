#ifndef VIBEOS_SCHED_POLICY_H
#define VIBEOS_SCHED_POLICY_H

#include <stdint.h>

/* Which of several correct choices to make. Phase S-P5 of docs/sched/.
 *
 * This is the phase that makes the thing a scheduler rather than a rotation,
 * and it is last on purpose. A policy can only be built on a lifetime layer
 * that is correct, and it can only be *evaluated* with the accounting from
 * S-P4 - so building it earlier would have meant tuning a thing whose behaviour
 * was dominated by a defect.
 *
 * It is a pure function of state the caller supplies: which slots are runnable,
 * and which core is asking. It touches no task table, takes no lock and reads
 * no clock, so every rule below is decided by a host test rather than by
 * watching a boot and forming an impression.
 *
 * Named `sched_policy` rather than `policy` because `vibeos/policy.h` is
 * already the security policy - capabilities and MAC. Two unrelated subjects
 * with one name is how somebody later includes the wrong one and cannot see
 * why.
 */

#define VIBEOS_SCHED_MAX_SLOTS 64u

/* Classes, highest first. A runnable task in a higher class always runs before
 * any task in a lower one - that is what makes it a class rather than a large
 * weight, and it is the property the kernel's own work depends on.
 *
 * IDLE is a class rather than a flag so an idle task competes by the same rule
 * as everything else instead of needing a special case in the picker. */
typedef enum vibeos_sched_class {
    VIBEOS_SCHED_KERNEL = 0,   /* the kernel's own tasks             */
    VIBEOS_SCHED_NORMAL = 1,   /* everything a user runs             */
    VIBEOS_SCHED_IDLE   = 2,   /* only when nothing else is runnable */
    VIBEOS_SCHED_CLASS_COUNT
} vibeos_sched_class_t;

/* nice, as Linux spells it: lower is more favourable. */
#define VIBEOS_NICE_MIN (-20)
#define VIBEOS_NICE_MAX 19

/* The quantum, stated rather than implied.
 *
 * Preemption today happens whenever the timer fires, so the time slice is
 * "whatever the timer period is" - a number nobody chose. These are chosen, per
 * class, in ticks. */
uint32_t vibeos_sched_policy_quantum(vibeos_sched_class_t cls);

int vibeos_sched_policy_init(uint32_t slots);

/* Admit a slot to scheduling. `cpu_mask` is the set of cores it may run on;
 * pass 0 to mean "anywhere", which is what everything gets unless somebody
 * asks otherwise. */
int vibeos_sched_policy_admit(uint32_t slot, vibeos_sched_class_t cls, int nice,
                              uint32_t cpu_mask);
void vibeos_sched_policy_forget(uint32_t slot);

int vibeos_sched_policy_set_nice(uint32_t slot, int nice);
int vibeos_sched_policy_nice(uint32_t slot);
int vibeos_sched_policy_set_affinity(uint32_t slot, uint32_t cpu_mask);

/* Charge `ticks` of CPU to a slot. Weighted: a favourable nice makes a tick
 * count for less, which is the whole of the fairness rule.
 *
 * Kept separate from the accounting layer deliberately. That layer records what
 * happened and must stay honest; this one records what the policy thinks it is
 * worth, and confusing the two is how a scheduler comes to believe its own
 * weighting is the truth about the machine. */
void vibeos_sched_policy_charge(uint32_t slot, uint64_t ticks);

/* Who should run on `cpu`, given the set of slots that are runnable right now.
 *
 * Returns the slot, or negative when nothing in `runnable` may run here. Bit N
 * of `runnable` is slot N; a slot that was never admitted is ignored even if
 * its bit is set, because a picker that schedules something it knows nothing
 * about would charge it nothing and prefer it forever. */
int vibeos_sched_policy_pick(uint32_t cpu, uint64_t runnable);

/* The virtual time a slot has accumulated: the number the picker orders by, and
 * the number a test measures a weight against. */
uint64_t vibeos_sched_policy_vruntime(uint32_t slot);

/* The largest gap, in charged ticks, between a runnable slot's virtual time and
 * the smallest in its class.
 *
 * This is the starvation number. A weighted queue cannot promise that everybody
 * runs equally, only that nobody is left behind indefinitely - so what is worth
 * asserting is that this stays bounded, not that it is small. */
uint64_t vibeos_sched_policy_max_lag(uint64_t runnable);

#endif /* VIBEOS_SCHED_POLICY_H */
