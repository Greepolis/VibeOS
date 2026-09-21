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
static int hw_signal_permitted(int target) {
    hw_task_t *me;

    if (g_current_task < 0 || target < 0 || target >= VIBEOS_HW_MAX_TASKS) {
        return 0;
    }
    me = &g_tasks[g_current_task];
    if (me->pid <= 1u) {
        return 1;
    }
    /* Its own thread group, always: a program may signal itself and its
     * threads whatever else is true. */
    if (g_tasks[target].tgid == me->tgid) {
        return 1;
    }
    return g_tasks[target].sid == me->sid;
}

static long hw_sys_kill(uint64_t target_pid, uint64_t sig) {
    int target;
    int delivered = 0;
    int64_t signed_pid = (int64_t)target_pid;
    long r;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if (sig > VIBEOS_HW_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    /* Every branch below resolves ids to slots and acts on them, so every one
     * holds g_sched_lock from the lookup to the act. See hw_task_by_pid. */
    if (sig == 0u) {
        if (signed_pid <= 0) {
            signed_pid = g_tasks[g_current_task].tgid;
        }
        /* Positive by construction after the line above, so the negate-if-
         * negative that used to be here could never run. CodeQL called it what
         * it was - a comparison whose result is always the same - and a dead
         * branch in a signal path is worth removing rather than explaining. */
        hw_spin_lock_named(&g_sched_lock, __func__);
        target = hw_task_by_pid((uint32_t)signed_pid);
        if (target < 0) {
            r = -VIBEOS_ESRCH;
        } else {
            r = hw_signal_permitted(target) ? 0 : -VIBEOS_EPERM;
        }
        hw_spin_unlock(&g_sched_lock);
        return r;
    }
    if (signed_pid < 0 || signed_pid == 0) {
        uint32_t group = signed_pid < 0 ? (uint32_t)(-signed_pid) : g_tasks[g_current_task].pgid;
        int i;
        hw_spin_lock_named(&g_sched_lock, __func__);
        for (i = 0; i < VIBEOS_HW_MAX_TASKS; i++) {
            if (g_tasks[i].is_user && g_tasks[i].state != HW_TASK_FREE &&
                g_tasks[i].state != HW_TASK_RESERVED &&
                g_tasks[i].pgid == group && g_tasks[i].sid == g_tasks[g_current_task].sid) {
                if (hw_signal_raise(i, (uint32_t)sig) == 0) {
                    delivered++;
                }
            }
        }
        hw_spin_unlock(&g_sched_lock);
        return delivered == 0 ? -VIBEOS_ESRCH : 0;
    }
    hw_spin_lock_named(&g_sched_lock, __func__);
    target = hw_task_by_pid((uint32_t)signed_pid);
    if (target < 0) {
        r = -VIBEOS_ESRCH;
    } else if (!hw_signal_permitted(target)) {
        /* EPERM and not ESRCH: the caller is being refused, not lied to about
         * whether the process exists. Hiding existence would be a different
         * decision, and this kernel's group branch does not make it either. */
        r = -VIBEOS_EPERM;
    } else {
        r = (hw_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
    }
    hw_spin_unlock(&g_sched_lock);
    return r;
}

static long hw_sys_tkill(uint64_t target_tid, uint64_t sig) {
    int target;
    long r;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user ||
        sig > VIBEOS_HW_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    /* Lookup, check and raise as one critical section (H-007). */
    hw_spin_lock_named(&g_sched_lock, __func__);
    target = hw_task_by_tid((uint32_t)target_tid);
    if (target >= 0 && !hw_signal_permitted(target)) {
        r = -VIBEOS_EPERM;   /* same rule as kill; see hw_signal_permitted */
    } else if (target < 0) {
        r = -VIBEOS_ESRCH;
    } else if (sig == 0u) {
        r = 0;
    } else {
        r = (hw_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
    }
    hw_spin_unlock(&g_sched_lock);
    if (sig == 0u || r != 0) {
        return r;
    }
    /* After the lock, not inside it: the console lock is never taken under
     * g_sched_lock by choice here. One line, one critical section: puts and
     * print_hex each take the console lock on their own. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[SIG] tkill tid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)(uint32_t)target_tid);
    vibeos_x86_64_serial_puts(" sig=0x");
    vibeos_x86_64_serial_print_hex(sig);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
    return 0;
}

static long hw_sys_tgkill(uint64_t target_tgid, uint64_t target_tid,
                          uint64_t sig) {
    int target;
    long r;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user ||
        sig > VIBEOS_HW_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    /* Lookup, identity check and raise as one critical section (H-007): the
     * tgid comparison protects nothing if the slot can change after it. */
    hw_spin_lock_named(&g_sched_lock, __func__);
    target = hw_task_by_tid((uint32_t)target_tid);
    if (target >= 0 && !hw_signal_permitted(target)) {
        r = -VIBEOS_EPERM;   /* same rule as kill; see hw_signal_permitted */
    } else if (target < 0 || g_tasks[target].tgid != (uint32_t)target_tgid) {
        r = -VIBEOS_ESRCH;
    } else if (sig == 0u) {
        r = 0;
    } else {
        r = (hw_signal_raise(target, (uint32_t)sig) == 0) ? 0 : -VIBEOS_EINVAL;
    }
    hw_spin_unlock(&g_sched_lock);
    if (sig == 0u || r != 0) {
        return r;
    }
    /* After the lock; see tkill. One line, one critical section: puts and
     * print_hex each take the console lock on their own. */
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[SIG] tgkill tgid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)(uint32_t)target_tgid);
    vibeos_x86_64_serial_puts(" tid=0x");
    vibeos_x86_64_serial_print_hex((uint64_t)(uint32_t)target_tid);
    vibeos_x86_64_serial_puts(" sig=0x");
    vibeos_x86_64_serial_print_hex(sig);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();
    return 0;
}

/* rt_sigaction(): install, or report, the disposition of one signal. */
static long hw_sys_rt_sigaction(uint64_t sig, uint64_t act_uptr, uint64_t old_uptr) {
    hw_task_t *t;

    if (g_current_task < 0 || sig == 0u || sig > VIBEOS_HW_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    if (sig == VIBEOS_SIGKILL || sig == VIBEOS_SIGSTOP) {
        return -VIBEOS_EINVAL;   /* neither can be caught */
    }
    t = &g_tasks[g_current_task];
    if (t->ps == 0) {
        return -VIBEOS_EINVAL;
    }

    /* struct sigaction: handler at 0, flags at 8, restorer at 16, mask at 24. */
    if (old_uptr != 0u) {
        if (!linux_user_ok(old_uptr, 32, 1)) {
            return -VIBEOS_EFAULT;
        }
        uint64_t old[4];
        old[0] = t->ps->sig_handler[sig];
        old[1] = t->ps->sig_flags[sig];
        old[2] = t->ps->sig_restorer[sig];
        old[3] = t->ps->sig_mask[sig] >> 1;
        /* Written through the fault-safe copy: a sibling munmap between the
         * range check and here would fault in ring 0 otherwise (H-016). */
        if (vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, old, sizeof(old)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    if (act_uptr != 0u) {
        uint64_t act[4];
        if (!linux_user_ok(act_uptr, 32, 0)) {
            return -VIBEOS_EFAULT;
        }
        if (vibeos_uaccess_copy(act, (const void *)(uintptr_t)act_uptr,
                                sizeof(act)) != 0) {
            return -VIBEOS_EFAULT;   /* H-016 */
        }
        /* The handler becomes frame->rip at delivery, and iretq to a
         * non-canonical rip faults in ring 0 - a kernel panic a program could
         * trigger with sigaction + a signal (H-017). A canonical handler that
         * is unmapped only faults in ring 3 and kills the task, so canonicality
         * and the user window are what must be checked. The two sentinels are
         * dispositions (default, ignore), not addresses. */
        if (act[0] != SIG_DFL_ADDR && act[0] != SIG_IGN_ADDR &&
            !hw_user_addr_ok(act[0])) {
            return -VIBEOS_EINVAL;
        }
        t->ps->sig_handler[sig] = act[0];
        t->ps->sig_flags[sig] = act[1];
        t->ps->sig_restorer[sig] = act[2];
        t->ps->sig_mask[sig] = act[3] << 1;
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
static uint64_t hw_sigset_from_user(uint64_t user_set) {
    return user_set << 1;
}

static uint64_t hw_sigset_to_user(uint64_t kernel_set) {
    return kernel_set >> 1;
}

static long hw_sys_rt_sigprocmask(uint64_t how, uint64_t set_uptr, uint64_t old_uptr) {
    hw_task_t *t;
    uint64_t set = 0;

    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];
    if (old_uptr != 0u) {
        if (!linux_user_ok(old_uptr, 8, 1)) {
            return -VIBEOS_EFAULT;
        }
        uint64_t out = hw_sigset_to_user(t->sig_blocked);
        if (vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, &out,
                                sizeof(out)) != 0) {
            return -VIBEOS_EFAULT;   /* H-016 */
        }
    }
    if (set_uptr == 0u) {
        return 0;
    }
    if (!linux_user_ok(set_uptr, 8, 0)) {
        return -VIBEOS_EFAULT;
    }
    {
        uint64_t raw;
        if (vibeos_uaccess_copy(&raw, (const void *)(uintptr_t)set_uptr,
                                sizeof(raw)) != 0) {
            return -VIBEOS_EFAULT;   /* H-016 */
        }
        set = hw_sigset_from_user(raw);
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

/* rt_sigreturn(): put back everything the handler interrupted. */
static long hw_sys_rt_sigreturn(vibeos_x86_64_isr_frame_t *frame) {
    hw_task_t *t;
    const hw_sigframe_t *sf;
    hw_sigframe_t kf;
    uint64_t base;

    if (g_current_task < 0) {
        return -VIBEOS_EINVAL;
    }
    t = &g_tasks[g_current_task];
    /* The handler has returned, so rsp points just past the return address
     * that the trampoline popped. */
    base = frame->rsp - 8ull + 8ull;
    if (!linux_user_ok(base, sizeof(hw_sigframe_t), 0)) {
        hw_task_exit(128ull + VIBEOS_SIGSEGV);
        return 0;
    }
    /* Copied into the kernel before anything is read: the range check and the
     * reads below are two instants, and a sibling munmap of the frame page in
     * between would fault in ring 0 (H-018). */
    if (vibeos_uaccess_copy(&kf, (const void *)(uintptr_t)base, sizeof(kf)) != 0) {
        hw_task_exit(128ull + VIBEOS_SIGSEGV);
        return 0;
    }
    sf = &kf;
    if (sf->magic != HW_SIGFRAME_MAGIC) {
        /* Somebody called rt_sigreturn without a frame, or overwrote it.
         * Resuming from whatever is on the stack would hand ring 3 a chance to
         * pick its own cs and rflags. */
        hw_task_exit(128ull + VIBEOS_SIGSEGV);
        return 0;
    }
    {
        vibeos_x86_64_isr_frame_t restored = sf->frame;
        t->sig_blocked = sf->blocked;
        /* Only user state is restored, and the segment selectors are forced
         * back to the user ones: the frame is in memory the program can write,
         * so nothing read out of it may decide privilege. */
        restored.cs = VIBEOS_HW_USER_CODE_SEL;
        restored.ss = VIBEOS_HW_USER_DATA_SEL;
        restored.rflags = (restored.rflags & 0x0000000000000CD5ull) | 0x202ull;
        /* rip comes from a user-writable frame; a non-canonical rip reaches
         * iretq and #GPs in ring 0 (H-018). cs/ss/rflags are forced above, so
         * this is the remaining ring-0 fault vector. rsp is left unchecked - a
         * bad rsp faults in ring 3 on the next push, killing the task safely. */
        if (!hw_user_addr_ok(restored.rip)) {
            hw_task_exit(128ull + VIBEOS_SIGSEGV);
            return 0;
        }
        *frame = restored;
    }
    return (long)frame->rax;
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 *   tkill   raise() goes through tkill, not kill: a library raising a signal in
 *           itself targets its own thread, and with one thread per process that is
 *           the same destination.
 *   tgkill  the thread named by tid, provided it still belongs to tgid - the check
 *           that stops a recycled thread id from reaching a different process. */
#define LINUX_SIG_SYSCALLS(X) \
    X(13,  rt_sigaction,   SIG_ACTION,   hw_sys_rt_sigaction(ARG(0), ARG(1), ARG(2))) \
    X(14,  rt_sigprocmask, SIG_PROCMASK, hw_sys_rt_sigprocmask(ARG(0), ARG(1), ARG(2))) \
    X(15,  rt_sigreturn,   SIG_RETURN,   hw_sys_rt_sigreturn(FRAME)) \
    X(62,  kill,           KILL,         hw_sys_kill(ARG(0), ARG(1))) \
    X(200, tkill,          TKILL,        hw_sys_tkill(ARG(0), ARG(1))) \
    X(234, tgkill,         TGKILL,       hw_sys_tgkill(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(sig, LINUX_SIG_SYSCALLS)
