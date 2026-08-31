#ifndef VIBEOS_TASK_H
#define VIBEOS_TASK_H

#include <stdint.h>

#include "vibeos/task_stats.h"

/* S0: the task table's state machine and its tenancy. Phase S-P1 of
 * docs/sched/.
 *
 * This layer owns two things about every slot - what state it is in, and which
 * tenancy of the slot this is - and nothing else. The architecture keeps the
 * registers, the address space and the file descriptors; it stops keeping the
 * answer to "is this slot free?", because that answer was written from nine
 * places and read from forty.
 *
 * Every defect this subsystem has produced is a state change made by code that
 * knew part of the truth: a slot published as reusable while still being
 * written to, a dying task announced before its address space was gone, a
 * recycled slot carrying a stale cr3. A transition table cannot prevent all of
 * them, but it turns the illegal ones into a counted refusal instead of a
 * machine that stops without a word.
 */

typedef enum {
    VIBEOS_TASK_FREE = 0,   /* nobody's; may be allocated                     */
    VIBEOS_TASK_READY,      /* schedulable                                    */
    VIBEOS_TASK_RUNNING,    /* on a CPU                                       */
    VIBEOS_TASK_ZOMBIE,     /* dead, not yet reaped                           */
    VIBEOS_TASK_BLOCKED,    /* waiting for an event                           */
    VIBEOS_TASK_SETUP,      /* claimed by a creator still filling it in       */
    VIBEOS_TASK_STATE_COUNT
} vibeos_task_state_t;

/* A reference that survives the lock it was taken under.
 *
 * Decision T1: everywhere, not only where a stale index has already bitten. A
 * bare slot number is valid forever and names whoever holds the slot now, so a
 * reference kept across a lock release silently becomes a reference to a
 * different task - which is the shape of the wedge, the reaper defect, and the
 * exec defect alike. The generation makes that detectable, and a rule with
 * exceptions is how the last one happened: the right question existed in one
 * place and not the other. */
typedef struct vibeos_task_ref {
    uint32_t slot;
    uint32_t generation;
} vibeos_task_ref_t;

/* Bring the layer up over `slots` entries. Every slot starts FREE at
 * generation 0. */
int vibeos_task_table_init(uint32_t slots);

/* The state of a slot, and its current tenancy. */
vibeos_task_state_t vibeos_task_state(uint32_t slot);
uint32_t vibeos_task_generation(uint32_t slot);

/* Change a slot's state. Returns 0 on success, negative if the transition is
 * not legal - in which case nothing changes and `illegal_transition` is
 * counted. `why` is recorded and printed by the task view: a state change with
 * no author is what made three of these defects take a session each.
 *
 * Allocating a slot (FREE to SETUP) bumps the generation. Releasing one
 * (ZOMBIE to FREE) is the publish point, and is the last thing its caller may
 * do to the slot. */
int vibeos_task_transition(uint32_t slot, vibeos_task_state_t to,
                           const char *why);

/* Take a reference to a slot as it is now. */
vibeos_task_ref_t vibeos_task_ref(uint32_t slot);

/* Is this reference still the task it named? A mismatch is counted, because a
 * number that grows says a caller is holding references across something it
 * should not. */
int vibeos_task_ref_valid(vibeos_task_ref_t ref);

/* Who last changed this slot's state. */
const char *vibeos_task_last_why(uint32_t slot);

/* The name of a state, for the view and for messages. */
const char *vibeos_task_state_name(vibeos_task_state_t state);

/* Is this transition allowed? Exposed for the tests, which check the table
 * rather than the behaviour it produces. */
int vibeos_task_transition_legal(vibeos_task_state_t from,
                                 vibeos_task_state_t to);

#endif /* VIBEOS_TASK_H */
