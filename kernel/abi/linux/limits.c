/* Linux ABI: limits, usage, priorities and personality (docs/abi/ L2 step 4).
 *
 * A process carries Linux's sixteen resource limits, soft and hard, in its
 * state (vibeos_procstate_t). Five are enforced where the kernel already had
 * the mechanism: the descriptor table's own limit (NOFILE), the regular-file
 * write path (FSIZE: SIGXFSZ and EFBIG), brk (DATA), fork and clone (NPROC,
 * for a user who is not root) and two CPU-time timers in the process-timer
 * table (CPU: SIGXCPU every second past the soft limit, SIGKILL at the hard
 * one). The rest are kept and reported, and say so in the registry.
 *
 * Priorities are the scheduler's nice, per task, which is what Linux's
 * PRIO_PROCESS names too. */

#include "linux_internal.h"

/* What a process starts with when nothing it came from says otherwise: the
 * first process of the machine, made by the architecture rather than by fork.
 * Linux's defaults where the kernel can honour them; the stack, the task count
 * and the descriptor count are what this machine has. */
void linux_procstate_defaults(vibeos_procstate_t *ps) {
    uint32_t i;

    for (i = 0; i < VIBEOS_RLIM_COUNT; i++) {
        ps->rlim_cur[i] = VIBEOS_RLIM_INFINITY;
        ps->rlim_max[i] = VIBEOS_RLIM_INFINITY;
    }
    ps->rlim_cur[LINUX_RLIMIT_CORE] = 0;
    ps->rlim_cur[LINUX_RLIMIT_STACK] = ps->rlim_max[LINUX_RLIMIT_STACK] = ks_stack_bytes();
    ps->rlim_cur[LINUX_RLIMIT_NPROC] = ps->rlim_max[LINUX_RLIMIT_NPROC] = ks_slots();
    ps->rlim_cur[LINUX_RLIMIT_NOFILE] = ps->rlim_max[LINUX_RLIMIT_NOFILE] = (uint64_t)LINUX_MAX_FDS;
    ps->rlim_cur[LINUX_RLIMIT_MEMLOCK] = ps->rlim_max[LINUX_RLIMIT_MEMLOCK] = 8ull << 20;
    ps->rlim_cur[LINUX_RLIMIT_SIGPENDING] = ps->rlim_max[LINUX_RLIMIT_SIGPENDING] = 1024u;
    ps->rlim_cur[LINUX_RLIMIT_MSGQUEUE] = ps->rlim_max[LINUX_RLIMIT_MSGQUEUE] = 819200u;
    ps->rlim_cur[LINUX_RLIMIT_NICE] = ps->rlim_max[LINUX_RLIMIT_NICE] = 0;
    ps->rlim_cur[LINUX_RLIMIT_RTPRIO] = ps->rlim_max[LINUX_RLIMIT_RTPRIO] = 0;
    ps->personality = 0;
}

/* RLIMIT_CPU as two timers on the process's CPU time: SIGXCPU when the soft
 * limit is reached and every second after it, SIGKILL at the hard one. Armed
 * for what is left of each, given the time already run; a limit already past
 * fires on the next tick charged. */
void linux_rlimit_cpu_arm(uint32_t tgid, const vibeos_procstate_t *ps, uint64_t ran) {
    uint64_t left = 0, iv = 0, lim;
    int k;

    for (k = 0; k < 2; k++) {
        int32_t id = k == 0 ? VIBEOS_PTIMER_RLIMIT_SOFT : VIBEOS_PTIMER_RLIMIT_HARD;
        uint64_t secs = k == 0 ? ps->rlim_cur[LINUX_RLIMIT_CPU] : ps->rlim_max[LINUX_RLIMIT_CPU];

        /* No limit, or one too far away to be reached: disarmed. */
        if (secs == VIBEOS_RLIM_INFINITY || secs > (1ull << 32)) {
            (void)vibeos_ptimer_set(tgid, id, 0, 0, 0, ks_ticks(), &left, &iv);
            continue;
        }
        lim = secs * ks_hz();
        (void)vibeos_ptimer_set(tgid, id, lim > ran ? lim - ran : 1u,
                                k == 0 ? ks_hz() : 0u, 0, ks_ticks(), &left, &iv);
    }
}

/* May the caller have one more task? RLIMIT_NPROC counts every task a user
 * has, threads included, as Linux counts them; the superuser is not counted
 * against it (Linux exempts CAP_SYS_RESOURCE). 0, or -EAGAIN. */
