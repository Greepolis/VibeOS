/* The task table's state machine. Phase S-P1 of docs/sched/.
 *
 * The transition table is the design. Everything else in this file exists to
 * make it the only way a slot's state changes.
 */

#include "vibeos/task.h"

#define VIBEOS_TASK_MAX_SLOTS 64u

static uint8_t g_state[VIBEOS_TASK_MAX_SLOTS];
static uint32_t g_generation[VIBEOS_TASK_MAX_SLOTS];
static const char *g_why[VIBEOS_TASK_MAX_SLOTS];
static uint32_t g_slots;

/* Legal transitions, written as a table rather than as nine functions each
 * knowing part of it.
 *
 * The entries that are *absent* are the interesting ones:
 *
 *   RUNNING -> FREE   a task cannot be released while it is on a CPU. This is
 *                     the wedge: a slot published as reusable, taken by a fork
 *                     on another core, and then torn down by the exit that had
 *                     already announced itself.
 *   SETUP   -> FREE   is legal, and deliberately so: a creator that fails
 *                     half-way must be able to give the slot back. What it may
 *                     not do is publish it before releasing what it built,
 *                     which is ordering rather than legality and is enforced by
 *                     the caller being one function.
 *   ZOMBIE  -> READY  a dead task does not come back. Allowing it would let a
 *                     reap race a wake and resurrect a task onto a destroyed
 *                     address space.
 *
 * SETUP -> RUNNING is legal, and was missing from the first draft of this
 * table. The state machine refused it on the first boot, which is the table
 * being wrong rather than the kernel: a core adopts the thread it is already
 * executing into a slot, and an idle task is created already on its CPU.
 * Neither passes through READY because neither was ever waiting.
 */
static const uint8_t g_legal[VIBEOS_TASK_STATE_COUNT][VIBEOS_TASK_STATE_COUNT] = {
    /* from \ to      FREE READY RUN  ZOMB BLOCK SETUP */
    /* FREE    */   {  0,   0,    0,   0,   0,    1 },
    /* READY   */   {  0,   0,    1,   1,   1,    0 },
    /* RUNNING */   {  0,   1,    0,   1,   1,    0 },
    /* ZOMBIE  */   {  1,   0,    0,   0,   0,    0 },
    /* BLOCKED */   {  0,   1,    0,   1,   0,    0 },
    /* SETUP   */   {  1,   1,    1,   0,   0,    0 },
};

int vibeos_task_transition_legal(vibeos_task_state_t from,
                                 vibeos_task_state_t to) {
    if ((uint32_t)from >= (uint32_t)VIBEOS_TASK_STATE_COUNT ||
        (uint32_t)to >= (uint32_t)VIBEOS_TASK_STATE_COUNT) {
        return 0;
    }
    return g_legal[from][to] != 0u;
}

int vibeos_task_table_init(uint32_t slots) {
    uint32_t i;

    if (slots == 0u || slots > VIBEOS_TASK_MAX_SLOTS) {
        return -1;
    }
    g_slots = slots;
    for (i = 0; i < slots; i++) {
        g_state[i] = (uint8_t)VIBEOS_TASK_FREE;
        g_generation[i] = 0;
        g_why[i] = "init";
    }
    return 0;
}

vibeos_task_state_t vibeos_task_state(uint32_t slot) {
    if (slot >= g_slots) {
        return VIBEOS_TASK_FREE;
    }
    return (vibeos_task_state_t)g_state[slot];
}

uint32_t vibeos_task_generation(uint32_t slot) {
    return (slot < g_slots) ? g_generation[slot] : 0u;
}

const char *vibeos_task_last_why(uint32_t slot) {
    if (slot >= g_slots || !g_why[slot]) {
        return "-";
    }
    return g_why[slot];
}

int vibeos_task_transition(uint32_t slot, vibeos_task_state_t to,
                           const char *why) {
    vibeos_task_state_t from;

    if (slot >= g_slots) {
        return -1;
    }
    from = (vibeos_task_state_t)g_state[slot];
    if (!vibeos_task_transition_legal(from, to)) {
        /* Counted, not fatal. A count can be asserted across a whole boot; a
         * panic can only be met once, and the first time it fires is usually
         * while somebody is investigating something else. */
        vibeos_task_stats()->illegal_transition++;
        return -1;
    }

    /* A new tenancy begins when a slot is claimed, not when it is released.
     *
     * Bumping it on release would leave the freed slot carrying the generation
     * its next occupant will have, so a reference taken before the release
     * would validate against the new tenant - which is exactly the failure the
     * generation exists to catch. */
    if (from == VIBEOS_TASK_FREE && to == VIBEOS_TASK_SETUP) {
        g_generation[slot]++;
    }

    g_state[slot] = (uint8_t)to;
    g_why[slot] = why;
    return 0;
}

vibeos_task_ref_t vibeos_task_ref(uint32_t slot) {
    vibeos_task_ref_t r;

    r.slot = slot;
    r.generation = vibeos_task_generation(slot);
    return r;
}

int vibeos_task_ref_valid(vibeos_task_ref_t ref) {
    if (ref.slot >= g_slots) {
        return 0;
    }
    if (g_generation[ref.slot] != ref.generation) {
        vibeos_task_stats()->tenancy_mismatch++;
        return 0;
    }
    /* A reference to a slot that has been released names nothing, even at the
     * right generation: the tenancy has not moved on yet, but the task it
     * described is gone. */
    return g_state[ref.slot] != (uint8_t)VIBEOS_TASK_FREE;
}

const char *vibeos_task_state_name(vibeos_task_state_t state) {
    switch (state) {
        case VIBEOS_TASK_FREE:    return "free";
        case VIBEOS_TASK_READY:   return "ready";
        case VIBEOS_TASK_RUNNING: return "running";
        case VIBEOS_TASK_ZOMBIE:  return "zombie";
        case VIBEOS_TASK_BLOCKED: return "blocked";
        case VIBEOS_TASK_SETUP:   return "setup";
        default:                  return "?";
    }
}
