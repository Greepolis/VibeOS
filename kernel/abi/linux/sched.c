/* Linux ABI: scheduling - policies and priorities, affinity, the CPU a thread
 * is on, I/O priorities and membarrier (docs/abi/ L6 step 2).
 *
 * Linux's policies map onto the scheduler's classes (kernel/sched/
 * sched_policy.c): SCHED_OTHER and SCHED_BATCH are NORMAL, weighted by nice;
 * SCHED_FIFO and SCHED_RR are REALTIME, ordered by their priority and above
 * every NORMAL task; SCHED_IDLE is NORMAL too - Linux gives it a weight so
 * small it rarely matters, and the strict IDLE class here is the cores' own
 * idle tasks, which a program must not be able to starve behind. What the
 * policy *was* is the thread's own word, kept in its identity, because that is
 * what sched_getscheduler must say. FIFO and RR are both time-sliced by the
 * REALTIME quantum: a FIFO thread is not kept on its core against an equal
 * one. SCHED_DEADLINE is refused - it needs admission control this kernel does
 * not have. */

#include "linux_internal.h"

/* The target of a call that names a thread by id: 0 for the caller, anything
 * negative EINVAL (the order Linux checks in), and ESRCH for nobody. */
static int sched_target(uint64_t pid, long *err) {
    int t;

    if (VIBEOS_ARG_INT(pid) < 0) {
        *err = -VIBEOS_EINVAL;
        return -1;
    }
    if (VIBEOS_ARG_INT(pid) == 0) {
        return ks_current();
    }
    ks_lock(ks_sched_lock(), __func__);
    t = ks_task_by_tid((uint32_t)pid);
    ks_unlock(ks_sched_lock());
    if (t < 0 || !ks_id(t)->is_user) {
        *err = -VIBEOS_ESRCH;
        return -1;
    }
    return t;
}

/* The thread id `pid` named, for the check made where a change is applied:
 * the slot sched_target found was found under the scheduler's lock, and the
 * lock let go before the change - an exit and a fork between the two put
 * another thread in the slot, and the change landed on it (M-116). A slot's
 * new tenant never has the old one's id. */
static uint32_t sched_tid(uint64_t pid, int t) {
    return VIBEOS_ARG_INT(pid) == 0 ? ks_id(t)->pid : (uint32_t)pid;
}

/* Whether the caller may change `t`'s scheduling: the superuser, or a caller
 * whose effective user is the target's real or effective one (Linux's
 * check_same_owner, which also stands for CAP_SYS_NICE here). */
static int sched_may_change(int t) {
    vibeos_cred_t me;
    const vibeos_procstate_t *ps = ks_ps(t);

    linux_cred(&me);
    return me.euid == 0u || (ps && (ps->cred.uid == me.euid || ps->cred.euid == me.euid));
}

static int sched_policy_ok(uint32_t policy) {
    return policy == LINUX_SCHED_NORMAL || policy == LINUX_SCHED_FIFO ||
           policy == LINUX_SCHED_RR || policy == LINUX_SCHED_BATCH ||
           policy == LINUX_SCHED_IDLE;
}

static int sched_is_rt(uint32_t policy) {
    return policy == LINUX_SCHED_FIFO || policy == LINUX_SCHED_RR;
}

/* Set a thread's policy and priority, after everything Linux checks: the
 * priority's range for the policy, who may change whom, and an unprivileged
 * caller's ceiling - RLIMIT_RTPRIO for a real-time priority, and never a way
 * out of reset-on-fork. */
