/* Linux ABI: timers and the clocks they count (docs/abi/ L2 step 3).
 *
 * alarm, setitimer and getitimer; the POSIX timer_* calls; clock_getres,
 * gettimeofday and times. The timers themselves are a portable table
 * (kernel/sched/ptimer.c) that the architecture's tick counts down and fires;
 * what is here is Linux's arithmetic and Linux's structures around it.
 *
 * Every clock is the timer's: a tick is the resolution, and clock_getres says
 * so rather than pretending to nanoseconds. CPU time is what the scheduler's
 * accounting charged - by the tick, all of it as user time, and only for the
 * threads a process has now: a thread that has exited takes its time with it.
 * Setting the clock is refused (the registry's REFUSED, EPERM). */

#include "linux_internal.h"
#include "vibeos/ptimer.h"
#include "vibeos/account.h"

static uint32_t linux_my_tgid(void) { return ks_id(ks_current())->tgid; }

static void linux_ts_of(uint64_t ticks, linux_timespec_t *ts) {
    ts->tv_sec = (int64_t)(ticks / ks_hz());
    ts->tv_nsec = (int64_t)((ticks % ks_hz()) * (1000000000ull / ks_hz()));
}

static void linux_tv_of(uint64_t ticks, linux_timeval_t *tv) {
    tv->tv_sec = (int64_t)(ticks / ks_hz());
    tv->tv_usec = (int64_t)((ticks % ks_hz()) * (1000000ull / ks_hz()));
}

/* A timeval as ticks, rounded up; -1 if it is not a time. */
static int64_t linux_ticks_of_tv(const linux_timeval_t *tv) {
    const uint64_t per = 1000000ull / ks_hz();

    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000ll) {
        return -1;
    }
    if ((uint64_t)tv->tv_sec > (~0ull >> 2) / ks_hz()) {
        return (int64_t)(~0ull >> 2);
    }
    return (int64_t)((uint64_t)tv->tv_sec * ks_hz() + ((uint64_t)tv->tv_usec + per - 1u) / per);
}

/* CPU ticks of a process: the threads it has now. */
uint64_t linux_cpu_of_process(uint32_t tgid) {
    uint64_t sum = 0;
    uint32_t i;

    for (i = 0; i < ks_slots(); i++) {
        const vibeos_task_account_t *a;

        if (vibeos_task_state(i) == VIBEOS_TASK_FREE || !ks_id((int)i)->is_user ||
            ks_id((int)i)->tgid != tgid) {
            continue;
        }
        a = vibeos_account_task(i);
        if (a && a->ticks > ks_id((int)i)->cpu_base) {
            sum += a->ticks - ks_id((int)i)->cpu_base;
        }
    }
    return sum;
}

uint64_t linux_cpu_of_thread(int slot) {
    uint64_t now = linux_cpu_slot(slot);
    return now > ks_id(slot)->cpu_base ? now - ks_id(slot)->cpu_base : 0u;
}

/* The slot's count, every tenant's: what a new task's cpu_base is taken from. */
uint64_t linux_cpu_slot(int slot) {
    const vibeos_task_account_t *a = vibeos_account_task((uint32_t)slot);
    return a ? a->ticks : 0u;
}

/* The clocks this kernel answers for, and which of the timer table's kinds a
 * timer on each counts with; -1 for a clock it does not know. A negative id is
 * Linux's encoding of another process's CPU clock, not supported. */
static int linux_clock_kind(uint64_t clk) {
    switch ((int64_t)(int32_t)(uint32_t)clk) {
        case LINUX_CLOCK_REALTIME:
        case LINUX_CLOCK_MONOTONIC:
        case LINUX_CLOCK_BOOTTIME:
        case LINUX_CLOCK_MONOTONIC_RAW:
        case LINUX_CLOCK_REALTIME_COARSE:
        case LINUX_CLOCK_MONOTONIC_COARSE:
        case LINUX_CLOCK_REALTIME_ALARM:
        case LINUX_CLOCK_BOOTTIME_ALARM:
        case LINUX_CLOCK_TAI:
            return VIBEOS_PCLOCK_REAL;
        case LINUX_CLOCK_PROCESS_CPUTIME_ID:
            return VIBEOS_PCLOCK_PROCESS_CPU;
        case LINUX_CLOCK_THREAD_CPUTIME_ID:
            return VIBEOS_PCLOCK_THREAD_CPU;
        default:
            return -1;
    }
}

/* What a clock reads now, in ticks; -1 for one it does not know. */
int64_t linux_clock_read(uint64_t clk) {
    switch (linux_clock_kind(clk)) {
        case VIBEOS_PCLOCK_REAL:        return (int64_t)ks_ticks();
        case VIBEOS_PCLOCK_PROCESS_CPU: return (int64_t)linux_cpu_of_process(linux_my_tgid());
        case VIBEOS_PCLOCK_THREAD_CPU:  return (int64_t)linux_cpu_of_thread(ks_current());
        default:                        return -1;
    }
}

