/* Host tests for the scheduling policy. Phase S-P5 of docs/sched/.
 *
 * The plan asks for weights that are *measured* rather than asserted by
 * inspection, and that is what most of this file does: it runs the picker for
 * thousands of ticks and counts who got what. A policy checked by reading it is
 * a policy nobody can change safely.
 */

#include <stdio.h>

#include "vibeos/sched_policy.h"

int test_sched_policy(void);

static int g_fail;

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:policy %s\n", what);
        g_fail = 1;
    }
    return cond;
}

#define SLOTS 8u

/* Run the picker for `ticks`, charging one tick to whoever it picks, and count
 * how often each slot ran. This is the whole scheduler loop in four lines,
 * which is the point of the layer being pure. */
static void run(uint64_t runnable, uint32_t ticks, uint32_t cpu,
                uint32_t out_runs[SLOTS]) {
    uint32_t t, i;

    for (i = 0; i < SLOTS; i++) {
        out_runs[i] = 0;
    }
    for (t = 0; t < ticks; t++) {
        int who = vibeos_sched_policy_pick(cpu, runnable);
        if (who < 0) {
            continue;
        }
        out_runs[who]++;
        vibeos_sched_policy_charge((uint32_t)who, 1);
    }
}

int test_sched_policy(void) {
    uint32_t runs[SLOTS];

    g_fail = 0;

    expect(vibeos_sched_policy_init(0) != 0, "zero slots accepted");
    expect(vibeos_sched_policy_init(VIBEOS_SCHED_MAX_SLOTS + 1u) != 0, "too many slots accepted");
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "init refused a sane size")) {
        return -1;
    }

    /* ---- the quantum is a number somebody chose ------------------------- */
    expect(vibeos_sched_policy_quantum(VIBEOS_SCHED_KERNEL) <
           vibeos_sched_policy_quantum(VIBEOS_SCHED_NORMAL),
           "a kernel task does not get a shorter slice than a normal one");
    expect(vibeos_sched_policy_quantum(VIBEOS_SCHED_NORMAL) > 0u, "the normal quantum is zero");

    /* ---- nothing runnable, nothing admitted ----------------------------- */
    expect(vibeos_sched_policy_pick(0, 0ull) < 0, "something was picked from an empty set");
    expect(vibeos_sched_policy_pick(0, 0xFFull) < 0,
           "a slot the policy was never told about was scheduled");

    /* ---- class beats everything ----------------------------------------- *
     *
     * Absolutely, not by a large weight: a runnable kernel task runs before any
     * normal one however long the normal one has waited. That is what makes it
     * a class, and it is the property the kernel's own work depends on. */
    if (!expect(vibeos_sched_policy_admit(0, VIBEOS_SCHED_NORMAL, 0, 0) == 0, "admit 0")) { return -1; }
    if (!expect(vibeos_sched_policy_admit(1, VIBEOS_SCHED_KERNEL, 19, 0) == 0, "admit 1")) { return -1; }
    run(0x3ull, 500, 0, runs);
    expect(runs[1] == 500u, "a kernel task did not take every tick");
    expect(runs[0] == 0u, "a normal task ran while a kernel task was runnable");

    /* And the idle class is genuinely last. */
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "re-init")) { return -1; }
    (void)vibeos_sched_policy_admit(0, VIBEOS_SCHED_IDLE, -20, 0);
    (void)vibeos_sched_policy_admit(1, VIBEOS_SCHED_NORMAL, 19, 0);
    run(0x3ull, 300, 0, runs);
    expect(runs[0] == 0u, "an idle task ran while normal work was runnable");
    expect(runs[1] == 300u, "the normal task did not run");
    /* Alone, it runs: idle is a class, not a refusal. */
    run(0x1ull, 10, 0, runs);
    expect(runs[0] == 10u, "an idle task did not run when it was the only one");

    /* ---- equal tasks share equally --------------------------------------- */
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "re-init")) { return -1; }
    (void)vibeos_sched_policy_admit(0, VIBEOS_SCHED_NORMAL, 0, 0);
    (void)vibeos_sched_policy_admit(1, VIBEOS_SCHED_NORMAL, 0, 0);
    (void)vibeos_sched_policy_admit(2, VIBEOS_SCHED_NORMAL, 0, 0);
    run(0x7ull, 3000, 0, runs);
    {
        uint32_t lo = runs[0], hi = runs[0], i;
        for (i = 1; i < 3u; i++) {
            if (runs[i] < lo) { lo = runs[i]; }
            if (runs[i] > hi) { hi = runs[i]; }
        }
        expect(hi - lo <= 1u, "three equal tasks did not share within one tick");
    }

    /* ---- weights produce the ratio they promise -------------------------- *
     *
     * Measured, not asserted by inspection. nice 0 against nice 5 is a weight
     * ratio of 102:33, near enough three to one, and what is checked is that
     * the observed ratio lands in a band around it rather than an exact figure
     * - the picker deals in whole ticks. */
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "re-init")) { return -1; }
    (void)vibeos_sched_policy_admit(0, VIBEOS_SCHED_NORMAL, 0, 0);
    (void)vibeos_sched_policy_admit(1, VIBEOS_SCHED_NORMAL, 5, 0);
    run(0x3ull, 40000, 0, runs);
    if (expect(runs[1] > 0u, "the less favoured task never ran")) {
        uint32_t ratio_x100 = (runs[0] * 100u) / runs[1];
        /* 102/33 = 3.09. Anything between 2.7 and 3.5 is the weight working;
         * outside that band it is not. */
        expect(ratio_x100 > 270u && ratio_x100 < 350u,
               "nice 0 against nice 5 did not produce roughly a 3:1 share");
    }

    /* The favourable direction, too: nice -5 against nice 0 is 312:102. */
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "re-init")) { return -1; }
    (void)vibeos_sched_policy_admit(0, VIBEOS_SCHED_NORMAL, -5, 0);
    (void)vibeos_sched_policy_admit(1, VIBEOS_SCHED_NORMAL, 0, 0);
    run(0x3ull, 40000, 0, runs);
    if (expect(runs[1] > 0u, "the nice-0 task never ran against a favoured one")) {
        uint32_t ratio_x100 = (runs[0] * 100u) / runs[1];
        expect(ratio_x100 > 260u && ratio_x100 < 350u,
               "nice -5 against nice 0 did not produce roughly a 3:1 share");
    }

    /* ---- nobody starves --------------------------------------------------- *
     *
     * The strong version: even at the extremes of the range, the least
     * favoured task must run. A weighted queue promises nothing about equality,
     * only that nobody is left behind indefinitely, so this is the property
     * worth asserting and the ratio above is the one worth measuring. */
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "re-init")) { return -1; }
    (void)vibeos_sched_policy_admit(0, VIBEOS_SCHED_NORMAL, -20, 0);
    (void)vibeos_sched_policy_admit(1, VIBEOS_SCHED_NORMAL, 19, 0);
    run(0x3ull, 100000, 0, runs);
    expect(runs[1] > 0u, "the most-niced task starved against the least");
    expect(vibeos_sched_policy_max_lag(0x3ull) < 100000ull,
           "virtual time diverged without bound");

    /* ---- a new task starts level, not at zero ---------------------------- *
     *
     * Admitting at zero would hand every newly created task the whole machine
     * until it caught up - not a subtle unfairness but a fork bomb that needs
     * no malice. */
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "re-init")) { return -1; }
    (void)vibeos_sched_policy_admit(0, VIBEOS_SCHED_NORMAL, 0, 0);
    run(0x1ull, 5000, 0, runs);
    (void)vibeos_sched_policy_admit(1, VIBEOS_SCHED_NORMAL, 0, 0);
    expect(vibeos_sched_policy_vruntime(1) == vibeos_sched_policy_vruntime(0),
           "a new task was admitted behind the others rather than level");
    run(0x3ull, 1000, 0, runs);
    expect(runs[0] > 400u && runs[1] > 400u,
           "a newly admitted task took the machine from the incumbent");

    /* ---- renicing does not reset history --------------------------------- */
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "re-init")) { return -1; }
    (void)vibeos_sched_policy_admit(0, VIBEOS_SCHED_NORMAL, 0, 0);
    vibeos_sched_policy_charge(0, 1000);
    {
        uint64_t before = vibeos_sched_policy_vruntime(0);
        expect(vibeos_sched_policy_set_nice(0, -20) == 0, "renice refused");
        expect(vibeos_sched_policy_vruntime(0) == before,
               "renicing reset the task's history, which is the machine for free");
        expect(vibeos_sched_policy_nice(0) == -20, "the new nice was not recorded");
    }
    expect(vibeos_sched_policy_set_nice(0, 20) != 0, "a nice past the maximum was accepted");
    expect(vibeos_sched_policy_set_nice(0, -21) != 0, "a nice past the minimum was accepted");

    /* ---- affinity: a pinned task never appears elsewhere ------------------ */
    if (!expect(vibeos_sched_policy_init(SLOTS) == 0, "re-init")) { return -1; }
    (void)vibeos_sched_policy_admit(0, VIBEOS_SCHED_NORMAL, 0, 1u << 2);  /* cpu 2 only */
    (void)vibeos_sched_policy_admit(1, VIBEOS_SCHED_NORMAL, 0, 0);        /* anywhere   */
    {
        uint32_t cpu;
        for (cpu = 0; cpu < 4u; cpu++) {
            run(0x3ull, 200, cpu, runs);
            if (cpu != 2u) {
                expect(runs[0] == 0u, "a pinned task ran on a core it was not pinned to");
            }
        }
        run(0x1ull, 5, 1, runs);
        expect(runs[0] == 0u, "a pinned task ran on the wrong core when alone");
        expect(vibeos_sched_policy_pick(1, 0x1ull) < 0,
               "the picker returned a task that may not run here");
        expect(vibeos_sched_policy_pick(2, 0x1ull) == 0,
               "the picker refused a task on the core it is pinned to");
    }

    /* ---- forgetting a slot removes it ------------------------------------ */
    vibeos_sched_policy_forget(0);
    expect(vibeos_sched_policy_pick(2, 0x1ull) < 0, "a forgotten slot was still scheduled");
    expect(vibeos_sched_policy_set_nice(0, 0) != 0, "a forgotten slot accepted a renice");

    return g_fail ? -1 : 0;
}