static long sched_apply(int t, uint32_t tid, uint32_t policy, uint32_t prio, int reset_on_fork,
                        int keep_nice, int nice) {
    vibeos_cred_t me;
    const vibeos_procstate_t *mps = ks_current() >= 0 ? ks_ps(ks_current()) : 0;

    if (!sched_policy_ok(policy)) {
        return -VIBEOS_EINVAL;
    }
    if (sched_is_rt(policy) ? (prio < 1u || prio > 99u) : prio != 0u) {
        return -VIBEOS_EINVAL;
    }
    if (!sched_may_change(t)) {
        return -VIBEOS_EPERM;
    }
    linux_cred(&me);
    if (me.euid != 0u) {
        uint64_t rtlim = mps ? mps->rlim_cur[LINUX_RLIMIT_RTPRIO] : 0u;

        if (sched_is_rt(policy) && prio > ks_task_rt_prio(t) && (uint64_t)prio > rtlim) {
            return -VIBEOS_EPERM;
        }
        if (ks_id(t)->sched_reset_on_fork && !reset_on_fork) {
            return -VIBEOS_EPERM;
        }
        if (!keep_nice && nice < ks_task_nice(t)) {
            uint64_t allowance = mps ? mps->rlim_cur[LINUX_RLIMIT_NICE] : 0u;
            if (allowance == 0u || (uint64_t)(20 - nice) > allowance) {
                return -VIBEOS_EPERM;
            }
        }
    }
    /* Under the scheduler's lock: the picker reads class and priority under
     * it, on every core. */
    ks_lock(ks_sched_lock(), __func__);
    if (ks_id(t)->pid != tid) {
        ks_unlock(ks_sched_lock());
        return -VIBEOS_ESRCH;   /* gone since it was found, and the slot reused */
    }
    if (ks_task_set_class(t, sched_is_rt(policy) ? VIBEOS_SCHED_REALTIME : VIBEOS_SCHED_NORMAL,
                          prio) != 0) {
        ks_unlock(ks_sched_lock());
        return -VIBEOS_EINVAL;
    }
    if (!keep_nice) {
        (void)ks_task_set_nice(t, nice);
    }
    ks_unlock(ks_sched_lock());
    ks_id(t)->sched_policy = (uint8_t)policy;
    ks_id(t)->sched_reset_on_fork = reset_on_fork ? 1u : 0u;
    return 0;
}

static long linux_sys_sched_setscheduler(uint64_t pid, uint64_t policy, uint64_t uparam) {
    linux_sched_param_t p;
    long err = 0;
    uint32_t pol = (uint32_t)policy;
    int t;

    if (uparam == 0u || VIBEOS_ARG_INT(pid) < 0 || VIBEOS_ARG_INT(policy) < 0) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&p, (const void *)(uintptr_t)uparam, sizeof(p)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if ((t = sched_target(pid, &err)) < 0) {
        return err;
    }
    return sched_apply(t, sched_tid(pid, t), pol & ~(uint32_t)LINUX_SCHED_RESET_ON_FORK, (uint32_t)p.sched_priority,
                       (pol & LINUX_SCHED_RESET_ON_FORK) != 0u, 1, 0);
}

static long linux_sys_sched_setparam(uint64_t pid, uint64_t uparam) {
    linux_sched_param_t p;
    long err = 0;
    int t;

    if (uparam == 0u || VIBEOS_ARG_INT(pid) < 0) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&p, (const void *)(uintptr_t)uparam, sizeof(p)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if ((t = sched_target(pid, &err)) < 0) {
        return err;
    }
    return sched_apply(t, sched_tid(pid, t), ks_id(t)->sched_policy, (uint32_t)p.sched_priority,
                       ks_id(t)->sched_reset_on_fork, 1, 0);
}

static long linux_sys_sched_getscheduler(uint64_t pid) {
    long err = 0;
    int t = sched_target(pid, &err);

    if (t < 0) {
        return err;
    }
    return (long)ks_id(t)->sched_policy |
           (ks_id(t)->sched_reset_on_fork ? (long)LINUX_SCHED_RESET_ON_FORK : 0);
}

