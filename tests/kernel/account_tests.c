/* Host tests for scheduler accounting. Phase S-P4 of docs/sched/.
 *
 * The layer is arithmetic, so it can be tested exhaustively rather than
 * sampled - and it is worth testing that way, because everything S-P5 decides
 * will be justified by these numbers. A policy tuned against wrong accounting
 * is not a policy, it is a coincidence.
 */

#include <stdio.h>

#include "vibeos/account.h"

int test_account(void);

static int g_fail;

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:account %s\n", what);
        g_fail = 1;
    }
    return cond;
}

#define SLOTS 8u
#define CPUS 4u

int test_account(void) {
    const vibeos_task_account_t *a;
    uint64_t charged, idle, seen;
    uint32_t i;

    g_fail = 0;

    expect(vibeos_account_init(0, CPUS) != 0, "zero slots was accepted");
    expect(vibeos_account_init(SLOTS, 0) != 0, "zero cpus was accepted");
    expect(vibeos_account_init(VIBEOS_ACCOUNT_MAX_SLOTS + 1u, CPUS) != 0,
           "more slots than this layer holds was accepted");
    if (!expect(vibeos_account_init(SLOTS, CPUS) == 0, "init refused a sane size")) {
        return -1;
    }

    /* ---- a tick goes to exactly one place -------------------------------- */
    vibeos_account_tick(0, 1, 0);
    vibeos_account_tick(0, 1, 0);
    vibeos_account_tick(1, 2, 0);
    if (vibeos_account_balance(&charged, &idle, &seen) != 0) {
        expect(0, "three ticks charged to tasks did not balance");
    }
    expect(charged == 3ull && idle == 0ull && seen == 3ull, "the split is wrong");
    a = vibeos_account_task(1);
    expect(a && a->ticks == 2ull, "task 1 was not charged twice");
    a = vibeos_account_task(2);
    expect(a && a->ticks == 1ull, "task 2 was not charged once");

    /* ---- an idle task is a task, and must not read as work ---------------- *
     *
     * Every core here has an idle task with a real slot. Charging by slot alone
     * would report a machine doing nothing as fully busy, and every ratio built
     * on that would be wrong in the same direction - the direction that hides a
     * problem. */
    vibeos_account_tick(2, 5, 1);       /* slot 5 is that core's idle task */
    vibeos_account_tick(3, -1, 0);      /* no current task at all          */
    (void)vibeos_account_balance(&charged, &idle, &seen);
    expect(charged == 3ull, "an idle task's tick was charged as work");
    expect(idle == 2ull, "idle ticks were not counted");
    expect(seen == 5ull, "a tick went missing");
    a = vibeos_account_task(5);
    expect(a && a->ticks == 0ull, "the idle task accumulated work");
    expect(vibeos_account_idle(2) == 1ull, "cpu 2's idle time is wrong");
    expect(vibeos_account_idle(3) == 1ull, "cpu 3's idle time is wrong");
    expect(vibeos_account_idle(0) == 0ull, "a busy cpu was credited with idle time");

    /* ---- the identity holds over a long mixed run ------------------------- *
     *
     * Not a tolerance. Charged plus idle equals seen, exactly, or a tick went
     * somewhere nobody can name - and every number above this layer inherits
     * that error. */
    for (i = 0; i < 10000u; i++) {
        uint32_t cpu = i % CPUS;
        int slot = (int)(i % SLOTS);
        vibeos_account_tick(cpu, slot, (i % 7u) == 0u);
    }
    if (vibeos_account_balance(&charged, &idle, &seen) != 0) {
        expect(0, "ten thousand ticks did not balance");
    }
    expect(charged + idle == seen, "charged plus idle is not seen");
    expect(seen == 10005ull, "ticks were lost or invented");

    /* An out-of-range slot is charged to idle rather than dropped: a tick that
     * vanishes breaks the identity, which is the one thing this layer promises.
     * An out-of-range cpu is refused outright, because there is no idle counter
     * to put it in and inventing one would be worse than losing the tick. */
    vibeos_account_tick(0, (int)SLOTS + 3, 0);
    (void)vibeos_account_balance(&charged, &idle, &seen);
    expect(seen == 10006ull, "a tick for an impossible slot was dropped");
    expect(idle > 0ull, "an impossible slot was charged as work");

    {
        uint64_t before = seen;
        vibeos_account_tick(CPUS + 1u, 1, 0);
        (void)vibeos_account_balance(&charged, &idle, &seen);
        expect(seen == before, "a tick on a cpu that does not exist was counted");
    }

    /* ---- switches and waiting -------------------------------------------- */
    if (!expect(vibeos_account_init(SLOTS, CPUS) == 0, "re-init failed")) {
        return -1;
    }
    for (i = 0; i < 100u; i++) {
        vibeos_account_tick(0, 1, 0);
    }
    vibeos_account_switch(0, 1, 40ull);     /* ready at 40, running at 100 */
    a = vibeos_account_task(1);
    expect(a && a->switches == 1ull, "the switch was not counted");
    expect(a && a->last_ran == 100ull, "last_ran is not the tick it started");
    expect(a && a->max_wait == 60ull, "the wait was mis-measured");

    /* A shorter wait must not replace a longer one: the number is the worst
     * case, which is the only version of it a starvation check can use. */
    for (i = 0; i < 10u; i++) {
        vibeos_account_tick(0, 1, 0);
    }
    vibeos_account_switch(0, 1, 105ull);    /* waited 5 */
    a = vibeos_account_task(1);
    expect(a && a->max_wait == 60ull, "a shorter wait overwrote the longest one");

    /* An unknown ready time records no wait rather than a wait of everything
     * since boot - which would look exactly like starvation. */
    vibeos_account_switch(0, 2, 0ull);
    a = vibeos_account_task(2);
    expect(a && a->max_wait == 0ull, "an unknown ready time was read as a long wait");
    expect(a && a->switches == 1ull, "the switch was not counted");

    /* A ready time in the future - which a caller can produce by racing the
     * tick counter - records nothing rather than a huge number by underflow. */
    vibeos_account_switch(0, 3, 1000000ull);
    a = vibeos_account_task(3);
    expect(a && a->max_wait == 0ull, "a ready time in the future underflowed");

    expect(vibeos_account_task(SLOTS) == 0, "a slot past the end was described");

    return g_fail ? -1 : 0;
}
