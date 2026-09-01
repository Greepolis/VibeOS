/* Host tests for the lifetime decisions. Phase S-P3 of docs/sched/.
 *
 * Three things that have each cost this project a session: a wait status read
 * as an exit code, a reap acting on a slot that had moved on, and a teardown
 * that published a slot before it was finished with it.
 */

#include <stdio.h>

#include "vibeos/lifetime.h"

int test_lifetime(void);

#define SLOTS 8u

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:lifetime %s\n", what);
    }
    return cond;
}

int test_lifetime(void) {
    vibeos_task_ref_t child, stale;
    int status;

    vibeos_task_stats_reset();
    if (vibeos_task_table_init(SLOTS) != 0) { goto fail; }

    /* ---- a wait status is not an exit code ------------------------------- *
     *
     * An init that read only the code byte reported a segfault as a clean
     * stop, and the crashing service came back STOPPED. */
    status = vibeos_wait_status_make(7u, 0u);
    if (!expect(vibeos_wait_status_exited(status), "a clean exit read as a signal death")) { goto fail; }
    if (!expect(vibeos_wait_status_code(status) == 7u, "the exit code was lost")) { goto fail; }
    if (!expect(vibeos_wait_status_signal(status) == 0u, "a clean exit carried a signal")) { goto fail; }

    status = vibeos_wait_status_make(0u, 11u);
    if (!expect(!vibeos_wait_status_exited(status), "a signal death read as a clean exit")) { goto fail; }
    if (!expect(vibeos_wait_status_signal(status) == 11u, "the signal was lost")) { goto fail; }
    if (!expect(vibeos_wait_status_code(status) == 0u,
                "a signal death carried an exit code")) { goto fail; }

    /* A task that was killed carries the signal even if an exit code was also
     * recorded: the death is what happened, the code is what it never reached. */
    status = vibeos_wait_status_make(3u, 9u);
    if (!expect(vibeos_wait_status_signal(status) == 9u,
                "an exit code hid the signal that killed the task")) { goto fail; }

    /* 128 + sig is what a shell prints, not what the kernel stores. */
    if (!expect(vibeos_wait_status_make(0u, 11u) != 139,
                "the shell's 128+sig convention leaked into the kernel")) { goto fail; }

    /* ---- reaping --------------------------------------------------------- */
    if (vibeos_task_transition(1, VIBEOS_TASK_SETUP, "fork") != 0) { goto fail; }
    if (vibeos_task_transition(1, VIBEOS_TASK_READY, "spawn") != 0) { goto fail; }
    child = vibeos_task_ref(1);

    /* Alive: the caller has to wait. */
    if (!expect(vibeos_reap_check(child, 20u, 10u, 10u, 0u) == VIBEOS_REAP_NOT_DEAD,
                "a living child was reaped")) { goto fail; }

    /* Somebody else's child. */
    if (!expect(vibeos_reap_check(child, 20u, 99u, 10u, 0u) == VIBEOS_REAP_NOT_A_CHILD,
                "a task belonging to another parent was reaped")) { goto fail; }

    /* The right parent, the wrong child. */
    if (vibeos_task_transition(1, VIBEOS_TASK_ZOMBIE, "exit") != 0) { goto fail; }
    if (!expect(vibeos_reap_check(child, 20u, 10u, 10u, 21u) == VIBEOS_REAP_NOT_A_CHILD,
                "waitpid returned a process it was not asked for")) { goto fail; }
    if (!expect(vibeos_reap_check(child, 20u, 10u, 10u, 20u) == VIBEOS_REAP_OK,
                "waitpid refused the process it was asked for")) { goto fail; }
    if (!expect(vibeos_reap_check(child, 20u, 10u, 10u, 0u) == VIBEOS_REAP_OK,
                "wait for any child refused a zombie")) { goto fail; }

    /* ---- a stale reference names nothing --------------------------------- *
     *
     * The case that needs the tenancy. A wait4 that drops the scheduler lock
     * and comes back with a bare slot index is asking about whoever holds the
     * slot now. */
    stale = child;
    if (vibeos_task_transition(1, VIBEOS_TASK_FREE, "reaped") != 0) { goto fail; }
    if (vibeos_task_transition(1, VIBEOS_TASK_SETUP, "another fork") != 0) { goto fail; }
    /* Through READY: a task that fails during setup goes back to FREE, it does
     * not die as a zombie. The transition table refused the shortcut when this
     * test first took it, which is the table catching the test rather than the
     * other way round. */
    if (vibeos_task_transition(1, VIBEOS_TASK_READY, "spawn") != 0) { goto fail; }
    if (vibeos_task_transition(1, VIBEOS_TASK_ZOMBIE, "exit") != 0) { goto fail; }
    if (!expect(vibeos_reap_check(stale, 20u, 10u, 10u, 0u) == VIBEOS_REAP_GONE,
                "a reference from a previous tenancy reaped the new task")) { goto fail; }

    /* ---- an idempotent re-block ------------------------------------------ *
     *
     * A blocking syscall halts and re-checks, and `hlt` returns on any
     * interrupt - so a task can come back round its loop with the condition
     * still false and block again from BLOCKED. One boot went red on that.
     *
     * The rest of the diagonal stays refused, which is the point of testing
     * both halves: RUNNING -> RUNNING would hide two cores on one task. */
    if (vibeos_task_transition(1, VIBEOS_TASK_FREE, "reaped again") != 0) { goto fail; }
    if (vibeos_task_transition(1, VIBEOS_TASK_SETUP, "fork") != 0) { goto fail; }
    if (vibeos_task_transition(1, VIBEOS_TASK_READY, "spawn") != 0) { goto fail; }
    if (vibeos_task_transition(1, VIBEOS_TASK_BLOCKED, "waitpid") != 0) { goto fail; }
    if (!expect(vibeos_task_transition(1, VIBEOS_TASK_BLOCKED, "waitpid again") == 0,
                "a task could not re-assert the condition it was already waiting on")) { goto fail; }
    if (!expect(vibeos_task_transition_legal(VIBEOS_TASK_RUNNING, VIBEOS_TASK_RUNNING) == 0,
                "running to running was allowed: two cores on one task would pass")) { goto fail; }
    if (!expect(vibeos_task_transition_legal(VIBEOS_TASK_READY, VIBEOS_TASK_READY) == 0,
                "ready to ready was allowed")) { goto fail; }

    /* ---- teardown order --------------------------------------------------- */
    vibeos_task_stats_reset();
    vibeos_teardown_reset(2);
    if (!expect(vibeos_teardown_step(2, VIBEOS_TEARDOWN_ASPACE) == 0, "aspace step refused")) { goto fail; }
    if (!expect(vibeos_teardown_step(2, VIBEOS_TEARDOWN_HARVESTED) == 0, "harvest step refused")) { goto fail; }
    if (!expect(vibeos_teardown_step(2, VIBEOS_TEARDOWN_PUBLISHED) == 0, "publish step refused")) { goto fail; }
    if (!expect(vibeos_task_stats()->use_after_publish == 0ull,
                "a correct teardown was counted as a violation")) { goto fail; }

    /* Publishing before the address space is gone is the silent wedge: a
     * parent reaps the slot, a fork on another core takes it, and the late
     * teardown frees the new tenant's page tables. */
    vibeos_teardown_reset(3);
    if (!expect(vibeos_teardown_step(3, VIBEOS_TEARDOWN_PUBLISHED) != 0,
                "a slot was published before anything was released")) { goto fail; }
    if (!expect(vibeos_task_stats()->use_after_publish == 1ull,
                "an out-of-order teardown was not counted")) { goto fail; }

    /* Publishing before harvesting is what let a reaper free a kernel stack a
     * fork had just allocated into the same slot. */
    vibeos_teardown_reset(4);
    if (vibeos_teardown_step(4, VIBEOS_TEARDOWN_ASPACE) != 0) { goto fail; }
    if (!expect(vibeos_teardown_step(4, VIBEOS_TEARDOWN_PUBLISHED) != 0,
                "a slot was published before it was harvested")) { goto fail; }

    /* Going backwards is a violation too: something touched the slot after it
     * had been published. */
    vibeos_teardown_reset(5);
    if (vibeos_teardown_step(5, VIBEOS_TEARDOWN_ASPACE) != 0) { goto fail; }
    if (vibeos_teardown_step(5, VIBEOS_TEARDOWN_HARVESTED) != 0) { goto fail; }
    if (vibeos_teardown_step(5, VIBEOS_TEARDOWN_PUBLISHED) != 0) { goto fail; }
    if (!expect(vibeos_teardown_step(5, VIBEOS_TEARDOWN_HARVESTED) != 0,
                "a published slot was harvested again")) { goto fail; }

    /* Repeating a step is allowed: a path that releases an address space it
     * turns out to share still passes through the same point, and refusing
     * that would make the check something callers work around. */
    vibeos_teardown_reset(6);
    if (vibeos_teardown_step(6, VIBEOS_TEARDOWN_ASPACE) != 0) { goto fail; }
    if (!expect(vibeos_teardown_step(6, VIBEOS_TEARDOWN_ASPACE) == 0,
                "repeating a step was refused")) { goto fail; }

    return 0;

fail:
    return -1;
}