static long linux_sys_sched_getparam(uint64_t pid, uint64_t uparam) {
    linux_sched_param_t p;
    long err = 0;
    int t;

    if (uparam == 0u || VIBEOS_ARG_INT(pid) < 0) {
        return -VIBEOS_EINVAL;
    }
    if ((t = sched_target(pid, &err)) < 0) {
        return err;
    }
    p.sched_priority = (int32_t)ks_task_rt_prio(t);
    return vibeos_uaccess_copy((void *)(uintptr_t)uparam, &p, sizeof(p)) != 0 ? -VIBEOS_EFAULT : 0;
}

static long linux_sys_sched_get_priority_max(uint64_t policy) {
    uint32_t p = (uint32_t)policy;

    if (sched_is_rt(p)) {
        return 99;
    }
    return sched_policy_ok(p) || p == LINUX_SCHED_DEADLINE ? 0 : -VIBEOS_EINVAL;
}

static long linux_sys_sched_get_priority_min(uint64_t policy) {
    uint32_t p = (uint32_t)policy;

    if (sched_is_rt(p)) {
        return 1;
    }
    return sched_policy_ok(p) || p == LINUX_SCHED_DEADLINE ? 0 : -VIBEOS_EINVAL;
}

/* The slice a thread gets: the REALTIME quantum for SCHED_RR, none for FIFO
 * (it runs until it yields, as far as Linux promises), the NORMAL quantum for
 * the rest. */
static long linux_sys_sched_rr_get_interval(uint64_t pid, uint64_t uts) {
    linux_timespec_t ts;
    uint64_t ticks;
    long err = 0;
    int t = sched_target(pid, &err);

    if (t < 0) {
        return err;
    }
    switch (ks_id(t)->sched_policy) {
        case LINUX_SCHED_FIFO: ticks = 0; break;
        case LINUX_SCHED_RR:   ticks = ks_class_quantum(VIBEOS_SCHED_REALTIME); break;
        default:               ticks = ks_class_quantum(VIBEOS_SCHED_NORMAL); break;
    }
    ts.tv_sec = (int64_t)(ticks / ks_hz());
    ts.tv_nsec = (int64_t)((ticks % ks_hz()) * (1000000000ull / ks_hz()));
    return vibeos_uaccess_copy((void *)(uintptr_t)uts, &ts, sizeof(ts)) != 0 ? -VIBEOS_EFAULT : 0;
}

/* sched_setattr and sched_getattr: the same, through a structure that grows.
 * Linux's rules for its size: 0 is the first version, smaller than that is
 * E2BIG with the size Linux knows written back, larger is fine if everything
 * past what Linux knows is zero. */
