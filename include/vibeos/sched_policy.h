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
    VIBEOS_SCHED_KERNEL   = 0, /* the kernel's own tasks             */
    /* A program that asked for real-time scheduling (docs/abi/ L6: Linux's
     * SCHED_FIFO and SCHED_RR). Above every NORMAL task and below the
     * kernel's, and within itself ordered by a priority, 1 to 99, before
     * virtual time. */
    VIBEOS_SCHED_REALTIME = 1,
    VIBEOS_SCHED_NORMAL   = 2, /* everything a user runs             */
    VIBEOS_SCHED_IDLE     = 3, /* only when nothing else is runnable */
    VIBEOS_SCHED_CLASS_COUNT
} vibeos_sched_class_t;

#define VIBEOS_SCHED_RT_PRIO_MAX 99u

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

/* The class a slot was admitted in, or NORMAL for a slot the policy does not
 * know. The scheduler needs this and used to derive it instead - from the one
 * bit it had to hand, is_idle - which silently collapsed every class that is
 * not IDLE into NORMAL. CodeQL found it as a comparison that is always false;
 * the defect behind it was that a KERNEL task could not outrank anything,
 * because nothing could ever be seen to be in the KERNEL class. */
vibeos_sched_class_t vibeos_sched_policy_class(uint32_t slot);

/* Does a runnable slot outrank `current` by class? See sched_policy.c. */
uint64_t vibeos_sched_policy_generation(void);
int vibeos_sched_policy_should_preempt(uint32_t cpu, int current, uint64_t runnable);
int vibeos_sched_policy_set_affinity(uint32_t slot, uint32_t cpu_mask);

/* A task moved to cores it is not on: its virtual time taken down to the
 * lowest of the runnable tasks of its class those cores may run, if it is
 * above it - never raised. Linux does the same when it migrates a task,
 * placing it at the destination queue's minimum. With one virtual clock for
 * the machine, a task that had run more than the tasks the new cores were
 * running waited behind every one of them, and the other cores could not take
 * it: THREADS' C6_SCHED pinned a thread to core 1 and it was still elsewhere
 * three seconds later, on a loaded host. Called by the arch only when the task
 * is not already on an allowed core, so calling it in a loop gains nothing. */
void vibeos_sched_policy_place(uint32_t slot, uint32_t cpu_mask, uint64_t runnable);
uint32_t vibeos_sched_policy_affinity(uint32_t slot);

/* Move an admitted slot to another class, with a priority inside it (0 for
 * every class but REALTIME, 1 to VIBEOS_SCHED_RT_PRIO_MAX for it). Its virtual
 * time starts again at the new class's floor, as an admission's does: a slot
 * carrying a small number into a busy class would have it to itself. 0, or -1
 * for a slot the policy does not know, a class it does not have, a priority
 * out of range, or KERNEL, which a program cannot ask for. */
int vibeos_sched_policy_set_class(uint32_t slot, vibeos_sched_class_t cls, uint32_t rt_prio);
uint32_t vibeos_sched_policy_rt_prio(uint32_t slot);

/* Charge `ticks` of CPU to a slot. Weighted: a favourable nice makes a tick
 * count for less, which is the whole of the fairness rule.
 *
 * Kept separate from the accounting layer deliberately. That layer records what
 * happened and must stay honest; this one records what the policy thinks it is
 * worth, and confusing the two is how a scheduler comes to believe its own
 * weighting is the truth about the machine. */
void vibeos_sched_policy_charge(uint32_t slot, uint64_t ticks);
/* Ticks charged since boot, for the gate (L2 step 4). */
uint64_t vibeos_sched_policy_charged(void);

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
