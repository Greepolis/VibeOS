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
 * The answer was "the same session" until processes had owners, and that rule
 * is not Linux's: BusyBox's timeout leaves a watcher in a session of its own
 * (bb_daemonize calls setsid) that asks kill(parent, 0) each second whether
 * the program is still there - refused here, it concluded the program had
 * ended and never stopped it (L2 step 7's corpus). What keeps an unprivileged
 * program from killing what is not its own is the credentials (L2 step 1),
 * which is Linux's rule; SIGCONT within a session is allowed whoever owns the
 * target, so a shell can continue a job that changed its user.
 *
 * init and the kernel are exempt: init supervises services and stopping them is
 * its job. */
static int linux_signal_permitted(int target, uint32_t sig) {
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
    if (sig == VIBEOS_SIGCONT && ks_id(target)->sid == me->sid) {
        return 1;
    }
    /* And whose process it is (docs/abi/ L2): the superuser signals anybody;
     * anybody else, a process whose real or saved user id is the sender's real
     * or effective one - Linux's rule, on top of the session one above. */
    {
        vibeos_cred_t mine;
        const vibeos_procstate_t *tp = ks_ps(target);

        linux_cred(&mine);
        if (mine.euid == 0u || !tp) {
            return 1;
        }
        return mine.uid == tp->cred.uid || mine.uid == tp->cred.suid ||
               mine.euid == tp->cred.uid || mine.euid == tp->cred.suid;
    }
}

/* Who is sending: what a siginfo_t says of the sender (L2). Read before the
 * scheduler's lock is taken - the credentials have a lock of their own. */
static void linux_sender(vibeos_siginfo_t *why, uint32_t from) {
    vibeos_cred_t c;
    uint32_t i;

    for (i = 0; i < sizeof(*why); i++) {
        ((unsigned char *)why)[i] = 0;
    }
    linux_cred(&c);
    why->from = from;
    why->pid = ks_id(ks_current())->tgid;
    why->uid = c.uid;
}

/* A pid, or failing that a thread id: Linux's kill, rt_sigqueueinfo and the
 * rest find a process by the id of any of its threads (LTP's rt_sigqueueinfo01
 * sends to a thread's tid and got ESRCH here). Under the scheduler's lock. */
static int linux_task_by_any_id(uint32_t id) {
    int t = ks_task_by_pid(id);
    return t >= 0 ? t : ks_task_by_tid(id);
}

static long linux_sys_kill(uint64_t target_pid, uint64_t sig) {
    int target;
    int delivered = 0;
    int64_t signed_pid = (int64_t)target_pid;
    long r;
    vibeos_siginfo_t why;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (sig > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    linux_sender(&why, VIBEOS_SIG_FROM_PROCESS);
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
            r = linux_signal_permitted(target, (uint32_t)sig) ? 0 : -VIBEOS_EPERM;
        }
        ks_unlock(ks_sched_lock());
        return r;
    }
    if (signed_pid < 0 || signed_pid == 0) {
        uint32_t group = signed_pid < 0 ? (uint32_t)(-signed_pid) : ks_id(ks_current())->pgid;
        int i, members = 0;
        ks_lock(ks_sched_lock(), __func__);
        for (i = 0; i < (int)ks_slots(); i++) {
        /* Every member the sender may signal, each judged as a single kill
         * would judge it - not "every member of the sender's session", which
         * was this kernel's rule and not Linux's. A group with members none of
         * whom may be signalled is EPERM, as Linux answers; none at all ESRCH. */
            if (ks_id(i)->is_user && vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_FREE &&
                vibeos_task_state((uint32_t)(i)) != VIBEOS_TASK_SETUP &&
                ks_id(i)->pgid == group) {
                members++;
                if (linux_signal_permitted(i, (uint32_t)sig)) {
                    /* One already on its way out takes nothing, and that is
                     * not the sender's error - as for a single pid below. */
                    (void)ks_signal_send(i, (uint32_t)sig, &why);
                    delivered++;
                }
            }
        }
        ks_unlock(ks_sched_lock());
        return delivered != 0 ? 0 : members != 0 ? -VIBEOS_EPERM : -VIBEOS_ESRCH;
    }
    ks_lock(ks_sched_lock(), __func__);
    target = linux_task_by_any_id((uint32_t)signed_pid);
    if (target < 0) {
        r = -VIBEOS_ESRCH;
    } else if (!linux_signal_permitted(target, (uint32_t)sig)) {
        /* EPERM and not ESRCH: the caller is being refused, not lied to about
         * whether the process exists. Hiding existence would be a different
         * decision, and this kernel's group branch does not make it either. */
        r = -VIBEOS_EPERM;
    } else {
        /* A process that is already on its way out takes no signal, and that
         * is not the sender's error: Linux answers 0 for a signal to a process
         * that has exited and not been reaped (LTP's sigwait helpers kill
         * their child after it ended, and got EINVAL here). */
        (void)ks_signal_send(target, (uint32_t)sig, &why);
        r = 0;
    }
    ks_unlock(ks_sched_lock());
    return r;
}