long linux_nproc_check(void) {
    vibeos_cred_t me;
    const vibeos_procstate_t *ps = ks_ps(ks_current());
    uint64_t lim, n = 0;
    uint32_t i;

    linux_cred(&me);
    if (me.euid == 0u || !ps) {
        return 0;
    }
    lim = ps->rlim_cur[LINUX_RLIMIT_NPROC];
    ks_lock(ks_sched_lock(), __func__);
    for (i = 0; i < ks_slots(); i++) {
        const vibeos_procstate_t *tp;
        vibeos_task_state_t st = vibeos_task_state(i);

        if (st == VIBEOS_TASK_FREE || st == VIBEOS_TASK_SETUP || !ks_id((int)i)->is_user) {
            continue;
        }
        tp = ks_ps((int)i);
        if (tp && tp->cred.uid == me.uid) {
            n++;
        }
    }
    ks_unlock(ks_sched_lock());
    return n >= lim ? -VIBEOS_EAGAIN : 0;
}

/* ---- getrlimit, setrlimit, prlimit64 ---------------------------------------------- */

/* Change one limit of `ps` (the process `tgid`) as `c` asks. The rules are
 * Linux's: soft above hard is EINVAL; raising the hard limit is the
 * superuser's; and nobody raises NOFILE past what the descriptor table can
 * hold (Linux's nr_open). */
static long linux_rlimit_put(vibeos_procstate_t *ps, uint32_t tgid, uint32_t res,
                             const linux_rlimit64_t *nl, const vibeos_cred_t *c) {
    if (nl->rlim_cur > nl->rlim_max) {
        return -VIBEOS_EINVAL;
    }
    if (nl->rlim_max > ps->rlim_max[res] && c->euid != 0u) {
        return -VIBEOS_EPERM;
    }
    if (res == LINUX_RLIMIT_NOFILE && nl->rlim_max > (uint64_t)LINUX_MAX_FDS) {
        return -VIBEOS_EPERM;
    }
    ps->rlim_cur[res] = nl->rlim_cur;
    ps->rlim_max[res] = nl->rlim_max;
    if (res == LINUX_RLIMIT_NOFILE) {
        /* The table's own limit, which open and dup already ask. One aligned
         * store: a reader under files_lock sees the old value or the new. */
        ps->files.limit = (uint32_t)nl->rlim_cur;
    } else if (res == LINUX_RLIMIT_CPU) {
        linux_rlimit_cpu_arm(tgid, ps, linux_cpu_of_process(tgid));
    }
    return 0;
}

/* The one implementation the three calls share. `pid` 0 is the caller. */
static long linux_rlimit(uint64_t pid, uint64_t resource, uint64_t new_uptr, uint64_t old_uptr) {
    uint32_t res = (uint32_t)resource;
    linux_rlimit64_t nl, ol;
    vibeos_cred_t c;
    vibeos_procstate_t *ps;
    uint32_t tgid;
    int self, target;
    long r = 0;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (res >= LINUX_RLIM_NLIMITS) {
        return -VIBEOS_EINVAL;
    }
    if (new_uptr != 0u &&
        vibeos_uaccess_copy(&nl, (const void *)(uintptr_t)new_uptr, sizeof(nl)) != 0) {
        return -VIBEOS_EFAULT;
    }
    linux_cred(&c);
    self = (uint32_t)pid == 0u || (uint32_t)pid == ks_id(ks_current())->tgid;
    if (self) {
        ps = ks_ps(ks_current());
        if (!ps) {
            return -VIBEOS_EINVAL;
        }
        tgid = ks_id(ks_current())->tgid;
        ks_lock(&ps->files_lock, __func__);
        ol.rlim_cur = ps->rlim_cur[res];
        ol.rlim_max = ps->rlim_max[res];
        if (new_uptr != 0u) {
            r = linux_rlimit_put(ps, tgid, res, &nl, &c);
        }
        ks_unlock(&ps->files_lock);
    } else {
        /* Another process: found, judged and changed under the scheduler's
         * lock, which keeps its state from being let go meanwhile. Its
         * files_lock is not taken under that one - the order here is the other
         * way round - so the limits are read and written as the aligned words
         * they are, as kill reads credentials. The superuser, or somebody
         * whose ids all match the target's (Linux's rule for prlimit). */
        ks_lock(ks_sched_lock(), __func__);
        target = ks_task_by_pid((uint32_t)pid);
        ps = target >= 0 ? ks_ps(target) : 0;
        if (!ps) {
            r = -VIBEOS_ESRCH;
        } else if (c.euid != 0u &&
                   (ps->cred.uid != c.uid || ps->cred.euid != c.uid || ps->cred.suid != c.uid ||
                    ps->cred.gid != c.gid || ps->cred.egid != c.gid || ps->cred.sgid != c.gid)) {
            r = -VIBEOS_EPERM;
        } else {
            tgid = ks_id(target)->tgid;
            ol.rlim_cur = ps->rlim_cur[res];
            ol.rlim_max = ps->rlim_max[res];
            if (new_uptr != 0u) {
                r = linux_rlimit_put(ps, tgid, res, &nl, &c);
            }
        }
        ks_unlock(ks_sched_lock());
    }
    if (r == 0 && old_uptr != 0u &&
        vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, &ol, sizeof(ol)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return r;
}

static long linux_sys_getrlimit(uint64_t resource, uint64_t uptr) {
    return linux_rlimit(0, resource, 0, uptr);
}

static long linux_sys_setrlimit(uint64_t resource, uint64_t uptr) {
    return linux_rlimit(0, resource, uptr, 0);
}

static long linux_sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_uptr, uint64_t old_uptr) {
    return linux_rlimit(pid, resource, new_uptr, old_uptr);
}

