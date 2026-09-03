/* Host tests for the fork guard. Phase S-P6 of docs/sched/.
 *
 * The property under test is not "fork eventually fails" - it already did. It
 * is that when a program takes everything it is allowed to, the machine is
 * still administrable: init can start a service, the shell can run a command.
 * A machine that is alive and cannot be acted on is worse than one that
 * refuses.
 */

#include <stdio.h>

#include "vibeos/forkguard.h"

int test_forkguard(void);

static int g_fail;

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:forkguard %s\n", what);
        g_fail = 1;
    }
    return cond;
}

int test_forkguard(void) {
    g_fail = 0;

    /* ---- refusing to be configured into uselessness ---------------------- */
    expect(vibeos_forkguard_init(0, 0, 4) != 0, "a table of zero slots was accepted");
    expect(vibeos_forkguard_init(24, 24, 4) != 0,
           "a reserve as large as the table was accepted - that refuses every fork");
    expect(vibeos_forkguard_init(24, 30, 4) != 0, "a reserve larger than the table was accepted");
    expect(vibeos_forkguard_init(24, 4, 0) != 0, "a child limit of zero was accepted");

    if (!expect(vibeos_forkguard_init(24, 4, 8) == 0, "a sane configuration was refused")) {
        return -1;
    }

    /* ---- the ordinary case ------------------------------------------------ */
    expect(vibeos_forkguard_check(5, 0, 0) == VIBEOS_FORK_OK,
           "an ordinary fork on an empty machine was refused");

    /* ---- the ceiling: one task cannot take everything -------------------- */
    expect(vibeos_forkguard_check(5, 7, 0) == VIBEOS_FORK_OK,
           "a task below its child limit was refused");
    expect(vibeos_forkguard_check(5, 8, 0) == VIBEOS_FORK_TOO_MANY_KIDS,
           "a task at its child limit was allowed to fork again");
    expect(vibeos_forkguard_check(5, 800, 0) == VIBEOS_FORK_TOO_MANY_KIDS,
           "a task far past its limit was allowed");

    /* ---- the floor: the last slots stay for the system ------------------- *
     *
     * 24 slots with 4 reserved: an unprivileged task may take the machine to
     * 19 in use and no further. */
    expect(vibeos_forkguard_check(19, 0, 0) == VIBEOS_FORK_OK,
           "an unprivileged fork was refused while the reserve was intact");
    expect(vibeos_forkguard_check(20, 0, 0) == VIBEOS_FORK_RESERVED,
           "an unprivileged fork ate into the system reserve");
    expect(vibeos_forkguard_check(23, 0, 0) == VIBEOS_FORK_RESERVED,
           "an unprivileged fork took the last slot");

    /* And the reserve is *for* somebody: this is the whole point. With the
     * table nearly full, the tasks that keep the machine administrable can
     * still act. */
    expect(vibeos_forkguard_check(20, 0, 1) == VIBEOS_FORK_OK,
           "a privileged task could not use the reserve it exists for");
    expect(vibeos_forkguard_check(23, 100, 1) == VIBEOS_FORK_OK,
           "a privileged task was held to the unprivileged child limit");

    /* Full is full, for everybody. There is no slot to hand out. */
    expect(vibeos_forkguard_check(24, 0, 1) == VIBEOS_FORK_NO_SLOTS,
           "a fork was allowed with no free slot at all");
    expect(vibeos_forkguard_check(99, 0, 1) == VIBEOS_FORK_NO_SLOTS,
           "a fork was allowed past the end of the table");

    /* ---- the reason is the reason, not just a refusal -------------------- *
     *
     * A task at its child limit on an empty machine must be told *that*. Both
     * produce EAGAIN and they are completely different investigations: one is a
     * program misbehaving, the other is a machine that is full. */
    expect(vibeos_forkguard_check(1, 8, 0) == VIBEOS_FORK_TOO_MANY_KIDS,
           "a child-limit refusal was reported as the machine being full");

    /* ---- the counters separate them too ---------------------------------- */
    {
        const vibeos_forkguard_stats_t *s;
        if (!expect(vibeos_forkguard_init(24, 4, 8) == 0, "re-init")) { return -1; }
        (void)vibeos_forkguard_check(1, 0, 0);    /* ok               */
        (void)vibeos_forkguard_check(1, 9, 0);    /* children         */
        (void)vibeos_forkguard_check(21, 0, 0);   /* reserve          */
        (void)vibeos_forkguard_check(24, 0, 0);   /* nothing at all   */
        s = vibeos_forkguard_stats();
        expect(s->allowed == 1ull, "the allowed count is wrong");
        expect(s->refused_children == 1ull, "the child-limit count is wrong");
        expect(s->refused_reserved == 1ull, "the reserve count is wrong");
        expect(s->refused_no_slots == 1ull, "the no-slots count is wrong");
    }

    /* ---- unconfigured means unchanged ------------------------------------- *
     *
     * A guard that has not been set up must not quietly become a policy of its
     * own. Until somebody configures it, the machine behaves as it did before
     * this file existed. */
    {
        vibeos_forkguard_stats_t saved = *vibeos_forkguard_stats();
        (void)saved;
        /* init(0,...) fails and leaves the previous configuration, so this is
         * checked by the name rather than by re-running init - the important
         * half is that a failed init did not disable the guard. */
        expect(vibeos_forkguard_init(0, 0, 4) != 0, "a bad init was accepted");
        expect(vibeos_forkguard_check(21, 0, 0) == VIBEOS_FORK_RESERVED,
               "a failed init silently disabled the guard");
    }

    expect(vibeos_fork_verdict_name(VIBEOS_FORK_RESERVED)[0] != '?',
           "a verdict has no name, so a log line cannot say which it was");

    return g_fail ? -1 : 0;
}
