/* Host tests for the task state machine. Phase S-P1 of docs/sched/.
 *
 * The transitions that are *absent* from the table are the ones that matter,
 * because each of them is a defect this kernel has actually had. So the tests
 * are mostly refusals, and each says which failure it stands for.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/task.h"

int test_task(void);

#define SLOTS 8u

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:task %s\n", what);
    }
    return cond;
}

int test_task(void) {
    vibeos_task_ref_t ref, stale;
    uint32_t gen;

    vibeos_task_stats_reset();
    if (vibeos_task_table_init(SLOTS) != 0) { goto fail; }

    /* ---- everything starts free ------------------------------------------ */
    if (!expect(vibeos_task_state(0) == VIBEOS_TASK_FREE, "slot did not start free")) { goto fail; }
    if (!expect(vibeos_task_generation(0) == 0u, "generation did not start at zero")) { goto fail; }

    /* ---- the ordinary life of a task ------------------------------------- */
    if (vibeos_task_transition(0, VIBEOS_TASK_SETUP, "alloc") != 0) { goto fail; }
    if (!expect(vibeos_task_generation(0) == 1u,
                "claiming a slot did not begin a new tenancy")) { goto fail; }
    if (vibeos_task_transition(0, VIBEOS_TASK_READY, "spawn") != 0) { goto fail; }
    if (vibeos_task_transition(0, VIBEOS_TASK_RUNNING, "sched") != 0) { goto fail; }
    if (vibeos_task_transition(0, VIBEOS_TASK_ZOMBIE, "exit") != 0) { goto fail; }
    if (vibeos_task_transition(0, VIBEOS_TASK_FREE, "reap") != 0) { goto fail; }
    if (!expect(vibeos_task_stats()->illegal_transition == 0ull,
                "a legal sequence was refused")) { goto fail; }

    /* ---- RUNNING to FREE is the wedge ------------------------------------ *
     *
     * A slot published as reusable while a task is still on a CPU: a fork on
     * another core takes it, and the exit that had already announced itself
     * then tears down the new tenant's address space. No panic, no output. */
    if (vibeos_task_transition(1, VIBEOS_TASK_SETUP, "alloc") != 0) { goto fail; }
    if (vibeos_task_transition(1, VIBEOS_TASK_READY, "spawn") != 0) { goto fail; }
    if (vibeos_task_transition(1, VIBEOS_TASK_RUNNING, "sched") != 0) { goto fail; }
    if (!expect(vibeos_task_transition(1, VIBEOS_TASK_FREE, "reap") != 0,
                "a running task could be released")) { goto fail; }
    if (!expect(vibeos_task_state(1) == VIBEOS_TASK_RUNNING,
                "a refused transition changed the state anyway")) { goto fail; }
    if (!expect(vibeos_task_stats()->illegal_transition == 1ull,
                "an illegal transition was not counted")) { goto fail; }

    /* ---- the dead do not come back --------------------------------------- *
     *
     * Allowing ZOMBIE to READY would let a reap race a wake and put a task
     * back on an address space that has been destroyed. */
    if (vibeos_task_transition(1, VIBEOS_TASK_ZOMBIE, "exit") != 0) { goto fail; }
    if (!expect(vibeos_task_transition(1, VIBEOS_TASK_READY, "wake") != 0,
                "a zombie was made runnable")) { goto fail; }

    /* ---- a creator that fails may give the slot back --------------------- *
     *
     * SETUP to FREE is legal on purpose: fork failing half-way has to release
     * what it claimed. What it may not do is publish before releasing, and
     * that is ordering rather than legality - enforced by the caller being one
     * function, not by this table. */
    if (vibeos_task_transition(2, VIBEOS_TASK_SETUP, "alloc") != 0) { goto fail; }
    if (vibeos_task_transition(2, VIBEOS_TASK_FREE, "fork failed") != 0) { goto fail; }

    /* ---- blocked and back ------------------------------------------------ */
    if (vibeos_task_transition(3, VIBEOS_TASK_SETUP, "alloc") != 0) { goto fail; }
    if (vibeos_task_transition(3, VIBEOS_TASK_READY, "spawn") != 0) { goto fail; }
    if (vibeos_task_transition(3, VIBEOS_TASK_BLOCKED, "wait") != 0) { goto fail; }
    if (vibeos_task_transition(3, VIBEOS_TASK_READY, "woken") != 0) { goto fail; }
    if (!expect(vibeos_task_transition(3, VIBEOS_TASK_SETUP, "alloc again") != 0,
                "a live slot could be claimed again")) { goto fail; }

    /* ---- who did it is recorded ------------------------------------------ */
    if (!expect(vibeos_task_last_why(3)[0] == 'w',
                "the author of a transition was not kept")) { goto fail; }

    /* ---- tenancy: a stale reference is caught ---------------------------- *
     *
     * The reason decision T1 chose "everywhere". A bare slot number stays
     * valid forever and names whoever holds the slot now, so a reference kept
     * across a lock release silently becomes a reference to a different task. */
    if (vibeos_task_transition(4, VIBEOS_TASK_SETUP, "alloc") != 0) { goto fail; }
    if (vibeos_task_transition(4, VIBEOS_TASK_READY, "spawn") != 0) { goto fail; }
    ref = vibeos_task_ref(4);
    if (!expect(vibeos_task_ref_valid(ref), "a fresh reference was rejected")) { goto fail; }

    stale = ref;
    if (vibeos_task_transition(4, VIBEOS_TASK_ZOMBIE, "exit") != 0) { goto fail; }
    if (vibeos_task_transition(4, VIBEOS_TASK_FREE, "reap") != 0) { goto fail; }
    if (!expect(!vibeos_task_ref_valid(stale),
                "a reference to a released slot was accepted")) { goto fail; }

    gen = vibeos_task_generation(4);
    if (vibeos_task_transition(4, VIBEOS_TASK_SETUP, "alloc") != 0) { goto fail; }
    if (!expect(vibeos_task_generation(4) == gen + 1u,
                "reusing a slot did not begin a new tenancy")) { goto fail; }
    if (!expect(!vibeos_task_ref_valid(stale),
                "a reference from a previous tenancy named the new task")) { goto fail; }
    if (!expect(vibeos_task_stats()->tenancy_mismatch >= 1ull,
                "a stale reference was not counted")) { goto fail; }

    /* ---- the table itself, rather than the behaviour it produces ---------- */
    if (!expect(!vibeos_task_transition_legal(VIBEOS_TASK_RUNNING, VIBEOS_TASK_FREE),
                "RUNNING to FREE is in the table")) { goto fail; }
    if (!expect(!vibeos_task_transition_legal(VIBEOS_TASK_ZOMBIE, VIBEOS_TASK_READY),
                "ZOMBIE to READY is in the table")) { goto fail; }
    if (!expect(!vibeos_task_transition_legal(VIBEOS_TASK_FREE, VIBEOS_TASK_READY),
                "a free slot can become runnable without being claimed")) { goto fail; }
    if (!expect(vibeos_task_transition_legal(VIBEOS_TASK_SETUP, VIBEOS_TASK_FREE),
                "a failed creator cannot give the slot back")) { goto fail; }

    /* ---- out of range is refused, not counted as a defect ---------------- */
    if (!expect(vibeos_task_transition(SLOTS, VIBEOS_TASK_SETUP, "bad") != 0,
                "a slot outside the table was accepted")) { goto fail; }

    return 0;

fail:
    return -1;
}