/* ---- getrusage ----------------------------------------------------------------- */

static void linux_tv_of_ticks(uint64_t ticks, linux_timeval_t *tv) {
    tv->tv_sec = (int64_t)(ticks / ks_hz());
    tv->tv_usec = (int64_t)((ticks % ks_hz()) * (1000000ull / ks_hz()));
}

/* The CPU time, all of it user time, and nothing else counted: the scheduler
 * knows how long a task ran and not what it did meanwhile. */
static long linux_sys_getrusage(uint64_t who, uint64_t uptr) {
    linux_rusage_t ru;
    const vibeos_procstate_t *ps;
    uint64_t ticks;
    uint32_t i;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    switch ((int32_t)(uint32_t)who) {
        case LINUX_RUSAGE_SELF:
            ticks = linux_cpu_of_process(ks_id(ks_current())->tgid);
            break;
        case LINUX_RUSAGE_THREAD:
            ticks = linux_cpu_of_thread(ks_current());
            break;
        case LINUX_RUSAGE_CHILDREN:
            ps = ks_ps(ks_current());
            ticks = ps ? __atomic_load_n(&ps->cpu_children, __ATOMIC_RELAXED) : 0u;
            break;
        default:
            return -VIBEOS_EINVAL;
    }
    for (i = 0; i < sizeof(ru); i++) {
        ((unsigned char *)&ru)[i] = 0;
    }
    linux_tv_of_ticks(ticks, &ru.ru_utime);
    return vibeos_uaccess_copy((void *)(uintptr_t)uptr, &ru, sizeof(ru)) == 0 ? 0 : -VIBEOS_EFAULT;
}

/* ---- getpriority, setpriority ---------------------------------------------------- */

/* Does task `i` belong to what `which` and `who` name? `who` 0 is the caller's
 * own: itself, its group, its user. */
static int linux_prio_names(int i, int32_t which, uint32_t who, const vibeos_cred_t *me) {
    const vibeos_task_t *t = ks_id(i);
    const vibeos_procstate_t *ps;
    vibeos_task_state_t st = vibeos_task_state((uint32_t)i);

    if (!t->is_user || st == VIBEOS_TASK_FREE || st == VIBEOS_TASK_SETUP || st == VIBEOS_TASK_ZOMBIE) {
        return 0;
    }
    switch (which) {
        case LINUX_PRIO_PROCESS:
            return who == 0u ? i == ks_current() : t->pid == who;
        case LINUX_PRIO_PGRP:
            return t->pgid == (who == 0u ? ks_id(ks_current())->pgid : who);
        case LINUX_PRIO_USER:
            ps = ks_ps(i);
            return ps && ps->cred.uid == (who == 0u ? me->uid : who);
        default:
            return 0;
    }
}

/* Linux's raw answer is 20 - nice, so that it is never negative and never an
 * error; the C library turns it back. The highest priority - lowest nice - of
 * everything named. */