static long linux_sys_sched_setattr(uint64_t pid, uint64_t uattr, uint64_t flags) {
    linux_sched_attr_t a;
    uint32_t size = 0, k;
    long err = 0;
    int t;

    if (uattr == 0u || VIBEOS_ARG_INT(pid) < 0 || flags != 0u) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&size, (const void *)(uintptr_t)uattr, 4u) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (size == 0u) {
        size = LINUX_SCHED_ATTR_SIZE_VER0;
    }
    if (size < LINUX_SCHED_ATTR_SIZE_VER0 || size > 4096u) {
        uint32_t known = (uint32_t)sizeof(a);
        (void)vibeos_uaccess_copy((void *)(uintptr_t)uattr, &known, 4u);
        return -VIBEOS_E2BIG;
    }
    /* The whole of what the size says, judged before any of it is read: its
     * length is in the structure, so the row can declare only the first word. */
    if (!linux_user_ok(uattr, size, 0)) {
        return -VIBEOS_EFAULT;
    }
    for (k = 0; k < sizeof(a); k++) {
        ((uint8_t *)&a)[k] = 0;
    }
    if (vibeos_uaccess_copy(&a, (const void *)(uintptr_t)uattr,
                            size < sizeof(a) ? size : sizeof(a)) != 0) {
        return -VIBEOS_EFAULT;
    }
    for (k = (uint32_t)sizeof(a); k < size; k++) {
        uint8_t c;
        if (vibeos_uaccess_copy(&c, (const void *)(uintptr_t)(uattr + k), 1u) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (c != 0u) {
            uint32_t known = (uint32_t)sizeof(a);
            (void)vibeos_uaccess_copy((void *)(uintptr_t)uattr, &known, 4u);
            return -VIBEOS_E2BIG;
        }
    }
    if ((t = sched_target(pid, &err)) < 0) {
        return err;
    }
    if (a.sched_flags & ~(uint64_t)LINUX_SCHED_FLAG_ALL) {
        return -VIBEOS_EINVAL;
    }
    if (a.sched_flags & (LINUX_SCHED_FLAG_UTIL_CLAMP_MIN | LINUX_SCHED_FLAG_UTIL_CLAMP_MAX)) {
        return -VIBEOS_EOPNOTSUPP;   /* Linux without CONFIG_UCLAMP_TASK */
    }
    if (a.sched_policy == LINUX_SCHED_DEADLINE) {
        return -VIBEOS_EINVAL;
    }
    if (a.sched_flags & LINUX_SCHED_FLAG_KEEP_POLICY) {
        a.sched_policy = ks_id(t)->sched_policy;
    }
    if (a.sched_flags & LINUX_SCHED_FLAG_KEEP_PARAMS) {
        a.sched_priority = ks_task_rt_prio(t);
    }
    if (a.sched_nice < -20 || a.sched_nice > 19) {
        return -VIBEOS_EINVAL;
    }
    return sched_apply(t, sched_tid(pid, t), a.sched_policy, a.sched_priority,
                       (a.sched_flags & LINUX_SCHED_FLAG_RESET_ON_FORK) != 0u,
                       sched_is_rt(a.sched_policy), a.sched_nice);
}

static long linux_sys_sched_getattr(uint64_t pid, uint64_t uattr, uint64_t usize, uint64_t flags) {
    linux_sched_attr_t a;
    uint32_t k, n;
    long err = 0;
    int t;

    if (uattr == 0u || VIBEOS_ARG_INT(pid) < 0 || flags != 0u ||
        usize < LINUX_SCHED_ATTR_SIZE_VER0 || usize > 4096u) {
        return -VIBEOS_EINVAL;
    }
    if ((t = sched_target(pid, &err)) < 0) {
        return err;
    }
    for (k = 0; k < sizeof(a); k++) {
        ((uint8_t *)&a)[k] = 0;
    }
    n = usize < sizeof(a) ? (uint32_t)usize : (uint32_t)sizeof(a);
    a.size = n;
    a.sched_policy = ks_id(t)->sched_policy;
    a.sched_flags = ks_id(t)->sched_reset_on_fork ? LINUX_SCHED_FLAG_RESET_ON_FORK : 0u;
    a.sched_nice = ks_task_nice(t);
    a.sched_priority = ks_task_rt_prio(t);
    a.sched_util_max = 1024u;   /* SCHED_CAPACITY_SCALE: no clamp */
    if (!linux_user_ok(uattr, n, 1)) {
        return -VIBEOS_EFAULT;   /* as long as the caller said, past the row's first word */
    }
    return vibeos_uaccess_copy((void *)(uintptr_t)uattr, &a, n) != 0 ? -VIBEOS_EFAULT : 0;
}

/* ---- affinity and the CPU a thread is on ----------------------------------- */

/* Linux's mask is as long as its CPU count rounded up to a long: eight bytes
 * here, whatever the caller's buffer. A thread that may run anywhere has an
 * empty mask in the scheduler and reports every core that is online. */
