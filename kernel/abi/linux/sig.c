/* Linux ABI: sending signals and the signal mask.
 *
 * Lifted out of arch_hw.c (C4 stage 3). Nothing here is new: the handlers and the
 * helpers only they use, moved as they were. */

#include "linux_internal.h"

/* May the calling task send a signal to `target`?
 *
 * The rule was already here and applied to half the cases. kill's
 * process-group branch filtered on `sid == the caller's sid`; its single-pid
 * branch, and tkill and tgkill, checked nothing at all - so any unprivileged
 * program could SIGKILL any process on the machine by pid: another session's
 * shell, or a supervised service, repeatedly, until init's restart budget was
 * spent and the service was permanently down.
 *
 * Written as one function rather than three copies of a condition, for the
 * reason the fork guard ended up with one entry point: the version with the
 * check in some places and not others is exactly what shipped.
 *
 * Session, not parent, because that is the relationship this kernel already
 * uses to decide the same question for a group, and inventing a second rule
 * would mean two answers to "may I signal this".
 *
 * init and the kernel are exempt: init supervises services and stopping them is
 * its job. */
static int linux_signal_permitted(int target) {
    const vibeos_task_t *me;

    if (ks_current() < 0 || target < 0 || target >= (int)ks_slots()) {
        return 0;
    }
    me = ks_id(ks_current());
    if (me->pid <= 1u) {
        return 1;
    }
    /* Its own thread group, always: a program may signal itself and its
     * threads whatever else is true. */
    if (ks_id(target)->tgid == me->tgid) {
        return 1;
    }
    return ks_id(target)->sid == me->sid;
}