static long linux_sys_tkill(uint64_t target_tid, uint64_t sig) {
    int target;
    long r;
    vibeos_siginfo_t why;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user ||
        sig > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    linux_sender(&why, VIBEOS_SIG_FROM_THREAD);
    /* Lookup, check and raise as one critical section (H-007). */
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_tid((uint32_t)target_tid);
    if (target >= 0 && !linux_signal_permitted(target, (uint32_t)sig)) {
        r = -VIBEOS_EPERM;   /* same rule as kill; see linux_signal_permitted */
    } else if (target < 0) {
        r = -VIBEOS_ESRCH;
    } else if (sig == 0u) {
        r = 0;
    } else {
        (void)ks_signal_send(target, (uint32_t)sig, &why);   /* exiting: see kill */
        r = 0;
    }
    ks_unlock(ks_sched_lock());
    if (sig == 0u || r != 0 || !linux_sig_chatty()) {
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
    vibeos_siginfo_t why;

    /* Ids are positive: Linux refuses a zero or negative one before looking
     * (LTP's tgkill03). */
    if (ks_current() < 0 || !ks_id(ks_current())->is_user ||
        sig > VIBEOS_SIG_MAX || (int32_t)(uint32_t)target_tgid <= 0 ||
        (int32_t)(uint32_t)target_tid <= 0) {
        return -VIBEOS_EINVAL;
    }
    linux_sender(&why, VIBEOS_SIG_FROM_THREAD);
    /* Lookup, identity check and raise as one critical section (H-007): the
     * tgid comparison protects nothing if the slot can change after it. */
    ks_lock(ks_sched_lock(), __func__);
    target = ks_task_by_tid((uint32_t)target_tid);
    if (target >= 0 && !linux_signal_permitted(target, (uint32_t)sig)) {
        r = -VIBEOS_EPERM;   /* same rule as kill; see linux_signal_permitted */
    } else if (target < 0 || ks_id(target)->tgid != (uint32_t)target_tgid) {
        r = -VIBEOS_ESRCH;
    } else if (sig == 0u) {
        r = 0;
    } else {
        (void)ks_signal_send(target, (uint32_t)sig, &why);   /* exiting: see kill */
        r = 0;
    }
    ks_unlock(ks_sched_lock());
    if (sig == 0u || r != 0 || !linux_sig_chatty()) {
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
static long linux_sys_rt_sigaction(uint64_t sig, uint64_t act_uptr, uint64_t old_uptr,
                                   uint64_t setsize) {
    vibeos_procstate_t *ps;

    /* A mask is eight bytes; any other size is a program built for another
     * kernel's sigset_t, refused before anything is read (LTP's rt_sigaction03). */
    if (ks_current() < 0 || sig == 0u || sig > VIBEOS_SIG_MAX || setsize != 8u) {
        return -VIBEOS_EINVAL;
    }
    if (sig == VIBEOS_SIGKILL || sig == VIBEOS_SIGSTOP) {
        return -VIBEOS_EINVAL;   /* neither can be caught */
    }
    ps = ks_ps(ks_current());
    if (ps == 0) {
        return -VIBEOS_EINVAL;
    }

    if (old_uptr != 0u) {
        linux_sigaction_t old;
        old.sa_handler = ps->sig_handler[sig];
        old.sa_flags = ps->sig_flags[sig];
        old.sa_restorer = ps->sig_restorer[sig];
        old.sa_mask = ps->sig_mask[sig] >> 1;
        /* Written through the fault-safe copy: a sibling munmap between the
         * range check and here would fault in ring 0 otherwise (H-016). */
        if (vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, &old, sizeof(old)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    if (act_uptr != 0u) {
        linux_sigaction_t act;
        if (vibeos_uaccess_copy(&act, (const void *)(uintptr_t)act_uptr,
                                sizeof(act)) != 0) {
            return -VIBEOS_EFAULT;   /* H-016 */
        }
        /* The handler becomes the resume address at delivery, and iretq to a
         * non-canonical rip faults in ring 0 - a kernel panic a program could
         * trigger with sigaction + a signal (H-017). A canonical handler that
         * is unmapped only faults in ring 3 and kills the task, so canonicality
         * and the user window are what must be checked. The two sentinels are
         * dispositions (default, ignore), not addresses. */
        if (act.sa_handler != SIG_DFL_ADDR && act.sa_handler != SIG_IGN_ADDR &&
            !ks_user_addr_ok(act.sa_handler)) {
            return -VIBEOS_EINVAL;
        }
        ps->sig_handler[sig] = act.sa_handler;
        ps->sig_flags[sig] = act.sa_flags;
        ps->sig_restorer[sig] = act.sa_restorer;
        ps->sig_mask[sig] = act.sa_mask << 1;
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
/* linux_sigset_from_user and linux_sigset_to_user are in linux_internal.h:
 * the signal frame converts the mask it saves too. */

static long linux_sys_rt_sigprocmask(uint64_t how, uint64_t set_uptr, uint64_t old_uptr,
                                     uint64_t setsize) {
    vibeos_task_t *t;
    uint64_t set = 0;

    if (ks_current() < 0 || setsize != 8u) {   /* as rt_sigaction (rt_sigprocmask02) */
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
        case LINUX_SIG_BLOCK: t->sig_blocked |= set; break;
        case LINUX_SIG_UNBLOCK: t->sig_blocked &= ~set; break;
        case LINUX_SIG_SETMASK: t->sig_blocked = set; break;
        default: return -VIBEOS_EINVAL;
    }
    /* Blocking these would make a process unkillable, so the request is
     * accepted and the two bits are dropped, exactly as Linux does. */
    t->sig_blocked &= ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    return 0;
}

/* The alternate stack, set from a stack_t: sigaltstack, and rt_sigreturn putting
 * back what the frame recorded. Refused while the thread is running on it -
 * moving a stack out from under its own handler is how a program corrupts it. */
static long linux_altstack_set(vibeos_task_t *t, const linux_stack_t *ss, uint64_t sp) {
    int32_t mode = ss->ss_flags & ~(int32_t)LINUX_SS_AUTODISARM;

    if (linux_on_altstack(t, sp)) {
        return -VIBEOS_EPERM;
    }
    if (mode != 0 && mode != LINUX_SS_ONSTACK && mode != LINUX_SS_DISABLE) {
        return -VIBEOS_EINVAL;
    }
    if (mode == LINUX_SS_DISABLE) {
        t->sas_sp = 0;
        t->sas_size = 0;
        t->sas_flags = 0;
        return 0;
    }
    if (ss->ss_size < LINUX_MINSIGSTKSZ) {
        return -VIBEOS_ENOMEM;
    }
    t->sas_sp = ss->ss_sp;
    t->sas_size = ss->ss_size;
    t->sas_flags = (uint32_t)ss->ss_flags & LINUX_SS_AUTODISARM;
    return 0;
}

/* rt_sigreturn(): put back everything the handler interrupted.
 *
 * The frame is Linux's (signal.c), in memory the program can write - and may
 * have, on purpose: a handler that recovers from a fault moves the saved rip.
 * So it is read as the program left it, and what of it may be trusted is the
 * architecture's to decide (ks_regs_set: the selectors and privileged flags
 * forced, the resume address checked, H-018). A frame that is missing,
 * unreadable or refused ends the task with SIGSEGV rather than resuming from
 * whatever is on the stack. */
static long linux_sys_rt_sigreturn(ks_regs_t *frame) {
    vibeos_task_t *t;
    linux_ucontext_t uc;
    vibeos_uregs_t r;
    uint64_t base;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    t = ks_id(ks_current());
    /* The handler has returned, so rsp points just past the return address
     * that the trampoline popped: at the ucontext. */
    base = ks_regs_sp(frame);
    if (!linux_user_ok(base, sizeof(uc), 0) ||
        vibeos_uaccess_copy(&uc, (const void *)(uintptr_t)base, sizeof(uc)) != 0) {
        goto bad;
    }
    r.r8 = uc.uc_mcontext.r8;    r.r9 = uc.uc_mcontext.r9;
    r.r10 = uc.uc_mcontext.r10;  r.r11 = uc.uc_mcontext.r11;
    r.r12 = uc.uc_mcontext.r12;  r.r13 = uc.uc_mcontext.r13;
    r.r14 = uc.uc_mcontext.r14;  r.r15 = uc.uc_mcontext.r15;
    r.rdi = uc.uc_mcontext.rdi;  r.rsi = uc.uc_mcontext.rsi;
    r.rbp = uc.uc_mcontext.rbp;  r.rbx = uc.uc_mcontext.rbx;
    r.rdx = uc.uc_mcontext.rdx;  r.rax = uc.uc_mcontext.rax;
    r.rcx = uc.uc_mcontext.rcx;  r.rsp = uc.uc_mcontext.rsp;
    r.rip = uc.uc_mcontext.rip;  r.rflags = uc.uc_mcontext.eflags;
    r.cs = uc.uc_mcontext.cs;
    r.ss = uc.uc_mcontext.ss;
    /* The vector registers, if the frame names them. 0 is "none", as on Linux;
     * anything else has to be an aligned area the program can read. */
    if (uc.uc_mcontext.fpstate != 0u &&
        ((uc.uc_mcontext.fpstate & 15u) != 0u ||
         !linux_user_ok(uc.uc_mcontext.fpstate, ks_fpu_size(), 0) ||
         ks_fpu_restore(uc.uc_mcontext.fpstate) != 0)) {
        goto bad;
    }
    if (ks_regs_set(frame, &r) != 0) {
        goto bad;
    }
    t->sig_blocked = linux_sigset_from_user(uc.uc_sigmask) &
                     ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    /* The alternate stack as it was when the handler was entered - which is
     * how SS_AUTODISARM arms it again. A refusal is not the program's error
     * here; Linux ignores it too. */
    (void)linux_altstack_set(t, &uc.uc_stack, r.rsp);
    return (long)ks_regs_ret(frame);

bad:
    t->exit_signal = VIBEOS_SIGSEGV;
    ks_task_exit(128ull + VIBEOS_SIGSEGV);
    return 0;
}

/* sigaltstack(): set, report, or both. The old one is read before the new one
 * is set and written only if setting it worked. */
static long linux_sys_sigaltstack(ks_regs_t *frame, uint64_t ss_uptr, uint64_t old_uptr) {
    vibeos_task_t *t;
    linux_stack_t ss, old;
    uint64_t sp;
    long r;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    t = ks_id(ks_current());
    sp = ks_regs_sp(frame);
    {
        uint32_t i;
        for (i = 0; i < sizeof(old); i++) {
            ((unsigned char *)&old)[i] = 0;   /* the padding goes out too */
        }
    }
    old.ss_sp = t->sas_sp;
    old.ss_size = t->sas_size;
    old.ss_flags = (int32_t)(t->sas_size == 0u ? LINUX_SS_DISABLE
                   : (linux_on_altstack(t, sp) ? LINUX_SS_ONSTACK : 0)) | (int32_t)t->sas_flags;
    if (ss_uptr != 0u) {
        if (vibeos_uaccess_copy(&ss, (const void *)(uintptr_t)ss_uptr, sizeof(ss)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if ((r = linux_altstack_set(t, &ss, sp)) != 0) {
            return r;
        }
    }
    if (old_uptr != 0u &&
        vibeos_uaccess_copy((void *)(uintptr_t)old_uptr, &old, sizeof(old)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* rt_sigpending(): what has been raised and is waiting behind the mask. */
static long linux_sys_rt_sigpending(uint64_t set_uptr, uint64_t size) {
    const vibeos_task_t *t;
    uint64_t out;

    if (ks_current() < 0 || size != 8u) {
        return -VIBEOS_EINVAL;
    }
    t = ks_id(ks_current());
    out = linux_sigset_to_user(t->sig_pending & t->sig_blocked);
    if (vibeos_uaccess_copy((void *)(uintptr_t)set_uptr, &out, sizeof(out)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* Wait until a signal arrives that the program has to see: one with a handler,
 * or one that ends it. Always EINTR, never run again - sigsuspend and pause are
 * defined as returning when a handler has run. */
static long linux_wait_for_signal(void) {
    int me = ks_current();

    while (!ks_signal_interrupts(me)) {
        ks_wait_tick();
    }
    return -VIBEOS_EINTR;
}

static long linux_sys_pause(void) {
    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    return linux_wait_for_signal();
}

/* rt_sigsuspend(): wait under a temporary mask. The program's own mask is kept
 * aside for the handler's frame (sig_saved), so the handler runs under the
 * temporary one and the program resumes under its own - the one atomic step
 * sigprocmask followed by pause cannot be. */
static long linux_sys_rt_sigsuspend(uint64_t set_uptr, uint64_t size) {
    vibeos_task_t *t;
    uint64_t raw;

    if (ks_current() < 0 || size != 8u) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&raw, (const void *)(uintptr_t)set_uptr, sizeof(raw)) != 0) {
        return -VIBEOS_EFAULT;
    }
    t = ks_id(ks_current());
    t->sig_saved = t->sig_blocked;
    t->sig_saved_valid = 1;
    t->sig_blocked = linux_sigset_from_user(raw) &
                     ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    return linux_wait_for_signal();
}

/* rt_sigtimedwait(): take one of `set` off the pending set without running its
 * handler - which is why programs block the set first - and say why it came.
 * EAGAIN when the time runs out, EINTR when some other signal needs acting on. */
static long linux_sys_rt_sigtimedwait(uint64_t set_uptr, uint64_t info_uptr,
                                      uint64_t ts_uptr, uint64_t size) {
    int me = ks_current();
    vibeos_task_t *t;
    uint64_t raw, want, deadline = 0;

    if (me < 0 || size != 8u) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&raw, (const void *)(uintptr_t)set_uptr, sizeof(raw)) != 0) {
        return -VIBEOS_EFAULT;
    }
    want = linux_sigset_from_user(raw) & ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    if (ts_uptr != 0u) {
        linux_timespec_t ts;
        int64_t ticks;

        if (vibeos_uaccess_copy(&ts, (const void *)(uintptr_t)ts_uptr, sizeof(ts)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if ((ticks = linux_ticks_of(&ts)) < 0) {
            return -VIBEOS_EINVAL;
        }
        deadline = ks_ticks() + (uint64_t)ticks;
    }
    t = ks_id(me);
    for (;;) {
        uint64_t ready = t->sig_pending & want;

        if (ready) {
            uint32_t sig = 1;
            vibeos_siginfo_t why;

            while ((ready & (1ull << sig)) == 0u) {
                sig++;
            }
            if (ks_signal_take(me, sig, &why)) {
                if (info_uptr != 0u) {
                    linux_siginfo_t info;

                    linux_siginfo_from(&info, sig, &why);
                    if (vibeos_uaccess_copy((void *)(uintptr_t)info_uptr, &info, sizeof(info)) != 0) {
                        return -VIBEOS_EFAULT;
                    }
                }
                return (long)sig;
            }
            continue;
        }
        if (ks_signal_interrupts(me)) {
            return -VIBEOS_EINTR;
        }
        if (ts_uptr != 0u && ks_ticks() >= deadline) {
            return -VIBEOS_EAGAIN;
        }
        ks_wait_tick();
    }
}

/* rt_sigqueueinfo() and rt_tgsigqueueinfo(): a signal with a reason the sender
 * wrote. Only to itself may a program claim a code that says the kernel or kill
 * sent it (si_code >= 0, or SI_TKILL); anything else could forge a sender. */
static long linux_sys_rt_sigqueueinfo(uint64_t tgid, uint64_t tid, uint64_t sig,
                                      uint64_t info_uptr, int to_thread) {
    linux_siginfo_t in;
    vibeos_siginfo_t why;
    const vibeos_task_t *me;
    int target;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user || sig > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    me = ks_id(ks_current());
    if (vibeos_uaccess_copy(&in, (const void *)(uintptr_t)info_uptr, sizeof(in)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if ((in.si_code >= 0 || in.si_code == LINUX_SI_TKILL) &&
        (uint32_t)(to_thread ? tid : tgid) != me->pid) {
        return -VIBEOS_EPERM;
    }
    {
        uint32_t i;
        for (i = 0; i < sizeof(why); i++) {
            ((unsigned char *)&why)[i] = 0;
        }
    }
    why.from = VIBEOS_SIG_FROM_QUEUE;
    why.code = in.si_code;
    why.pid = (uint32_t)in.pid;
    why.uid = in.uid;
    why.addr = in.value;

    /* Lookup, check and raise as one critical section (H-007), as kill. */
    ks_lock(ks_sched_lock(), __func__);
    target = to_thread ? ks_task_by_tid((uint32_t)tid) : linux_task_by_any_id((uint32_t)tgid);
    if (target < 0 || (to_thread && ks_id(target)->tgid != (uint32_t)tgid)) {
        r = -VIBEOS_ESRCH;
    } else if (!linux_signal_permitted(target, (uint32_t)sig)) {
        r = -VIBEOS_EPERM;
    } else if (sig == 0u) {
        r = 0;
    } else {
        (void)ks_signal_send(target, (uint32_t)sig, &why);   /* exiting: see kill */
        r = 0;
    }
    ks_unlock(ks_sched_lock());
    return r;
}

/* ---- pidfds (docs/abi/ L2 step 5) ---------------------------------------------------
 *
 * A descriptor that names a process, so that a signal reaches the process it
 * was meant for even if the number is reused (kernel/abi/files/pidfd.c). */

/* pidfd_open(): for a process - a thread group's leader - not for a thread.
 * Close-on-exec always, as Linux makes them. */
static long linux_sys_pidfd_open(uint64_t pid, uint64_t flags) {
    vibeos_file_t *f;
    uint32_t seq = 0;
    int slot;
    long r = 0;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    if ((int32_t)(uint32_t)pid <= 0 || (flags & ~(uint64_t)LINUX_PIDFD_NONBLOCK)) {
        return -VIBEOS_EINVAL;
    }
    ks_lock(ks_sched_lock(), __func__);
    slot = ks_task_by_tid((uint32_t)pid);
    if (slot < 0) {
        r = -VIBEOS_ESRCH;
    } else if (ks_id(slot)->tgid != (uint32_t)pid) {
        r = -VIBEOS_EINVAL;   /* a thread, not a process */
    } else {
        seq = ks_seq(slot);
    }
    ks_unlock(ks_sched_lock());
    if (r != 0) {
        return r;
    }
    f = vibeos_open_pidfd((uint32_t)pid, seq, (uint32_t)flags);
    if (!f) {
        return -VIBEOS_ENFILE;
    }
    return linux_fd_install(f, VIBEOS_FD_CLOEXEC, 0);
}

/* The process a pidfd names, by pid - for waitid's P_PIDFD. EBADF for a
 * descriptor that is not a pidfd. */
long linux_pidfd_pid(uint64_t fd) {
    vibeos_file_t *f = linux_file_get(fd);
    long pid;

    if (!f) {
        return -VIBEOS_EBADF;
    }
    pid = f->ops == &vibeos_fops_pidfd ? (long)f->proc_pid : -VIBEOS_EBADF;
    vibeos_file_put(f);
    return pid;
}

/* pidfd_send_signal(): kill, or rt_sigqueueinfo when a siginfo is given, to the
 * process the descriptor names - ESRCH once it has been reaped, whoever has its
 * number now. */
static long linux_sys_pidfd_send_signal(uint64_t fd, uint64_t sig, uint64_t info_uptr, uint64_t flags) {
    vibeos_file_t *f;
    vibeos_siginfo_t why;
    int slot;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user || flags != 0u || sig > VIBEOS_SIG_MAX) {
        return -VIBEOS_EINVAL;
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    if (f->ops != &vibeos_fops_pidfd) {
        vibeos_file_put(f);
        return -VIBEOS_EBADF;
    }
    if (info_uptr != 0u) {
        linux_siginfo_t in;
        uint32_t i;

        if (vibeos_uaccess_copy(&in, (const void *)(uintptr_t)info_uptr, sizeof(in)) != 0) {
            vibeos_file_put(f);
            return -VIBEOS_EFAULT;
        }
        /* As rt_sigqueueinfo: the record must be for this signal, and only to
         * itself may a process claim kill or the kernel sent it. */
        if ((uint32_t)in.si_signo != (uint32_t)sig ||
            ((in.si_code >= 0 || in.si_code == LINUX_SI_TKILL) && f->proc_pid != ks_id(ks_current())->tgid)) {
            vibeos_file_put(f);
            return (uint32_t)in.si_signo != (uint32_t)sig ? -VIBEOS_EINVAL : -VIBEOS_EPERM;
        }
        for (i = 0; i < sizeof(why); i++) {
            ((unsigned char *)&why)[i] = 0;
        }
        why.from = VIBEOS_SIG_FROM_QUEUE;
        why.code = in.si_code;
        why.pid = (uint32_t)in.pid;
        why.uid = in.uid;
        why.addr = in.value;
    } else {
        linux_sender(&why, VIBEOS_SIG_FROM_PROCESS);
    }
    ks_lock(ks_sched_lock(), __func__);
    slot = vibeos_pidfd_slot(f);
    if (slot < 0) {
        r = -VIBEOS_ESRCH;
    } else if (!linux_signal_permitted(slot, (uint32_t)sig)) {
        r = -VIBEOS_EPERM;
    } else {
        if (sig != 0u) {
            (void)ks_signal_send(slot, (uint32_t)sig, &why);   /* exiting: see kill */
        }
        r = 0;
    }
    ks_unlock(ks_sched_lock());
    vibeos_file_put(f);
    return r;
}

/* ---- the syscalls this file implements ---------------------------------------
 *
 *   tkill   raise() goes through tkill, not kill: a library raising a signal in
 *           itself targets its own thread, and with one thread per process that is
 *           the same destination.
 *   tgkill  the thread named by tid, provided it still belongs to tgid - the check
 *           that stops a recycled thread id from reaching a different process. */
#define LINUX_SIG_SYSCALLS(X) \
    X(13,  rt_sigaction,   SIG_ACTION,   PTRS(OUT_OPT(2, sizeof(linux_sigaction_t)), IN_OPT(1, sizeof(linux_sigaction_t))), linux_sys_rt_sigaction(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(14,  rt_sigprocmask, SIG_PROCMASK, PTRS(OUT_OPT(2, 8), IN_OPT(1, 8)), linux_sys_rt_sigprocmask(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(15,  rt_sigreturn,   SIG_RETURN,   NOPTR, linux_sys_rt_sigreturn(FRAME)) \
    X(62,  kill,           KILL,         NOPTR, linux_sys_kill(ARG(0), ARG(1))) \
    X(200, tkill,          TKILL,        NOPTR, linux_sys_tkill(ARG(0), ARG(1))) \
    X(234, tgkill,         TGKILL,       NOPTR, linux_sys_tgkill(ARG(0), ARG(1), ARG(2))) \
    X(34,  pause,          SIG_PAUSE,    NOPTR, linux_sys_pause()) \
    X(127, rt_sigpending,  SIG_PENDING,  PTRS(OUT(0, 8)), linux_sys_rt_sigpending(ARG(0), ARG(1))) \
    X(128, rt_sigtimedwait, SIG_TIMEDWAIT, PTRS(IN(0, 8), OUT_OPT(1, sizeof(linux_siginfo_t)), IN_OPT(2, sizeof(linux_timespec_t))), linux_sys_rt_sigtimedwait(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(129, rt_sigqueueinfo, SIG_QUEUE,   PTRS(IN(2, sizeof(linux_siginfo_t))), linux_sys_rt_sigqueueinfo(ARG(0), 0, ARG(1), ARG(2), 0)) \
    X(130, rt_sigsuspend,  SIG_SUSPEND,  PTRS(IN(0, 8)), linux_sys_rt_sigsuspend(ARG(0), ARG(1))) \
    X(131, sigaltstack,    SIG_ALTSTACK, PTRS(IN_OPT(0, sizeof(linux_stack_t)), OUT_OPT(1, sizeof(linux_stack_t))), linux_sys_sigaltstack(FRAME, ARG(0), ARG(1))) \
    X(297, rt_tgsigqueueinfo, SIG_TGQUEUE, PTRS(IN(3, sizeof(linux_siginfo_t))), linux_sys_rt_sigqueueinfo(ARG(0), ARG(1), ARG(2), ARG(3), 1)) \
    X(434, pidfd_open,     PIDFD_OPEN,   NOPTR, linux_sys_pidfd_open(ARG(0), ARG(1))) \
    X(424, pidfd_send_signal, PIDFD_SEND_SIGNAL, PTRS(IN_OPT(2, sizeof(linux_siginfo_t))), linux_sys_pidfd_send_signal(ARG(0), ARG(1), ARG(2), ARG(3)))

LINUX_DEFINE_SYSCALLS(sig, LINUX_SIG_SYSCALLS)
