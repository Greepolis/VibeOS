#ifndef VIBEOS_PTIMER_H
#define VIBEOS_PTIMER_H

#include <stdint.h>

/* Timers that belong to a process (docs/abi/ L2 step 3): what alarm, setitimer
 * and the POSIX timer_* calls arm, and what ends a wait that nothing else
 * would end. LTP's own timeout is one of them - a test that hangs is stopped
 * by alarm(), so until these existed one stuck test took the rest of its boot.
 *
 * One table for the machine, keyed by thread group, with a lock of its own
 * (vibeos_ptimer_set_lock): it is armed by the process's syscalls on any core,
 * counted down by every core's tick and fired by the clock owner's, and a table
 * reached from all of those is not one to leave to its callers' locking.
 *
 * Time is in the timer's ticks. Two kinds of clock count differently:
 *
 *   REAL           the machine's clock: `expires` is the tick it fires at
 *   VIRT, PROF,    the process's or one thread's CPU time: `expires` is the
 *   PROCESS_CPU,   ticks left, charged by the tick of whichever core is
 *   THREAD_CPU     running it (VIRT only while it runs in user mode)
 *
 * Firing does not happen under the table's lock: expiry collects what fired
 * and calls back after releasing it, because the callback raises a signal,
 * which takes the scheduler's lock - and the table is reached from code that
 * holds that lock. The order is: this lock never encloses another. */

#define VIBEOS_PTIMER_MAX 128u

typedef enum {
    VIBEOS_PCLOCK_REAL = 0,
    VIBEOS_PCLOCK_VIRT,          /* ITIMER_VIRTUAL: user time of the process  */
    VIBEOS_PCLOCK_PROF,          /* ITIMER_PROF: all CPU time of the process  */
    VIBEOS_PCLOCK_PROCESS_CPU,   /* a POSIX timer on CLOCK_PROCESS_CPUTIME_ID */
    VIBEOS_PCLOCK_THREAD_CPU     /* one on CLOCK_THREAD_CPUTIME_ID           */
} vibeos_pclock_t;

/* How an expiry is told: a signal to the process, a signal to one thread, or
 * nothing at all (a timer read with timer_gettime). */
#define VIBEOS_PTIMER_SIGNAL 0u
#define VIBEOS_PTIMER_NONE   1u
#define VIBEOS_PTIMER_THREAD 2u

/* The three interval timers have fixed ids below zero; POSIX timers are
 * numbered from zero per process, smallest free first, as Linux numbers them. */
#define VIBEOS_PTIMER_ITIMER(which) (-1 - (int32_t)(which))
/* And two more the kernel arms for itself (L2 step 4): RLIMIT_CPU's soft
 * limit, which raises SIGXCPU every second once reached, and its hard limit,
 * which raises SIGKILL. Both count the process's CPU time. */
#define VIBEOS_PTIMER_RLIMIT_SOFT (-4)
#define VIBEOS_PTIMER_RLIMIT_HARD (-5)
#define VIBEOS_PTIMER_FIXED_LAST  VIBEOS_PTIMER_RLIMIT_HARD

/* One expiry, for the callback. */
typedef struct {
    uint32_t tgid;
    uint32_t tid;        /* THREAD: the thread to signal */
    uint32_t to_thread;
    uint32_t signo;
    int32_t id;          /* the timer: an itimer's negative id, or a POSIX id */
    uint64_t value;      /* sigev_value */
    uint32_t overrun;    /* periods that went by with no tick to fire them */
} vibeos_ptimer_fire_t;

/* Raise the signal for one expiry. 0 if it was raised. If that timer's signal
 * was still pending from an earlier expiry this one is an overrun, not a second
 * signal: the callback adds it (and `overrun`) to the pending signal's count,
 * as Linux does, and returns 1 + that count. */
typedef int (*vibeos_ptimer_fire_fn)(const vibeos_ptimer_fire_t *f);

void vibeos_ptimer_set_lock(void (*lock)(void), void (*unlock)(void));
void vibeos_ptimer_reset(void);

/* A POSIX timer: 0 and its id, -1 if the table is full (EAGAIN). `charged` is
 * the thread a THREAD_CPU clock counts. A `value` of VIBEOS_PTIMER_VALUE_IS_ID
 * is replaced by the id the timer gets: what timer_create without a sigevent
 * puts in sigev_value, and only the table knows the id before it is handed out. */
#define VIBEOS_PTIMER_VALUE_IS_ID 0xFFFFFFFFFFFFFFFFull
int vibeos_ptimer_create(uint32_t tgid, uint32_t clock, uint32_t notify, uint32_t notify_tid,
                         uint32_t charged, uint32_t signo, uint64_t value, int32_t *id_out);
/* Arm (`ticks` from now, or at the clock reading `ticks` when `absolute`;
 * 0 disarms) and say what it was. An interval timer is made on first use.
 * 0, or -1 if the process has no such timer, or the table is full. */
int vibeos_ptimer_set(uint32_t tgid, int32_t id, uint64_t ticks, uint64_t interval,
                      int absolute, uint64_t now, uint64_t *old_left, uint64_t *old_interval);
/* What is left before it fires (0: disarmed) and its interval. -1: no timer. */
int vibeos_ptimer_get(uint32_t tgid, int32_t id, uint64_t now, uint64_t *left, uint64_t *interval);
/* Expiries missed before the last one was taken; -1: no timer. */
int vibeos_ptimer_overrun(uint32_t tgid, int32_t id);
int vibeos_ptimer_delete(uint32_t tgid, int32_t id);
/* The clock a timer counts (vibeos_pclock_t), or -1: no such timer. */
int vibeos_ptimer_clock(uint32_t tgid, int32_t id);
/* The process ran a new program: its POSIX timers go, its interval timers
 * stay (Linux keeps alarm() across exec). */
void vibeos_ptimer_exec(uint32_t tgid);
/* The process is gone: everything it had goes. */
void vibeos_ptimer_exit(uint32_t tgid);

/* The clock owner's tick: fire every REAL timer that is due. */
void vibeos_ptimer_tick(uint64_t now, vibeos_ptimer_fire_fn fire);
/* Any core's tick, charged to the thread it was running. */
void vibeos_ptimer_charge(uint32_t tgid, uint32_t tid, int user, vibeos_ptimer_fire_fn fire);

/* How many timers are in use; for the gate's count and the tests. */
uint32_t vibeos_ptimer_used(void);
/* Expiries fired, ever, and those that found their signal still pending. */
uint64_t vibeos_ptimer_fired(void);
uint64_t vibeos_ptimer_overruns(void);

#endif