static long linux_sys_kill(uint64_t target_pid, uint64_t sig) {
    int target;
    int delivered = 0;
    int64_t signed_pid = (int64_t)target_pid;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (sig > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    /* Every branch below resolves ids to slots and acts on them, so every one
     * holds g_sched_lock from the lookup to the act. See ks_task_by_pid. */
    if (sig == 0u) {
        if (signed_pid <= 0) {
            signed_pid = ks_id(ks_current())->tgid;
        }
        /* Positive by construction after the line above, so the negate-if-
         * negative that used to be here could never run. CodeQL called it what
         * it was - a comparison whose result is always the same - and a dead
         * branch in a signal path is worth removing rather than explaining. */
        ks_lock(ks_sched_lock(), __func__);
        target = ks_task_by_pid((uint32_t)signed_pid);
        if (target < 0) {
            r = -VIBEOS_ESRCH;
        } else {
            r = linux_signal_permitted(target) ? 0 : -VIBEOS_EPERM;
        }
        ks_unlock(ks_sched_lock());
        return r;
    }
    if (signed_pid < 0 || signed_pid == 0) {
        uint32_t group = signed_pid < 0 ? (uint32_t)(-signed_pid) : ks_id(ks_current())->pgid;
        int i;
        ks_lock(ks_sched_lock(), __func__);
        for (i = 0; i < (int)ks_slots(); i++) {
            if (ks_id(i)->is_user && vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_FREE &&
                vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_SETUP &&
                ks_id(i)->pgid == group && ks_id(i)->sid == ks_id(ks_current())->sid) {
                if (ks_signal_raise(i, (uint32_t)sig) == 0) {
                    delivered++;
                }
            }
        }
        ks_unlock(ks_sched_lock());
        return delivered == 0 ? -VIBEOS_ESRCH : 0;
    }
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_pid((uint32_t)signed_pid);
    if (target < 0) {
        r = -VIBEOS_ESRCH;
    } else if (!linux_signal_permitted(target)) {
        /* EPERM and not ESRCH: the caller is being refused, not lied to about
         * whether the process exists. Hiding existence would be a different
         * decision, and this kernel's group branch does not make it either. */
        r = -VIBEOS_EPERM;
    } else {
        r = (ks_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
    }
    ks_unlock(ks_sched_lock());
    return r;
}

static long linux_sys_tkill(uint64_t target_tid, uint64_t sig) {
    int target;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user ||
        sig > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    /* Lookup, check and raise as one critical section (H-007). */
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_tid((uint32_t)target_tid);
    if (target >= 0 && !linux_signal_permitted(target)) {
        r = -VIBEOS_EPERM;   /* same rule as kill; see linux_signal_permitted */
    } else if (target < 0) {
        r = -VIBEOS_ESRCH;
    } else if (sig == 0u) {
        r = 0;
    } else {
        r = (ks_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
    }
    ks_unlock(ks_sched_lock());
    if (sig == 0u || r != 0) {
        return r;
    }
    /* After the lock, not inside it: the console lock is never taken under
     * g_sched_lock by choice here. One line, one critical section: puts and
     * print_hex each take the console lock on their own. */
    ks_con_lock();
    ks_con_puts("[SIG] tkill tid=0x");
    ks_con_hex((uint64_t)(uint32_t)target_tid);
    ks_con_puts(" sig=0x");
    ks_con_hex(sig);
    ks_con_puts("\n");
    ks_con_unlock();
    return 0;
}

static long linux_sys_tgkill(uint64_t target_tgid, uint64_t target_tid,
                          uint64_t sig) {
    int target;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user ||
        sig > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    /* Lookup, identity check and raise as one critical section (H-007): the
     * tgid comparison protects nothing if the slot can change after it. */
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_tid((uint32_t)target_tid);
    if (target >= 0 && !linux_signal_permitted(target)) {
        r = -VIBEOS_EPERM;   /* same rule as kill; see linux_signal_permitted */
    } else if (target < 0 || ks_id(target)->tgid != (uint32_t)target_tgid) {
        r = -VIBEOS_ESRCH;
    } else if (sig == 0u) {
        r = 0;
    } else {
        r = (ks_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
    }
    ks_unlock(ks_sched_lock());
    if (sig == 0u || r != 0) {
        return r;
    }
    /* After the lock; see tkill. One line, one critical section: puts and
     * print_hex each take the console lock on their own. */
    ks_con_lock();
    ks_con_puts("[SIG] tgkill tgid=0x");
    ks_con_hex((uint64_t)(uint32_t)target_tgid);
    ks_con_puts(" tid=0x");
    ks_con_hex((uint64_t)(uint32_t)target_tid);
    ks_con_puts(" sig=0x");
    ks_con_hex(sig);
    ks_con_puts("\n");
    ks_con_unlock();
    return 0;
}

/* rt_sigaction(): install, or report, the disposition of one signal. */
static long linux_sys_rt_sigaction(uint64_t sig, uint64_t act_uptr, uint64_t old_uptr) {
    vibeos_procstate_t *ps;

    if (ks_current() < 0 || sig == 0u || sig > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    if (sig == VIBEOS_SIGKILL || sig == VIBEOS_SIGSTOP) {
        return -VIBEOS_EINVAL;   /* neither can be caught */
    }
    ps = ks_ps(ks_current());
    if (ps == 0) {
        return -VIBEOS_EINVAL;
    }

    /* struct sigaction: handler at 0, flags at 8, restorer at 16, mask at 24. */
    if (old_uptr != 0u) {
        uint64_t old[4];
        old[0] = ps->sig_handler[sig];
        old[1] = ps->sig_flags[sig];
        old[2] = ps->sig_restorer[sig];
        old[3] = ps->sig_mask[sig] >> 1;
        /* Written through the fault-safe copy: a sibling munmap between the
         * range check and here would fault in ring 0 otherwise (H-016). */
        if (vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, old, sizeof(old)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    if (act_uptr != 0u) {
        uint64_t act[4];
        if (vibeos_uaccess_copy(act, (const void *)(uintptr_t)act_uptr,
                                sizeof(act)) != 0) {
            return -VIBEOS_EFAULT;   /* H-016 */
        }
        /* The handler becomes the resume address at delivery, and iretq to a
         * non-canonical rip faults in ring 0 - a kernel panic a program could
         * trigger with sigaction + a signal (H-017). A canonical handler that
         * is unmapped only faults in ring 3 and kills the task, so canonicality
         * and the user window are what must be checked. The two sentinels are
         * dispositions (default, ignore), not addresses. */
        if (act[0] != SIG_DFL_ADDR && act[0] != SIG_IGN_ADDR &&
            !ks_user_addr_ok(act[0])) {
            return -VIBEOS_EINVAL;
        }
        ps->sig_handler[sig] = act[0];
        ps->sig_flags[sig] = act[1];
        ps->sig_restorer[sig] = act[2];
        ps->sig_mask[sig] = act[3] << 1;
    }
    return 0;
}

/* rt_sigprocmask(): SIG_BLOCK 0, SIG_UNBLOCK 1, SIG_SETMASK 2. */
/* A Linux sigset_t numbers its bits from zero: bit 0 is signal 1. This kernel
 * numbers them by signal, so bit 9 is signal 9, which keeps every comparison
 * against a signal constant readable. The two representations are converted at
 * the boundary, and only here - getting it wrong shifts every mask by one, so
 * a program blocking SIGUSR2 actually blocks SIGSEGV and its own blocked
 * signal is delivered anyway. */
static uint64_t linux_sigset_from_user(uint64_t user_set) {
    return user_set << 1;
}

static uint64_t linux_sigset_to_user(uint64_t kernel_set) {
    return kernel_set >> 1;
}

static long linux_sys_rt_sigprocmask(uint64_t how, uint64_t set_uptr, uint64_t old_uptr) {
    vibeos_task_t *t;
    uint64_t set = 0;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    t = ks_id(ks_current());
    if (old_uptr != 0u) {
        uint64_t out = linux_sigset_to_user(t->sig_blocked);
        if (vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, &out,
                                sizeof(out)) != 0) {
            return -VIBEOS_EFAULT;   /* H-016 */
        }
    }
    if (set_uptr == 0u) {
        return 0;
    }
    {
        uint64_t raw;
        if (vibeos_uaccess_copy(&raw, (const void *)(uintptr_t)set_uptr,
                                sizeof(raw)) != 0) {
            return -VIBEOS_EFAULT;   /* H-016 */
        }
        set = linux_sigset_from_user(raw);
    }
    switch (how) {
        case 0: t->sig_blocked |= set; break;
        case 1: t->sig_blocked &= ~set; break;
        case 2: t->sig_blocked = set; break;
        default: return -VIBEOS_EINVAL;
    }
    /* Blocking these would make a process unkillable, so the request is
     * accepted and the two bits are dropped, exactly as Linux does. */
    t->sig_blocked &= ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    return 0;
}

/* rt_sigreturn(): put back everything the handler interrupted.
 *
 * The frame is in memory the program can write, so the architecture reads it
 * back and decides what of it may be trusted - the privilege-bearing registers
 * are forced, the resume address is checked (H-018). What is left here is the
 * policy: a frame that is missing, unreadable or forged ends the task with
 * SIGSEGV rather than resuming from whatever is on the stack. */
static long linux_sys_rt_sigreturn(ks_regs_t *frame) {
    uint64_t base, blocked = 0;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    /* The handler has returned, so rsp points just past the return address
     * that the trampoline popped. */
    base = ks_regs_sp(frame);
    if (!linux_user_ok(base, ks_sigframe_size(), 0) ||
        ks_sigframe_pop(frame, base, &blocked) != 0) {
        ks_task_exit(128ull + VIBEOS_SIGSEGV);
        return 0;
    }
    ks_id(ks_current())->sig_blocked = blocked;
    return (long)ks_regs_ret(frame);
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 *   tkill   raise() goes through tkill, not kill: a library raising a signal in
 *           itself targets its own thread, and with one thread per process that is
 *           the same destination.
 *   tgkill  the thread named by tid, provided it still belongs to tgid - the check
 *           that stops a recycled thread id from reaching a different process. */
#define LINUX_SIG_SYSCALLS(X) \
    X(13,  rt_sigaction,   SIG_ACTION,   PTRS(OUT_OPT(2, 32), IN_OPT(1, 32)), linux_sys_rt_sigaction(ARG(0), ARG(1), ARG(2))) \
    X(14,  rt_sigprocmask, SIG_PROCMASK, PTRS(OUT_OPT(2, 8), IN_OPT(1, 8)), linux_sys_rt_sigprocmask(ARG(0), ARG(1), ARG(2))) \
    X(15,  rt_sigreturn,   SIG_RETURN,   NOPTR, linux_sys_rt_sigreturn(FRAME)) \
    X(62,  kill,           KILL,         NOPTR, linux_sys_kill(ARG(0), ARG(1))) \
    X(200, tkill,          TKILL,        NOPTR, linux_sys_tkill(ARG(0), ARG(1))) \
    X(234, tgkill,         TGKILL,       NOPTR, linux_sys_tgkill(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(sig, LINUX_SIG_SYSCALLS)