/* ---- alarm, setitimer, getitimer -------------------------------------------- */

/* alarm(): the real interval timer, in seconds, and what was left of it -
 * rounded to the nearest second and never 0 for a timer that was still armed,
 * because 0 means "there was none". */
static long linux_sys_alarm(uint64_t secs) {
    uint64_t left = 0, interval = 0, r;

    if (vibeos_ptimer_set(linux_my_tgid(), VIBEOS_PTIMER_ITIMER(LINUX_ITIMER_REAL),
                          (uint64_t)(uint32_t)secs * ks_hz(), 0, 0, ks_ticks(),
                          &left, &interval) != 0) {
        return 0;   /* the table is full; alarm cannot fail on Linux either */
    }
    r = left / ks_hz();
    if ((left % ks_hz()) * 2u >= ks_hz()) {
        r++;
    }
    return (long)(r == 0u && left != 0u ? 1u : r);
}

static long linux_sys_setitimer(uint64_t which, uint64_t new_uptr, uint64_t old_uptr) {
    linux_itimerval_t in, out;
    uint64_t left = 0, interval = 0;
    int64_t value = 0, every = 0;

    if (ks_current() < 0 || (uint32_t)which > LINUX_ITIMER_PROF) {
        return -VIBEOS_EINVAL;
    }
    if (new_uptr != 0u) {
        if (vibeos_uaccess_copy(&in, (const void *)(uintptr_t)new_uptr, sizeof(in)) != 0) {
            return -VIBEOS_EFAULT;
        }
        value = linux_ticks_of_tv(&in.it_value);
        every = linux_ticks_of_tv(&in.it_interval);
        if (value < 0 || every < 0) {
            return -VIBEOS_EINVAL;
        }
    }
    /* No new value is a disarm, as Linux has always read it. */
    if (vibeos_ptimer_set(linux_my_tgid(), VIBEOS_PTIMER_ITIMER((uint32_t)which),
                          (uint64_t)value, (uint64_t)every, 0, ks_ticks(), &left, &interval) != 0) {
        return -VIBEOS_EAGAIN;
    }
    if (old_uptr != 0u) {
        linux_tv_of(left, &out.it_value);
        linux_tv_of(interval, &out.it_interval);
        if (vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, &out, sizeof(out)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return 0;
}

static long linux_sys_getitimer(uint64_t which, uint64_t cur_uptr) {
    linux_itimerval_t out;
    uint64_t left = 0, interval = 0;

    if (ks_current() < 0 || (uint32_t)which > LINUX_ITIMER_PROF) {
        return -VIBEOS_EINVAL;
    }
    (void)vibeos_ptimer_get(linux_my_tgid(), VIBEOS_PTIMER_ITIMER((uint32_t)which), ks_ticks(),
                            &left, &interval);
    linux_tv_of(left, &out.it_value);
    linux_tv_of(interval, &out.it_interval);
    return vibeos_uaccess_copy((void *)(uintptr_t)cur_uptr, &out, sizeof(out)) == 0 ? 0 : -VIBEOS_EFAULT;
}

/* ---- POSIX timers ------------------------------------------------------------- */

static long linux_sys_timer_create(uint64_t clk, uint64_t sev_uptr, uint64_t id_uptr) {
    linux_sigevent_t sev;
    uint32_t notify = VIBEOS_PTIMER_SIGNAL, tid = 0, signo = VIBEOS_SIGALRM;
    uint64_t value = VIBEOS_PTIMER_VALUE_IS_ID;
    int kind = linux_clock_kind(clk);
    int32_t id;

    if (ks_current() < 0 || kind < 0) {
        return -VIBEOS_EINVAL;
    }
    /* Clocks Linux keeps no timers on. */
    if ((int32_t)(uint32_t)clk == LINUX_CLOCK_MONOTONIC_RAW ||
        (int32_t)(uint32_t)clk == LINUX_CLOCK_REALTIME_COARSE ||
        (int32_t)(uint32_t)clk == LINUX_CLOCK_MONOTONIC_COARSE) {
        return -VIBEOS_EINVAL;
    }
    if (sev_uptr != 0u) {
        if (vibeos_uaccess_copy(&sev, (const void *)(uintptr_t)sev_uptr, sizeof(sev)) != 0) {
            return -VIBEOS_EFAULT;
        }
        value = sev.sigev_value;
        signo = (uint32_t)sev.sigev_signo;
        switch (sev.sigev_notify) {
            case LINUX_SIGEV_NONE:
                notify = VIBEOS_PTIMER_NONE;
                break;
            case LINUX_SIGEV_SIGNAL:
            case LINUX_SIGEV_THREAD:   /* the C library's thread; to the kernel, a signal */
                notify = VIBEOS_PTIMER_SIGNAL;
                break;
            case LINUX_SIGEV_THREAD_ID: {
                /* To one thread, which has to be one of this process's. */
                int t;
                ks_lock(ks_sched_lock(), __func__);
                t = ks_task_by_tid((uint32_t)sev.notify_tid);
                if (t >= 0 && ks_id(t)->tgid != linux_my_tgid()) {
                    t = -1;
                }
                ks_unlock(ks_sched_lock());
                if (t < 0) {
                    return -VIBEOS_EINVAL;
                }
                notify = VIBEOS_PTIMER_THREAD;
                tid = (uint32_t)sev.notify_tid;
                break;
            }
            default:
                return -VIBEOS_EINVAL;
        }
        if (notify != VIBEOS_PTIMER_NONE && (signo == 0u || signo > VIBEOS_SIG_MAX)) {
            return -VIBEOS_EINVAL;
        }
    }
    if (vibeos_ptimer_create(linux_my_tgid(), (uint32_t)kind, notify, tid,
                             ks_id(ks_current())->pid, signo, value, &id) != 0) {
        return -VIBEOS_EAGAIN;
    }
    if (vibeos_uaccess_copy((void *)(uintptr_t)id_uptr, &id, sizeof(id)) != 0) {
        (void)vibeos_ptimer_delete(linux_my_tgid(), id);
        return -VIBEOS_EFAULT;
    }
    return 0;
}

static long linux_sys_timer_settime(uint64_t idarg, uint64_t flags, uint64_t new_uptr,
                                    uint64_t old_uptr) {
    int32_t id = (int32_t)(uint32_t)idarg;
    linux_itimerspec_t in, out;
    int64_t value, every;
    uint64_t left = 0, interval = 0;
    int kind;

    if (ks_current() < 0 || id < 0 || (flags & ~(uint64_t)LINUX_TIMER_ABSTIME)) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&in, (const void *)(uintptr_t)new_uptr, sizeof(in)) != 0) {
        return -VIBEOS_EFAULT;
    }
    value = linux_ticks_of(&in.it_value);
    every = linux_ticks_of(&in.it_interval);
    if (value < 0 || every < 0 || (kind = vibeos_ptimer_clock(linux_my_tgid(), id)) < 0) {
        return -VIBEOS_EINVAL;
    }
    /* An absolute time on a CPU clock is turned into what is left of it: those
     * timers count down what they are charged. One already past fires on the
     * next tick charged. */
    if ((flags & LINUX_TIMER_ABSTIME) && kind != VIBEOS_PCLOCK_REAL && value != 0) {
        int64_t now = kind == VIBEOS_PCLOCK_THREAD_CPU ? (int64_t)linux_cpu_of_thread(ks_current())
                                                       : (int64_t)linux_cpu_of_process(linux_my_tgid());
        value = value > now ? value - now : 1;
    }
    if (vibeos_ptimer_set(linux_my_tgid(), id, (uint64_t)value, (uint64_t)every,
                          (flags & LINUX_TIMER_ABSTIME) != 0u, ks_ticks(), &left, &interval) != 0) {
        return -VIBEOS_EINVAL;
    }
    if (old_uptr != 0u) {
        linux_ts_of(left, &out.it_value);
        linux_ts_of(interval, &out.it_interval);
        if (vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, &out, sizeof(out)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return 0;
}

static long linux_sys_timer_gettime(uint64_t idarg, uint64_t cur_uptr) {
    int32_t id = (int32_t)(uint32_t)idarg;
    linux_itimerspec_t out;
    uint64_t left = 0, interval = 0;

    if (ks_current() < 0 || id < 0 ||
        vibeos_ptimer_get(linux_my_tgid(), id, ks_ticks(), &left, &interval) != 0) {
        return -VIBEOS_EINVAL;
    }
    linux_ts_of(left, &out.it_value);
    linux_ts_of(interval, &out.it_interval);
    return vibeos_uaccess_copy((void *)(uintptr_t)cur_uptr, &out, sizeof(out)) == 0 ? 0 : -VIBEOS_EFAULT;
}

static long linux_sys_timer_getoverrun(uint64_t idarg) {
    int32_t id = (int32_t)(uint32_t)idarg;
    int r;

    if (ks_current() < 0 || id < 0 || (r = vibeos_ptimer_overrun(linux_my_tgid(), id)) < 0) {
        return -VIBEOS_EINVAL;
    }
    return r;
}

static long linux_sys_timer_delete(uint64_t idarg) {
    int32_t id = (int32_t)(uint32_t)idarg;

    if (ks_current() < 0 || id < 0 || vibeos_ptimer_delete(linux_my_tgid(), id) != 0) {
        return -VIBEOS_EINVAL;
    }
    return 0;
}

/* ---- the clocks -------------------------------------------------------------- */

static long linux_sys_clock_getres(uint64_t clk, uint64_t res_uptr) {
    linux_timespec_t res;

    if (linux_clock_kind(clk) < 0) {
        return -VIBEOS_EINVAL;
    }
    if (res_uptr == 0u) {
        return 0;
    }
    res.tv_sec = 0;
    res.tv_nsec = (int64_t)(1000000000ull / ks_hz());   /* one tick: what the clock can tell */
    return vibeos_uaccess_copy((void *)(uintptr_t)res_uptr, &res, sizeof(res)) == 0 ? 0 : -VIBEOS_EFAULT;
}

static long linux_sys_gettimeofday(uint64_t tv_uptr, uint64_t tz_uptr) {
    linux_timeval_t tv;
    linux_timezone_t tz;

    if (tv_uptr != 0u) {
        linux_tv_of(ks_ticks(), &tv);
        if (vibeos_uaccess_copy((void *)(uintptr_t)tv_uptr, &tv, sizeof(tv)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    if (tz_uptr != 0u) {
        tz.tz_minuteswest = 0;
        tz.tz_dsttime = 0;
        if (vibeos_uaccess_copy((void *)(uintptr_t)tz_uptr, &tz, sizeof(tz)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return 0;
}

/* times(): in USER_HZ, which is 100 on every Linux - a program divides by
 * sysconf(_SC_CLK_TCK), and the C library answers 100 without asking. */
static uint64_t linux_user_hz(uint64_t ticks) {
    return ticks * 100u / ks_hz();
}

static long linux_sys_times(uint64_t buf_uptr) {
    linux_tms_t t;
    const vibeos_procstate_t *ps;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    if (buf_uptr != 0u) {
        ps = ks_ps(ks_current());
        t.tms_utime = (int64_t)linux_user_hz(linux_cpu_of_process(linux_my_tgid()));
        t.tms_stime = 0;
        t.tms_cutime = (int64_t)linux_user_hz(ps ? __atomic_load_n(&ps->cpu_children, __ATOMIC_RELAXED) : 0u);
        t.tms_cstime = 0;
        if (vibeos_uaccess_copy((void *)(uintptr_t)buf_uptr, &t, sizeof(t)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    return (long)linux_user_hz(ks_ticks());
}

/* ---- the syscalls this file implements ----------------------------------------- */
#define LINUX_TIMER_SYSCALLS(X) \
    X(37,  alarm,            ALARM,         NOPTR, linux_sys_alarm(ARG(0))) \
    X(38,  setitimer,        ITIMER_SET,    PTRS(IN_OPT(1, sizeof(linux_itimerval_t)), OUT_OPT(2, sizeof(linux_itimerval_t))), linux_sys_setitimer(ARG(0), ARG(1), ARG(2))) \
    X(36,  getitimer,        ITIMER_GET,    PTRS(OUT(1, sizeof(linux_itimerval_t))), linux_sys_getitimer(ARG(0), ARG(1))) \
    X(222, timer_create,     TIMER_CREATE,  PTRS(IN_OPT(1, sizeof(linux_sigevent_t)), OUT(2, 4)), linux_sys_timer_create(ARG(0), ARG(1), ARG(2))) \
    X(223, timer_settime,    TIMER_SET,     PTRS(IN(2, sizeof(linux_itimerspec_t)), OUT_OPT(3, sizeof(linux_itimerspec_t))), linux_sys_timer_settime(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(224, timer_gettime,    TIMER_GET,     PTRS(OUT(1, sizeof(linux_itimerspec_t))), linux_sys_timer_gettime(ARG(0), ARG(1))) \
    X(225, timer_getoverrun, TIMER_OVERRUN, NOPTR, linux_sys_timer_getoverrun(ARG(0))) \
    X(226, timer_delete,     TIMER_DELETE,  NOPTR, linux_sys_timer_delete(ARG(0))) \
    X(229, clock_getres,     CLOCK_GETRES,  PTRS(OUT_OPT(1, sizeof(linux_timespec_t))), linux_sys_clock_getres(ARG(0), ARG(1))) \
    X(96,  gettimeofday,     GETTIMEOFDAY,  PTRS(OUT_OPT(0, sizeof(linux_timeval_t)), OUT_OPT(1, sizeof(linux_timezone_t))), linux_sys_gettimeofday(ARG(0), ARG(1))) \
    X(100, times,            TIMES,         PTRS(OUT_OPT(0, sizeof(linux_tms_t))), linux_sys_times(ARG(0)))

LINUX_DEFINE_SYSCALLS(timer, LINUX_TIMER_SYSCALLS)