static long linux_sys_getpriority(uint64_t which, uint64_t who) {
    int32_t w = (int32_t)(uint32_t)which;
    vibeos_cred_t me;
    int best = 100;
    uint32_t i;

    if (ks_current() < 0 || w < LINUX_PRIO_PROCESS || w > LINUX_PRIO_USER) {
        return -VIBEOS_EINVAL;
    }
    linux_cred(&me);
    ks_lock(ks_sched_lock(), __func__);
    for (i = 0; i < ks_slots(); i++) {
        if (linux_prio_names((int)i, w, (uint32_t)who, &me)) {
            int n = ks_task_nice((int)i);
            if (n < best) {
                best = n;
            }
        }
    }
    ks_unlock(ks_sched_lock());
    return best == 100 ? -VIBEOS_ESRCH : (long)(20 - best);
}

/* A task's nice may be changed by the superuser, or by a caller whose
 * effective user is the task's real or effective one; lowering it - asking for
 * more of the machine - needs the superuser or RLIMIT_NICE's allowance. */
static long linux_sys_setpriority(uint64_t which, uint64_t who, uint64_t prio) {
    int32_t w = (int32_t)(uint32_t)which;
    int32_t nice = (int32_t)(uint32_t)prio;
    const vibeos_procstate_t *mps;
    vibeos_cred_t me;
    uint64_t allowance;
    int found = 0;
    long r = 0;
    uint32_t i;

    if (ks_current() < 0 || w < LINUX_PRIO_PROCESS || w > LINUX_PRIO_USER) {
        return -VIBEOS_EINVAL;
    }
    nice = nice < -20 ? -20 : nice > 19 ? 19 : nice;
    linux_cred(&me);
    mps = ks_ps(ks_current());
    allowance = mps ? mps->rlim_cur[LINUX_RLIMIT_NICE] : 0u;
    ks_lock(ks_sched_lock(), __func__);
    for (i = 0; i < ks_slots(); i++) {
        const vibeos_procstate_t *tp;

        if (!linux_prio_names((int)i, w, (uint32_t)who, &me)) {
            continue;
        }
        found = 1;
        tp = ks_ps((int)i);
        if (me.euid != 0u && (!tp || (tp->cred.uid != me.euid && tp->cred.euid != me.euid))) {
            r = -VIBEOS_EPERM;
            continue;
        }
        if (me.euid != 0u && nice < ks_task_nice((int)i) &&
            (allowance == 0u || (uint64_t)(20 - nice) > allowance)) {
            r = -VIBEOS_EACCES;
            continue;
        }
        (void)ks_task_set_nice((int)i, nice);
    }
    ks_unlock(ks_sched_lock());
    return found ? r : -VIBEOS_ESRCH;
}

/* ---- personality ----------------------------------------------------------------- */

/* The execution domain: this kernel has one, Linux's, and keeps whatever a
 * program sets so that reading it back says what was asked. 0xffffffff only
 * asks. */
static long linux_sys_personality(uint64_t persona) {
    vibeos_procstate_t *ps;
    uint32_t old;

    if (ks_current() < 0 || !(ps = ks_ps(ks_current()))) {
        return -VIBEOS_EINVAL;
    }
    ks_lock(&ps->files_lock, __func__);
    old = ps->personality;
    if ((uint32_t)persona != 0xFFFFFFFFu) {
        ps->personality = (uint32_t)persona;
    }
    ks_unlock(&ps->files_lock);
    return (long)old;
}

/* ---- the syscalls this file implements ---------------------------------------- */
#define LINUX_LIMITS_SYSCALLS(X) \
    X(97,  getrlimit,   GETRLIMIT,   PTRS(OUT(1, sizeof(linux_rlimit64_t))), linux_sys_getrlimit(ARG(0), ARG(1))) \
    X(160, setrlimit,   SETRLIMIT,   PTRS(IN(1, sizeof(linux_rlimit64_t))), linux_sys_setrlimit(ARG(0), ARG(1))) \
    X(302, prlimit64,   PRLIMIT,     PTRS(IN_OPT(2, sizeof(linux_rlimit64_t)), OUT_OPT(3, sizeof(linux_rlimit64_t))), linux_sys_prlimit64(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(98,  getrusage,   GETRUSAGE,   PTRS(OUT(1, sizeof(linux_rusage_t))), linux_sys_getrusage(ARG(0), ARG(1))) \
    X(140, getpriority, GETPRIORITY, NOPTR, linux_sys_getpriority(ARG(0), ARG(1))) \
    X(141, setpriority, SETPRIORITY, NOPTR, linux_sys_setpriority(ARG(0), ARG(1), ARG(2))) \
    X(135, personality, PERSONALITY, NOPTR, linux_sys_personality(ARG(0)))

LINUX_DEFINE_SYSCALLS(limits, LINUX_LIMITS_SYSCALLS)