static long linux_sys_sched_setaffinity(uint64_t pid, uint64_t len, uint64_t umask) {
    uint64_t mask = 0;
    long err = 0;
    int t;

    /* As much of the caller's mask as Linux's is long, and no more: the rest
     * of a longer buffer is never read, so it need not be there. */
    if (!linux_user_ok(umask, len < 8u ? len : 8u, 0) ||
        vibeos_uaccess_copy(&mask, (const void *)(uintptr_t)umask, len < 8u ? len : 8u) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (VIBEOS_ARG_INT(pid) < 0) {
        return -VIBEOS_ESRCH;   /* Linux looks a negative id up, and finds nobody */
    }
    if ((t = sched_target(pid, &err)) < 0) {
        return err;
    }
    if (!sched_may_change(t)) {
        return -VIBEOS_EPERM;
    }
    mask &= ks_cpus_online_mask();
    if (mask == 0u) {
        return -VIBEOS_EINVAL;
    }
    ks_lock(ks_sched_lock(), __func__);
    if (ks_id(t)->pid != sched_tid(pid, t)) {
        err = -VIBEOS_ESRCH;   /* gone since it was found, and the slot reused (M-116) */
    } else {
        err = ks_task_set_affinity(t, mask == ks_cpus_online_mask() ? 0u : (uint32_t)mask) == 0
                  ? 0 : -VIBEOS_EINVAL;
    }
    ks_unlock(ks_sched_lock());
    /* A caller that has just been told it may not run here is not running
     * here when the call returns, as on Linux - LTP's getcpu01 pins itself and
     * asks at once. Each tick gives the pickers a turn, and the one on this
     * core no longer takes it. Bounded: a second is a hundred of them. */
    if (err == 0 && t == ks_current()) {
        uint32_t ticks;

        for (ticks = 0; ticks < 100u && !(mask & (1ull << ks_cpu_id())); ticks++) {
            ks_idle();
        }
        if (!(mask & (1ull << ks_cpu_id()))) {
            /* A second and still not there. THREADS.ELF's C6_SCHED saw a child
             * pinned to core 1 miss it for three seconds, once in about
             * eighteen boots, with nothing else in the log (docs/abi/ L7). Say
             * what each allowed core was running - its slot, pid and class -
             * in one line each: whatever kept this task off is on one of them.
             *
             * What it has said so far (2026-10-12/13, about thirty boots): the
             * allowed core running another user task, NORMAL or IDLE class,
             * on most boots - so the task was released here at the end of its
             * slice and waited in that core's queue behind a busy boot's other
             * work. A latency, then, and open, not a refusal (the review
             * tracker's "Found internally"). INFO, not WARN: it is evidence on
             * every boot, and a boot must not fail for a wait the call already
             * bounds. */
            uint64_t a0[4], a1[4];
            uint32_t cpu, n = 0, i;

            /* Read under the scheduler's lock, said after it: the log takes the
             * console's, and that one is not to be taken inside this one. */
            ks_lock(ks_sched_lock(), __func__);
            for (cpu = 0; cpu < 32u && n < 4u; cpu++) {
                if (mask & (1ull << cpu)) {
                    int s = ks_cpu_running(cpu);

                    a0[n] = ((uint64_t)cpu << 32) | (uint32_t)s;
                    a1[n++] = s >= 0 ? (((uint64_t)ks_task_class(s) << 32) | ks_id(s)->pid) : ~0ull;
                }
            }
            ks_unlock(ks_sched_lock());
            for (i = 0; i < n; i++) {
                ks_log(VIBEOS_LOG_INFO, 63u, a0[i], a1[i],
                       "sched_setaffinity: still off the allowed core; a0=cpu<<32|slot it runs, a1=class<<32|pid");
            }
        }
    }
    return err;
}

static long linux_sys_sched_getaffinity(uint64_t pid, uint64_t len, uint64_t umask) {
    uint64_t mask;
    long err = 0;
    int t;

    if (len < 8u || (len & 7u) != 0u) {
        return -VIBEOS_EINVAL;
    }
    if (VIBEOS_ARG_INT(pid) < 0) {
        return -VIBEOS_ESRCH;
    }
    if ((t = sched_target(pid, &err)) < 0) {
        return err;
    }
    mask = ks_task_affinity(t);
    mask = mask ? (mask & ks_cpus_online_mask()) : ks_cpus_online_mask();
    if (vibeos_uaccess_copy((void *)(uintptr_t)umask, &mask, 8u) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 8;
}

/* One node; the core is whichever this call is running on, which is all Linux
 * promises too - it may have moved by the time the caller looks. */
static long linux_sys_getcpu(uint64_t ucpu, uint64_t unode) {
    uint32_t cpu = ks_cpu_id(), node = 0;

    if (ucpu != 0u && vibeos_uaccess_copy((void *)(uintptr_t)ucpu, &cpu, 4u) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (unode != 0u && vibeos_uaccess_copy((void *)(uintptr_t)unode, &node, 4u) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* ---- I/O priorities -------------------------------------------------------------
 *
 * Kept and reported, as Linux does on a disk with no I/O scheduler that reads
 * them: there is one queue here and every request waits in it alike. What is
 * refused is what Linux refuses - a class it does not have, a level past seven,
 * the real-time class to anybody but the superuser. Per thread, like nice. */

static long ioprio_check(uint32_t prio) {
    /* The whole data field, not its low three bits: a level of 8 is refused
     * (LTP's ioprio_set03), whatever newer kernels call the bits above. */
    uint32_t cls = prio >> LINUX_IOPRIO_CLASS_SHIFT, level = prio & 0x1FFFu;
    vibeos_cred_t me;

    switch (cls) {
        case LINUX_IOPRIO_CLASS_RT:
            linux_cred(&me);
            if (me.euid != 0u) {
                return -VIBEOS_EPERM;
            }
            /* fallthrough */
        case LINUX_IOPRIO_CLASS_BE:
            return level >= 8u ? -VIBEOS_EINVAL : 0;
        case LINUX_IOPRIO_CLASS_IDLE:
            return 0;
        case LINUX_IOPRIO_CLASS_NONE:
            return level != 0u ? -VIBEOS_EINVAL : 0;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* Whether slot `i` is one of the tasks `which`/`who` name: a thread by id,
 * a process group, or every task of a user. */
static int ioprio_names(int i, uint32_t which, uint32_t who, const vibeos_cred_t *me) {
    const vibeos_task_t *id = ks_id(i);
    const vibeos_procstate_t *ps;

    if (!id || !id->is_user || vibeos_task_state((uint32_t)i) == VIBEOS_TASK_FREE) {
        return 0;
    }
    switch (which) {
        case LINUX_IOPRIO_WHO_PROCESS:
            return who == 0u ? i == ks_current() : id->pid == who;
        case LINUX_IOPRIO_WHO_PGRP:
            return id->pgid == (who == 0u ? ks_id(ks_current())->pgid : who);
        case LINUX_IOPRIO_WHO_USER:
            ps = ks_ps(i);
            return ps && ps->cred.uid == (who == 0u ? me->uid : who);
        default:
            return 0;
    }
}

static long linux_sys_ioprio_set(uint64_t which, uint64_t who, uint64_t ioprio) {
    vibeos_cred_t me;
    int found = 0;
    long r;
    uint32_t i;

    if ((uint32_t)which < LINUX_IOPRIO_WHO_PROCESS || (uint32_t)which > LINUX_IOPRIO_WHO_USER) {
        return -VIBEOS_EINVAL;
    }
    if ((r = ioprio_check((uint32_t)ioprio)) != 0) {
        return r;
    }
    linux_cred(&me);
    ks_lock(ks_sched_lock(), __func__);
    for (i = 0; i < ks_slots(); i++) {
        if (!ioprio_names((int)i, (uint32_t)which, (uint32_t)who, &me)) {
            continue;
        }
        found = 1;
        if (!sched_may_change((int)i)) {
            r = -VIBEOS_EPERM;
            continue;
        }
        ks_id((int)i)->ioprio = (uint16_t)ioprio;
    }
    ks_unlock(ks_sched_lock());
    return found ? r : -VIBEOS_ESRCH;
}

/* What was set, NONE included - a task that set NONE reads NONE back (LTP's
 * ioprio_set02), and so does one that never set anything. Of several, the
 * best: the lowest class, then the lowest level, NONE counting as the
 * best-effort level its nice gives, which is what it means to the disk. */
static long linux_sys_ioprio_get(uint64_t which, uint64_t who) {
    vibeos_cred_t me;
    long best = -1, best_rank = 0;
    uint32_t i;

    if ((uint32_t)which < LINUX_IOPRIO_WHO_PROCESS || (uint32_t)which > LINUX_IOPRIO_WHO_USER) {
        return -VIBEOS_EINVAL;
    }
    linux_cred(&me);
    ks_lock(ks_sched_lock(), __func__);
    for (i = 0; i < ks_slots(); i++) {
        long p;

        if (!ioprio_names((int)i, (uint32_t)which, (uint32_t)who, &me)) {
            continue;
        }
        long rank;

        p = (long)ks_id((int)i)->ioprio;
        rank = p;
        if ((p >> LINUX_IOPRIO_CLASS_SHIFT) == LINUX_IOPRIO_CLASS_NONE) {
            rank = ((long)LINUX_IOPRIO_CLASS_BE << LINUX_IOPRIO_CLASS_SHIFT) |
                   (long)((ks_task_nice((int)i) + 20) / 5);
        }
        if (best < 0 || rank < best_rank) {
            best = p;
            best_rank = rank;
        }
    }
    ks_unlock(ks_sched_lock());
    return best < 0 ? -VIBEOS_ESRCH : best;
}

/* ---- membarrier -----------------------------------------------------------------
 *
 * A barrier on the other cores, so that a program can make its fast path free
 * of fences and pay for them here instead. Every command but the RSEQ pair,
 * which needs restartable sequences (rseq is ENOSYS). The expedited private
 * commands need the process to have registered first, as Linux asks; the
 * registration is the process's and an exec forgets it (a new process state). */

#define MEMBARRIER_SUPPORTED (LINUX_MEMBARRIER_CMD_GLOBAL | LINUX_MEMBARRIER_CMD_GLOBAL_EXPEDITED | \
                              LINUX_MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED | \
                              LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED | \
                              LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED | \
                              LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE | \
                              LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE)

static long linux_sys_membarrier(uint64_t cmd, uint64_t flags, uint64_t cpu) {
    vibeos_procstate_t *ps = ks_current() >= 0 ? ks_ps(ks_current()) : 0;
    uint32_t c = (uint32_t)cmd;

    (void)cpu;
    if (flags != 0u || !ps) {
        return -VIBEOS_EINVAL;
    }
    switch (c) {
        case LINUX_MEMBARRIER_CMD_QUERY:
            return MEMBARRIER_SUPPORTED;
        case LINUX_MEMBARRIER_CMD_GET_REGISTRATIONS:
            return (long)ps->membarrier_reg;
        case LINUX_MEMBARRIER_CMD_GLOBAL:
        case LINUX_MEMBARRIER_CMD_GLOBAL_EXPEDITED:
            ks_membarrier(1);
            return 0;
        case LINUX_MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED:
        case LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED:
        case LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE:
            __atomic_or_fetch(&ps->membarrier_reg, c, __ATOMIC_RELAXED);
            return 0;
        case LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED:
            if (!(ps->membarrier_reg & LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED)) {
                return -VIBEOS_EPERM;
            }
            ks_membarrier(0);
            return 0;
        case LINUX_MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE:
            if (!(ps->membarrier_reg & LINUX_MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE)) {
                return -VIBEOS_EPERM;
            }
            ks_membarrier(0);   /* the interrupt that is the barrier serialises the core too */
            return 0;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* ---- what a new task inherits ------------------------------------------------------
 *
 * Its creator's policy, priority, nice, affinity and I/O priority, for a thread
 * and for a process alike - unless the creator asked for reset-on-fork, which a
 * new *process* honours, as Linux's does for both: a real-time policy becomes
 * SCHED_OTHER, a negative nice 0, and the flag does not pass on. Under the
 * scheduler's lock, as the picker reads what this writes. */
void linux_sched_inherit(int child, int parent) {
    const vibeos_task_t *p = ks_id(parent);
    vibeos_task_t *c = ks_id(child);
    uint32_t policy = p->sched_policy, prio = ks_task_rt_prio(parent);
    int nice = ks_task_nice(parent);

    if (p->sched_reset_on_fork) {
        if (sched_is_rt(policy)) {
            policy = LINUX_SCHED_NORMAL;
            prio = 0;
        }
        if (nice < 0) {
            nice = 0;
        }
    }
    c->sched_policy = (uint8_t)policy;
    c->sched_reset_on_fork = 0;
    c->ioprio = p->ioprio;
    ks_lock(ks_sched_lock(), __func__);
    (void)ks_task_set_class(child, sched_is_rt(policy) ? VIBEOS_SCHED_REALTIME : VIBEOS_SCHED_NORMAL,
                            prio);
    (void)ks_task_set_nice(child, nice);
    (void)ks_task_set_affinity(child, ks_task_affinity(parent));
    ks_unlock(ks_sched_lock());
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_SCHED_SYSCALLS(X) \
    X(142, sched_setparam,          SCHED_SET,      PTRS(IN_OPT(1, 4)), linux_sys_sched_setparam(ARG(0), ARG(1))) \
    X(143, sched_getparam,          SCHED_GET,      PTRS(OUT_OPT(1, 4)), linux_sys_sched_getparam(ARG(0), ARG(1))) \
    X(144, sched_setscheduler,      SCHED_SET,      PTRS(IN_OPT(2, 4)), linux_sys_sched_setscheduler(ARG(0), ARG(1), ARG(2))) \
    X(145, sched_getscheduler,      SCHED_POLICY,   NOPTR, linux_sys_sched_getscheduler(ARG(0))) \
    X(146, sched_get_priority_max,  SCHED_POLICY,   NOPTR, linux_sys_sched_get_priority_max(ARG(0))) \
    X(147, sched_get_priority_min,  SCHED_POLICY,   NOPTR, linux_sys_sched_get_priority_min(ARG(0))) \
    X(148, sched_rr_get_interval,   SCHED_GET,      PTRS(OUT(1, 16)), linux_sys_sched_rr_get_interval(ARG(0), ARG(1))) \
    X(203, sched_setaffinity,       AFFINITY,       NOPTR, linux_sys_sched_setaffinity(ARG(0), ARG(1), ARG(2))) \
    X(204, sched_getaffinity,       AFFINITY,       PTRS(OUT(2, 8)), linux_sys_sched_getaffinity(ARG(0), ARG(1), ARG(2))) \
    X(251, ioprio_set,              IOPRIO,         NOPTR, linux_sys_ioprio_set(ARG(0), ARG(1), ARG(2))) \
    X(252, ioprio_get,              IOPRIO,         NOPTR, linux_sys_ioprio_get(ARG(0), ARG(1))) \
    X(309, getcpu,                  GETCPU,         PTRS(OUT_OPT(0, 4), OUT_OPT(1, 4)), linux_sys_getcpu(ARG(0), ARG(1))) \
    X(314, sched_setattr,           SCHED_SET,      PTRS(IN_OPT(1, 4)), linux_sys_sched_setattr(ARG(0), ARG(1), ARG(2))) \
    X(315, sched_getattr,           SCHED_GET,      PTRS(OUT_OPT(1, 4)), linux_sys_sched_getattr(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(324, membarrier,              MEMBARRIER,     NOPTR, linux_sys_membarrier(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(sched, LINUX_SCHED_SYSCALLS)
