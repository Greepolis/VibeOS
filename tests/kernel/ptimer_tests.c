/* Host tests for the process timers (kernel/sched/ptimer.c, docs/abi/ L2 step 3).
 *
 * The table is driven directly: a clock that is just a number, owners that are
 * just numbers, and a fire callback that writes down what it was told. The
 * Linux calls on top of it - alarm, setitimer, timer_* - are tested with the
 * other handlers (linux_abi_tests.c). */

#include <stdio.h>
#include <string.h>

#include "vibeos/ptimer.h"
#include "vibeos/mbz.h"

int test_ptimer(void);

static int g_fail;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:ptimer %s\n", what);
        g_fail = 1;
    }
}

static void t_lock(void) {}
static void t_unlock(void) {}

static vibeos_ptimer_fire_t g_fires[64];
static uint32_t g_nfires;
static int g_answer_pending;   /* what the next fires answer: 0 raised, 1 + n pending with n overruns */

static int record(const vibeos_ptimer_fire_t *f) {
    if (g_nfires < 64u) {
        g_fires[g_nfires] = *f;
    }
    g_nfires++;
    return g_answer_pending;
}

static void clear(void) {
    g_nfires = 0;
    g_answer_pending = 0;
}

int test_ptimer(void) {
    uint64_t left, iv, t;
    int32_t a, b, c;
    uint32_t i, used;

    g_fail = 0;
    vibeos_ptimer_set_lock(t_lock, t_unlock);
    vibeos_ptimer_reset();
    clear();

    /* Ids: the smallest a process is not using, counted per process. */
    expect(vibeos_ptimer_create(10, VIBEOS_PCLOCK_REAL, VIBEOS_PTIMER_SIGNAL, 0, 0, 14, 77, &a) == 0 && a == 0 &&
           vibeos_ptimer_create(10, VIBEOS_PCLOCK_REAL, VIBEOS_PTIMER_SIGNAL, 0, 0, 14, VIBEOS_PTIMER_VALUE_IS_ID, &b) == 0 && b == 1 &&
           vibeos_ptimer_create(20, VIBEOS_PCLOCK_REAL, VIBEOS_PTIMER_SIGNAL, 0, 0, 10, 0, &c) == 0 && c == 0,
           "ids count from zero in each process");
    expect(vibeos_ptimer_delete(10, 0) == 0 &&
           vibeos_ptimer_create(10, VIBEOS_PCLOCK_REAL, VIBEOS_PTIMER_SIGNAL, 0, 0, 14, 5, &a) == 0 && a == 0,
           "a deleted id is the next one handed out");
    expect(vibeos_ptimer_delete(10, 7) == -1 && vibeos_ptimer_get(10, 7, 0, &left, &iv) == -1 &&
           vibeos_ptimer_clock(10, 7) == -1 && vibeos_ptimer_overrun(10, 7) == -1,
           "a timer that does not exist is refused by every call");

    /* A one-shot real timer: never early, exactly once. */
    expect(vibeos_ptimer_set(10, 1, 5, 0, 0, 100, &left, &iv) == 0 && left == 0u && iv == 0u,
           "armed, and it was disarmed before");
    expect(vibeos_ptimer_get(10, 1, 100, &left, &iv) == 0 && left == 5u, "it reads back as the time asked for");
    vibeos_ptimer_tick(105, record);
    expect(g_nfires == 0u, "it does not fire with its time not wholly gone");
    vibeos_ptimer_tick(106, record);
    expect(g_nfires == 1u && g_fires[0].tgid == 10u && g_fires[0].id == 1 && g_fires[0].signo == 14u &&
           g_fires[0].value == 1u && !g_fires[0].to_thread,
           "it fires once, saying whose and which - and VALUE_IS_ID became its id");
    vibeos_ptimer_tick(200, record);
    expect(g_nfires == 1u && vibeos_ptimer_get(10, 1, 200, &left, &iv) == 0 && left == 0u,
           "and is disarmed afterwards");

    /* Periodic, with ticks that go by unseen counted as overruns. */
    clear();
    (void)vibeos_ptimer_set(10, 1, 3, 4, 0, 0, &left, &iv);   /* fires at 4, 8, 12, ... */
    vibeos_ptimer_tick(4, record);
    vibeos_ptimer_tick(8, record);
    vibeos_ptimer_tick(20, record);   /* 12 and 16 went by */
    expect(g_nfires == 3u && g_fires[2].overrun == 2u, "periods with no tick to fire them are overruns");
    expect(vibeos_ptimer_overrun(10, 1) == 2, "and timer_getoverrun says so after the delivery");
    expect(vibeos_ptimer_get(10, 1, 20, &left, &iv) == 0 && iv == 4u && left == 3u,
           "it keeps its period: next at 24");

    /* A signal still pending is an overrun, not a second signal: the count is
     * the pending signal's, and timer_getoverrun says what it says. */
    clear();
    g_answer_pending = 2;
    vibeos_ptimer_tick(24, record);
    g_answer_pending = 3;
    vibeos_ptimer_tick(28, record);
    expect(g_nfires == 2u && vibeos_ptimer_overrun(10, 1) == 2 && vibeos_ptimer_overruns() >= 2u,
           "expiries that found the signal pending are its overrun count");
    g_answer_pending = 0;
    vibeos_ptimer_tick(32, record);
    expect(g_nfires == 3u && g_fires[2].overrun == 0u && vibeos_ptimer_overrun(10, 1) == 0,
           "and a signal raised afresh starts from none");

    /* Absolute time, and a time already past. */
    clear();
    (void)vibeos_ptimer_set(10, 1, 50, 0, 1, 10, &left, &iv);
    vibeos_ptimer_tick(49, record);
    vibeos_ptimer_tick(50, record);
    expect(g_nfires == 1u, "an absolute time fires when the clock reads it");
    (void)vibeos_ptimer_set(10, 1, 5, 0, 1, 60, &left, &iv);
    vibeos_ptimer_tick(61, record);
    expect(g_nfires == 2u, "an absolute time already past fires on the next tick");

    /* Disarming reports what was left. */
    (void)vibeos_ptimer_set(10, 1, 30, 7, 0, 100, &left, &iv);
    expect(vibeos_ptimer_set(10, 1, 0, 0, 0, 110, &left, &iv) == 0 && left == 20u && iv == 7u,
           "disarming says what was left, and the period");

    /* The interval timers: made on first use, and only then. */
    used = vibeos_ptimer_used();
    expect(vibeos_ptimer_get(30, VIBEOS_PTIMER_ITIMER(0), 0, &left, &iv) == 0 && left == 0u &&
           vibeos_ptimer_set(30, VIBEOS_PTIMER_ITIMER(0), 0, 0, 0, 0, &left, &iv) == 0 &&
           vibeos_ptimer_used() == used,
           "an interval timer never armed reads as disarmed and takes no entry to disarm");
    clear();
    expect(vibeos_ptimer_set(30, VIBEOS_PTIMER_ITIMER(0), 2, 0, 0, 0, &left, &iv) == 0 &&
           vibeos_ptimer_used() == used + 1u, "arming one makes it");
    vibeos_ptimer_tick(3, record);
    expect(g_nfires == 1u && g_fires[0].signo == 14u && g_fires[0].id == VIBEOS_PTIMER_ITIMER(0),
           "ITIMER_REAL raises SIGALRM");

    /* CPU-time clocks count what they are charged. */
    clear();
    (void)vibeos_ptimer_set(30, VIBEOS_PTIMER_ITIMER(1), 3, 0, 0, 0, &left, &iv);   /* VIRT */
    (void)vibeos_ptimer_set(30, VIBEOS_PTIMER_ITIMER(2), 4, 2, 0, 0, &left, &iv);   /* PROF */
    vibeos_ptimer_charge(30, 30, 0, record);
    vibeos_ptimer_charge(30, 30, 0, record);
    vibeos_ptimer_charge(30, 30, 0, record);
    expect(g_nfires == 0u && vibeos_ptimer_get(30, VIBEOS_PTIMER_ITIMER(1), 0, &left, &iv) == 0 &&
           left == 3u, "time in the kernel is not ITIMER_VIRTUAL's");
    vibeos_ptimer_charge(30, 31, 1, record);
    expect(g_nfires == 1u && g_fires[0].signo == 27u, "and ITIMER_PROF counts it, and any thread's");
    vibeos_ptimer_charge(99, 99, 1, record);
    expect(g_nfires == 1u, "another process's time is not this one's");
    for (i = 0; i < 3u; i++) {
        vibeos_ptimer_charge(30, 30, 1, record);
    }
    expect(g_nfires == 3u && g_fires[1].signo == 26u && g_fires[2].signo == 27u,
           "user time counts for both; PROF reloads its interval");
    (void)vibeos_ptimer_set(30, VIBEOS_PTIMER_ITIMER(2), 0, 0, 0, 0, &left, &iv);   /* PROF is periodic */
    clear();
    expect(vibeos_ptimer_create(30, VIBEOS_PCLOCK_THREAD_CPU, VIBEOS_PTIMER_THREAD, 31, 31, 10, 0, &a) == 0,
           "a thread's CPU clock, signalling that thread");
    (void)vibeos_ptimer_set(30, a, 2, 0, 0, 0, &left, &iv);
    vibeos_ptimer_charge(30, 32, 1, record);
    vibeos_ptimer_charge(30, 32, 1, record);
    expect(g_nfires == 0u, "another thread's time is not its");
    vibeos_ptimer_charge(30, 31, 0, record);
    vibeos_ptimer_charge(30, 31, 0, record);
    expect(g_nfires == 1u && g_fires[0].to_thread && g_fires[0].tid == 31u,
           "its own, user or kernel, is - and it is told to that thread");

    /* SIGEV_NONE: counts, tells nobody. */
    clear();
    expect(vibeos_ptimer_create(30, VIBEOS_PCLOCK_REAL, VIBEOS_PTIMER_NONE, 0, 0, 0, 0, &b) == 0, "a silent timer");
    (void)vibeos_ptimer_set(30, b, 2, 0, 0, 0, &left, &iv);
    vibeos_ptimer_tick(10, record);
    expect(g_nfires == 0u && vibeos_ptimer_get(30, b, 10, &left, &iv) == 0 && left == 0u,
           "expires without telling anybody");

    /* exec keeps the interval timers and drops the rest; exit drops all. */
    (void)vibeos_ptimer_set(30, VIBEOS_PTIMER_ITIMER(0), 50, 0, 0, 10, &left, &iv);
    vibeos_ptimer_exec(30);
    expect(vibeos_ptimer_clock(30, a) == -1 && vibeos_ptimer_clock(30, b) == -1 &&
           vibeos_ptimer_get(30, VIBEOS_PTIMER_ITIMER(0), 10, &left, &iv) == 0 && left == 50u,
           "exec drops POSIX timers and keeps alarm");
    vibeos_ptimer_exit(30);
    used = vibeos_ptimer_used();
    expect(vibeos_ptimer_clock(30, VIBEOS_PTIMER_ITIMER(0)) == -1, "exit drops everything");
    clear();
    vibeos_ptimer_tick(1000, record);
    expect(g_nfires == 0u, "and nothing of a process that is gone fires");

    /* A full table is a refusal, not an overwrite. */
    vibeos_ptimer_reset();
    for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
        (void)vibeos_ptimer_create(40, VIBEOS_PCLOCK_REAL, VIBEOS_PTIMER_SIGNAL, 0, 0, 14, 0, &a);
    }
    expect(vibeos_ptimer_create(41, VIBEOS_PCLOCK_REAL, VIBEOS_PTIMER_SIGNAL, 0, 0, 14, 0, &a) == -1 &&
           vibeos_ptimer_set(41, VIBEOS_PTIMER_ITIMER(0), 5, 0, 0, 0, &left, &iv) == -1,
           "a full table refuses a new timer and a new interval timer");

    /* More timers due at once than one tick fires: the rest a tick later. */
    vibeos_ptimer_reset();
    clear();
    for (i = 0; i < 40u; i++) {
        (void)vibeos_ptimer_create(50, VIBEOS_PCLOCK_REAL, VIBEOS_PTIMER_SIGNAL, 0, 0, 14, 0, &a);
        (void)vibeos_ptimer_set(50, a, 1, 0, 0, 0, &left, &iv);
    }
    vibeos_ptimer_tick(2, record);
    t = g_nfires;
    vibeos_ptimer_tick(3, record);
    vibeos_ptimer_tick(4, record);
    expect(t > 0u && t < 40u && g_nfires == 40u, "every timer fires, in batches, none lost");

    /* With no lock registered, every call is counted. */
    {
        uint64_t before = vibeos_mbz_count(VIBEOS_MBZ_PTIMER_UNLOCKED);
        vibeos_ptimer_set_lock(0, 0);
        (void)vibeos_ptimer_used();
        expect(vibeos_mbz_count(VIBEOS_MBZ_PTIMER_UNLOCKED) == before + 1u,
               "a call with no lock registered is counted as ptimer_unlocked");
        vibeos_ptimer_set_lock(t_lock, t_unlock);
    }
    vibeos_ptimer_reset();
    return g_fail ? -1 : 0;
}
