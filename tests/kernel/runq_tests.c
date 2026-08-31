/* Host tests for the run queue. Phase S-P2 of docs/sched/.
 *
 * The case that justifies the whole phase is the last one: two CPUs must never
 * pick the same slot. That is a defect this kernel has had, and until now it
 * could only be looked for by booting and hoping the interleaving came out
 * wrong.
 */

#include <stdio.h>
#include <string.h>

#include "vibeos/runq.h"

int test_runq(void);

#define SLOTS 8u
#define CPUS  2u

/* A stand-in task table: runnable, and claimed by a CPU or not. */
static int g_ready[SLOTS];
static int g_owner[SLOTS];      /* -1 = nobody, else the cpu running it */

static int rq_runnable(void *ctx, uint32_t slot, uint32_t cpu) {
    (void)ctx;
    (void)cpu;
    return g_ready[slot] && g_owner[slot] < 0;
}

static void reset(void) {
    uint32_t i;
    for (i = 0; i < SLOTS; i++) {
        g_ready[i] = 0;
        g_owner[i] = -1;
    }
}

static int expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:runq %s\n", what);
    }
    return cond;
}

int test_runq(void) {
    vibeos_runq_t q;
    uint32_t i;

    /* ---- nothing runnable falls back to idle ----------------------------- */
    reset();
    if (vibeos_runq_init(&q, SLOTS, CPUS, rq_runnable, 0) != 0) { goto fail; }
    vibeos_runq_set_idle(&q, 0, 7);
    vibeos_runq_set_idle(&q, 1, 6);
    if (!expect(vibeos_runq_pick(&q, 0, -1) == 7, "no idle fallback")) { goto fail; }
    if (!expect(vibeos_runq_pick(&q, 1, -1) == 6, "wrong idle for cpu 1")) { goto fail; }

    /* ---- one runnable task is picked ------------------------------------- */
    g_ready[3] = 1;
    if (!expect(vibeos_runq_pick(&q, 0, -1) == 3, "a runnable slot was not picked")) { goto fail; }

    /* ---- a task somebody is running is not picked again ------------------ *
     *
     * This is the defect the phase exists for, in its simplest form. */
    g_owner[3] = 1;
    if (!expect(vibeos_runq_pick(&q, 0, -1) == 7,
                "a task already on a CPU was picked by another")) { goto fail; }
    g_owner[3] = -1;

    /* ---- keep running what you have when nothing better exists ----------- */
    if (!expect(vibeos_runq_pick(&q, 0, 3) == -1,
                "the only runnable task was preempted for nothing")) { goto fail; }

    /* ---- and give it up when it stops being runnable ---------------------- */
    g_ready[3] = 0;
    if (!expect(vibeos_runq_pick(&q, 0, 3) == 7,
                "a task that stopped being runnable kept the CPU")) { goto fail; }

    /* ---- round-robin is fair --------------------------------------------- *
     *
     * The failure this catches is a scan that restarts from the same place
     * every time: the low slots then get every timeslice and the high ones
     * starve. Over enough picks each runnable slot must appear. */
    reset();
    if (vibeos_runq_init(&q, SLOTS, CPUS, rq_runnable, 0) != 0) { goto fail; }
    vibeos_runq_set_idle(&q, 0, 7);
    for (i = 0; i < 5u; i++) {
        g_ready[i] = 1;
    }
    {
        int seen[SLOTS];
        memset(seen, 0, sizeof(seen));
        for (i = 0; i < 40u; i++) {
            int got = vibeos_runq_pick(&q, 0, -1);
            if (got < 0 || got >= (int)SLOTS) { goto fail; }
            seen[got] = 1;
        }
        for (i = 0; i < 5u; i++) {
            if (!seen[i]) {
                printf("FAIL:runq slot %u never ran in forty picks\n", i);
                goto fail;
            }
        }
    }

    /* ---- two CPUs never pick the same slot -------------------------------
     *
     * The reason this layer is portable. Each CPU picks and then claims, as
     * the scheduler does under its lock; if the queue can hand the same slot
     * to both, two cores run one task - which this kernel has done. */
    reset();
    if (vibeos_runq_init(&q, SLOTS, CPUS, rq_runnable, 0) != 0) { goto fail; }
    vibeos_runq_set_idle(&q, 0, 6);
    vibeos_runq_set_idle(&q, 1, 7);
    for (i = 0; i < 6u; i++) {
        g_ready[i] = 1;
    }
    for (i = 0; i < 30u; i++) {
        int a = vibeos_runq_pick(&q, 0, -1);
        int b;

        if (a >= 0) {
            g_owner[a] = 0;
        }
        b = vibeos_runq_pick(&q, 1, -1);
        if (b >= 0) {
            if (!expect(b != a || a == 6 || a == 7,
                        "two CPUs were given the same task")) { goto fail; }
            g_owner[b] = 1;
        }
        if (a >= 0 && a < (int)SLOTS) { g_owner[a] = -1; }
        if (b >= 0 && b < (int)SLOTS) { g_owner[b] = -1; }
    }

    /* ---- refusals ---------------------------------------------------------- */
    if (!expect(vibeos_runq_pick(&q, CPUS, -1) == -1,
                "a pick for a CPU outside the table was served")) { goto fail; }
    if (!expect(vibeos_runq_init(&q, SLOTS, VIBEOS_RUNQ_MAX_CPUS + 1u,
                                 rq_runnable, 0) != 0,
                "more CPUs than the table holds was accepted")) { goto fail; }

    return 0;

fail:
    return -1;
}
