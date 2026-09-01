#ifndef VIBEOS_LIFETIME_H
#define VIBEOS_LIFETIME_H

#include <stdint.h>

#include "vibeos/task.h"

/* S2: the decisions a task's death involves. Phase S-P3 of docs/sched/.
 *
 * What is portable here is not the machinery - switching CR3, freeing a kernel
 * stack and taking the scheduler lock are the architecture's - but the
 * *decisions*: how a wait status is encoded, whether a reap is legal, and the
 * order in which a slot may be taken apart. Those are where this subsystem's
 * defects have been, and none of them needs a machine to test.
 */

/* ---- wait status --------------------------------------------------------
 *
 * A wait status is not an exit code, and conflating them has bitten here: an
 * init that read only the code byte reported a segfault as a clean stop, and
 * the crashing service came back STOPPED. The encoding is written once, with
 * both directions next to each other, so a producer and a consumer cannot
 * disagree about it.
 *
 *   exit code   -> the high byte, low seven bits clear
 *   signal death-> the signal number in the low seven bits
 *
 * `128 + sig` is what a shell prints, not what the kernel stores. */
int vibeos_wait_status_make(uint32_t exit_code, uint32_t signal);
int vibeos_wait_status_exited(int status);
uint32_t vibeos_wait_status_code(int status);
uint32_t vibeos_wait_status_signal(int status);

/* ---- reaping ------------------------------------------------------------ */

typedef enum {
    VIBEOS_REAP_OK = 0,
    VIBEOS_REAP_NOT_A_CHILD,     /* not ours to reap                          */
    VIBEOS_REAP_NOT_DEAD,        /* alive: the caller must wait               */
    VIBEOS_REAP_GONE             /* the slot moved on: a stale reference      */
} vibeos_reap_verdict_t;

/* What a caller may do with this child.
 *
 * `want_tgid` is the process being waited for, or 0 for any. The child is
 * named by a reference rather than a slot: a wait4 that dropped the scheduler
 * lock and came back with a bare index would be looking at whoever holds the
 * slot now. */
vibeos_reap_verdict_t vibeos_reap_check(vibeos_task_ref_t child,
                                        uint32_t child_tgid,
                                        uint32_t child_ppid,
                                        uint32_t parent_tgid,
                                        uint32_t want_tgid);

/* ---- teardown order -----------------------------------------------------
 *
 * The two rules this subsystem has learned the hard way, made checkable
 * instead of remembered:
 *
 *   1. Tear down before announcing. A dying task's address space is released
 *      before the slot becomes a zombie, because a parent may reap the instant
 *      it sees one.
 *   2. Take what you need first, publish last. Everything the kernel reads out
 *      of a slot is read before the slot becomes reusable.
 *
 * A caller declares each step as it performs it, and the layer counts a step
 * taken out of order. It cannot prevent the mistake - the caller still has to
 * call these - but a violation becomes a counted number the boot gate asserts
 * rather than a machine that stops without a word. */
typedef enum {
    VIBEOS_TEARDOWN_BEGIN = 0,   /* the task is dying; nothing released yet    */
    VIBEOS_TEARDOWN_ASPACE,      /* its address space has been dealt with      */
    VIBEOS_TEARDOWN_HARVESTED,   /* everything needed has been copied out      */
    VIBEOS_TEARDOWN_PUBLISHED    /* the slot is reusable; nothing may touch it */
} vibeos_teardown_step_t;

void vibeos_teardown_reset(uint32_t slot);
int  vibeos_teardown_step(uint32_t slot, vibeos_teardown_step_t step);

#endif /* VIBEOS_LIFETIME_H */
