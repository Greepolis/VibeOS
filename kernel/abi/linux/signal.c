/* Signal delivery, lifted out of arch_hw.c.
 *
 * Building a frame on a user stack, calling a handler on it, and taking the
 * frame back when the handler returns is Linux ABI, not x86 architecture. What
 * is architectural about it - the selectors, the trap frame layout - is behind
 * ks_sigframe_push, ks_sigframe_pop and ks_regs_enter_handler (vibeos/ksvc.h),
 * and nothing else here needs to know.
 *
 * The two halves belong together and were four hundred lines apart in the old
 * file, with the crash dumper and half the syscall table between them.
 */

#include "linux_internal.h"


/* ---- delivering a signal ---------------------------------------------------
 *
 * A signal is delivered by making the interrupted program call the handler and
 * then return to where it was. The kernel does that by building a frame on the
 * program's own stack holding the entire interrupted register state, pointing
 * the return address at a trampoline the C library supplied, and rewriting the
 * trapframe so the resume goes to the handler instead of the interrupted
 * instruction. rt_sigreturn later reads that frame back.
 *
 * The frame goes below the red zone. The System V ABI lets a leaf function use
 * the 128 bytes below the stack pointer without reserving them, so writing a
 * frame at rsp would corrupt live data in a program that was doing nothing
 * wrong.
 */

/* The frame is Linux's since docs/abi/ L2: a return address, a ucontext and a
 * siginfo, with the vector registers in an FXSAVE area above them. It used to be
 * private to this kernel - a magic, the mask and the raw trap frame - which was
 * enough for a handler that only returns, and nothing for one that asks why it
 * was called or where: si_addr, the saved rip, what was blocked. */


/* Which signal to deliver next: the lowest-numbered pending one that is not
 * blocked. Lowest first is what Linux does, and it puts the fatal ones - which
 * are the low numbers - ahead of the informational ones. */
static uint32_t linux_signal_next(const vibeos_task_t *t) {
    uint64_t ready = t->sig_pending & ~t->sig_blocked;
    uint32_t sig;

    /* SIGKILL and SIGSTOP ignore the mask entirely. */
    ready |= t->sig_pending & ((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    if (ready == 0u) {
        return 0;
    }
    for (sig = 1; sig < VIBEOS_NSIG; sig++) {
        if (ready & (1ull << sig)) {
            return sig;
        }
    }
    return 0;
}

/* Is `sp` on the thread's alternate stack? Linux's test, end inclusive: a stack
 * pointer at the very top has pushed nothing yet and is on it. */
int linux_on_altstack(const vibeos_task_t *t, uint64_t sp) {
    return t->sas_size != 0u && sp > t->sas_sp && sp - t->sas_sp <= t->sas_size;
}

/* The reason a signal came, in Linux's words. Every field the signal's kind does
 * not use is zero: a handler that reads si_pid of a fault gets 0, not the last
 * tenant's stack. */
void linux_siginfo_from(linux_siginfo_t *o, uint32_t sig, const vibeos_siginfo_t *in) {
    uint32_t i;

    for (i = 0; i < sizeof(*o); i++) {
        ((unsigned char *)o)[i] = 0;
    }
    o->si_signo = (int32_t)sig;
    switch (in->from) {
        case VIBEOS_SIG_FROM_PROCESS:
            o->si_code = LINUX_SI_USER;
            o->pid = (int32_t)in->pid;
            o->uid = in->uid;
            break;
        case VIBEOS_SIG_FROM_THREAD:
            o->si_code = LINUX_SI_TKILL;
            o->pid = (int32_t)in->pid;
            o->uid = in->uid;
            break;
        case VIBEOS_SIG_FROM_QUEUE:
            /* As the sender wrote it: sigqueueinfo hands the whole record over,
             * and only the codes that would claim to be the kernel were refused
             * when it was sent. */
            o->si_code = in->code;
            o->pid = (int32_t)in->pid;
            o->uid = in->uid;
            o->value = in->addr;
            break;
        case VIBEOS_SIG_FROM_CHILD:
            o->si_code = in->code == (int32_t)VIBEOS_CHILD_KILLED ? LINUX_CLD_KILLED : LINUX_CLD_EXITED;
            o->pid = (int32_t)in->pid;
            o->uid = in->uid;
            linux_si_set_status(o, in->status);
            o->utime = (int64_t)in->utime;
            break;
        case VIBEOS_SIG_FROM_FAULT:
            linux_si_set_addr(o, in->addr);
            switch (in->trapno) {
                case 14u: o->si_code = (in->fault_err & 1u) ? LINUX_SEGV_ACCERR : LINUX_SEGV_MAPERR; break;
                case 0u:  o->si_code = LINUX_FPE_INTDIV; break;
                case 6u:  o->si_code = LINUX_ILL_ILLOPN; break;
                case 17u: o->si_code = LINUX_BUS_ADRALN; break;
                case 16u:
                case 19u: o->si_code = LINUX_FPE_FLTINV; break;
                default:
                    /* #GP names no address: what the program touched is not
                     * something the CPU reports. */
                    o->si_code = LINUX_SI_KERNEL;
                    linux_si_set_addr(o, 0);
                    break;
            }
            break;
        default:
            o->si_code = LINUX_SI_KERNEL;
            break;
    }
}

/* A wait under a temporary mask (sigsuspend) that ended without a handler to
 * carry the program's own mask back: it goes back here. */
static void linux_signal_unsave(vibeos_task_t *t) {
    if (t->sig_saved_valid) {
        t->sig_blocked = t->sig_saved;
        t->sig_saved_valid = 0;
    }
}

/* Called on the way back to user space. Returns non-zero if the frame was
 * rewritten to enter a handler. May not return at all, if the signal kills.
 *
 * It also decides what becomes of a call a signal cut short (the dispatcher
 * left its number in sys_restart). If a handler runs and was installed with
 * SA_RESTART, the frame the handler will return to is made to issue the call
 * again; without the flag it keeps the EINTR. If no handler runs - the signal
 * was discarded, or stopped the task - there is nobody to have asked for
 * EINTR, and the call is issued again. This did not exist: every interrupted
 * wait was EINTR, and a parent whose child reports through a signal - LTP's
 * harness, every shell with a SIGCHLD handler - saw waitpid fail. */
int linux_signal_deliver(ks_regs_t *frame) {
    vibeos_task_t *t;
    vibeos_procstate_t *ps;
    int me;
    uint32_t sig;
    uint64_t handler, flags, restart, saved, sp, fp, base;
    vibeos_siginfo_t why;
    vibeos_uregs_t regs;
    linux_ucontext_t uc;
    linux_siginfo_t info;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return 0;
    }
    me = ks_current();
    t = ks_id(me);
    ps = ks_ps(me);
    restart = t->sys_restart;
    t->sys_restart = 0;
    for (;;) {
        sig = linux_signal_next(t);
        if (sig == 0u) {
            if (restart) {
                ks_regs_restart(frame, restart - 1u);
            }
            linux_signal_unsave(t);
            return 0;
        }
        if (!ks_signal_take(me, sig, &why)) {
            continue;   /* nobody else takes this thread's signals; cannot happen */
        }

        /* A user task whose process reference is already gone is exiting;
         * the default is the only disposition it has left. */
        handler = ps ? ps->sig_handler[sig] : SIG_DFL_ADDR;
        if (sig == VIBEOS_SIGKILL || sig == VIBEOS_SIGSTOP) {
            handler = SIG_DFL_ADDR;   /* uncatchable */
        }
        if (handler == SIG_IGN_ADDR) {
            continue;
        }
        if (handler == SIG_DFL_ADDR) {
            if (sig == VIBEOS_SIGSTOP) {
                t->signal_stopped = 1;
                (void)ks_set_state(me, VIBEOS_TASK_BLOCKED, __func__);
                ks_mark_ready(me, "sigstop");
                ks_con_puts("[SIG] task stopped by SIGSTOP\n");
                if (restart) {
                    ks_regs_restart(frame, restart - 1u);   /* waits again once continued */
                }
                return 0;
            }
            if (sig == VIBEOS_SIGKILL && ps != 0 &&
                __atomic_load_n(&ps->exit_group, __ATOMIC_ACQUIRE) != 0u) {
                /* Ended by exit_group, not by a signal from outside. The
                 * parent builds the wait status from the leader, so a leader
                 * that died here as "killed by 9" would report exactly what an
                 * exit_group built from SIGKILL alone reports - and Linux
                 * reports the group's code. */
                t->exit_signal = 0;
                ks_task_exit(ps->exit_group_code);   /* does not return */
            }
            if (ks_signal_default_kills(sig)) {
                t->exit_signal = sig;
                ks_task_exit(128ull + sig);   /* does not return */
            }
            continue;   /* default is to ignore it */
        }
        break;
    }
    flags = ps->sig_flags[sig];

    /* Bracketed, like every other multi-part message: puts and print_hex take
     * the lock individually, so without this the line is four critical
     * sections and another core writes into the middle of it. Found by the
     * gate's log-integrity check, which saw the handler address cut off after
     * its "0x". */
    ks_log(VIBEOS_LOG_DEBUG, 42u, (uint64_t)sig, handler,
           "signal delivered to a handler (a0 = signal, a1 = handler)");
    ks_con_lock();
    ks_con_puts("[SIG] deliver sig=0x");
    ks_con_hex(sig);
    ks_con_puts(" handler=0x");
    ks_con_hex(handler);
    ks_con_puts("\n");
    ks_con_unlock();

    /* The return address is the C library's trampoline, which issues
     * rt_sigreturn. Without SA_RESTORER there is nothing to return to, and a
     * handler that returns would jump to whatever was on the stack. Checked
     * first, before the stack is touched. */
    if ((flags & VIBEOS_SA_RESTORER) == 0u || ps->sig_restorer[sig] == 0u) {
        ks_task_exit(128ull + sig);
        return 0;
    }
    if (restart && (flags & VIBEOS_SA_RESTART)) {
        ks_regs_restart(frame, restart - 1u);   /* saved below: what sigreturn resumes */
    }
    ks_regs_get(frame, &regs);
    /* What the handler returns to: the program's own mask, also when it was
     * waiting under a temporary one. */
    saved = t->sig_saved_valid ? t->sig_saved : t->sig_blocked;

    /* Where: on the alternate stack if the handler asked for it and the thread
     * is not already on it, else below the red zone - the System V ABI lets a
     * leaf function use the 128 bytes below rsp without reserving them, so a
     * frame written at rsp would corrupt live data in a program that was doing
     * nothing wrong. Then the vector registers, 64-aligned as Linux places
     * them; then the frame, so that the handler is entered as if by a call:
     * rsp % 16 == 8, with the return address at rsp. */
    sp = regs.rsp;
    if ((flags & LINUX_SA_ONSTACK) && t->sas_size != 0u && !linux_on_altstack(t, sp)) {
        sp = t->sas_sp + t->sas_size;
    } else {
        sp -= 128ull;
    }
    fp = (sp - ks_fpu_size()) & ~63ull;
    base = ((fp - (8ull + sizeof(uc) + sizeof(info))) & ~15ull) - 8ull;

    /* No usable stack to deliver on: a program cannot be asked to handle that,
     * so it takes SIGSEGV, as Linux does. */
    if (!linux_user_ok(base, (fp + ks_fpu_size()) - base, 1)) {
        t->exit_signal = VIBEOS_SIGSEGV;
        ks_task_exit(128ull + VIBEOS_SIGSEGV);
        return 0;
    }

    {
        uint32_t i;
        for (i = 0; i < sizeof(uc); i++) {
            ((unsigned char *)&uc)[i] = 0;
        }
    }
    uc.uc_flags = LINUX_UC_SIGCONTEXT_SS | LINUX_UC_STRICT_RESTORE_SS;
    uc.uc_stack.ss_sp = t->sas_sp;
    uc.uc_stack.ss_size = t->sas_size;
    uc.uc_stack.ss_flags = (int32_t)(t->sas_size == 0u ? LINUX_SS_DISABLE
                           : (linux_on_altstack(t, regs.rsp) ? LINUX_SS_ONSTACK : 0)) |
                           (int32_t)t->sas_flags;
    uc.uc_mcontext.r8 = regs.r8;    uc.uc_mcontext.r9 = regs.r9;
    uc.uc_mcontext.r10 = regs.r10;  uc.uc_mcontext.r11 = regs.r11;
    uc.uc_mcontext.r12 = regs.r12;  uc.uc_mcontext.r13 = regs.r13;
    uc.uc_mcontext.r14 = regs.r14;  uc.uc_mcontext.r15 = regs.r15;
    uc.uc_mcontext.rdi = regs.rdi;  uc.uc_mcontext.rsi = regs.rsi;
    uc.uc_mcontext.rbp = regs.rbp;  uc.uc_mcontext.rbx = regs.rbx;
    uc.uc_mcontext.rdx = regs.rdx;  uc.uc_mcontext.rax = regs.rax;
    uc.uc_mcontext.rcx = regs.rcx;  uc.uc_mcontext.rsp = regs.rsp;
    uc.uc_mcontext.rip = regs.rip;  uc.uc_mcontext.eflags = regs.rflags;
    uc.uc_mcontext.cs = regs.cs;
    uc.uc_mcontext.ss = regs.ss;
    if (why.from == VIBEOS_SIG_FROM_FAULT) {
        uc.uc_mcontext.err = why.fault_err;
        uc.uc_mcontext.trapno = why.trapno;
        uc.uc_mcontext.cr2 = why.trapno == 14u ? why.addr : 0u;
    }
    uc.uc_mcontext.oldmask = linux_sigset_to_user(saved);
    uc.uc_mcontext.fpstate = fp;
    uc.uc_sigmask = linux_sigset_to_user(saved);
    linux_siginfo_from(&info, sig, &why);

    /* Built in the kernel and copied out fault-safely: a sibling thread can
     * munmap the stack page between the range check and these writes, and
     * building the frame in place would fault in ring 0 (H-023). On a fault the
     * task takes SIGSEGV rather than the kernel taking the fault. */
    {
        uint64_t ret = ps->sig_restorer[sig];

        if (ks_fpu_save(fp) != 0 ||
            vibeos_uaccess_copy((void *)(uintptr_t)base, &ret, sizeof(ret)) != 0 ||
            vibeos_uaccess_copy((void *)(uintptr_t)(base + 8ull), &uc, sizeof(uc)) != 0 ||
            vibeos_uaccess_copy((void *)(uintptr_t)(base + 8ull + sizeof(uc)), &info, sizeof(info)) != 0) {
            t->exit_signal = VIBEOS_SIGSEGV;
            ks_task_exit(128ull + VIBEOS_SIGSEGV);
            return 0;
        }
    }

    /* SS_AUTODISARM: the stack is the handler's now, and a signal inside it
     * must not start again at its top; sigreturn arms it again from uc_stack. */
    if (t->sas_flags & LINUX_SS_AUTODISARM) {
        t->sas_sp = 0;
        t->sas_size = 0;
        t->sas_flags = 0;
    }
    /* While the handler runs, this signal is blocked, plus whatever the
     * program asked to block along with it - otherwise a repeating signal
     * re-enters the handler until the stack is gone. SA_NODEFER asks for the
     * re-entry; SA_RESETHAND for one delivery only. */
    t->sig_saved_valid = 0;
    t->sig_blocked = saved | ps->sig_mask[sig] |
                     ((flags & LINUX_SA_NODEFER) ? 0u : (1ull << sig));
    t->sig_blocked &= ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    if (flags & LINUX_SA_RESETHAND) {
        ps->sig_handler[sig] = SIG_DFL_ADDR;
        ps->sig_flags[sig] &= ~LINUX_SA_SIGINFO;
    }

    ks_regs_enter_handler(frame, handler, base, sig,
                          base + 8ull + sizeof(uc), base + 8ull);
    return 1;
}

/* A CPU exception in a program (docs/abi/ L2): the architecture asks whether
 * the program takes it. Yes only if a handler is installed and the signal is
 * not blocked - Linux forces the default for a synchronous fault that is
 * blocked or ignored, since returning to the instruction would fault again
 * forever. 1 if the frame now enters the handler; 0 and the architecture kills
 * the task, as it did for every fault before L2. */
int linux_signal_fault(ks_regs_t *frame, uint32_t sig, const vibeos_siginfo_t *why) {
    int me = ks_current();
    const vibeos_procstate_t *ps;
    uint64_t handler;

    if (me < 0 || !ks_id(me)->is_user || sig == 0u || sig > VIBEOS_SIG_MAX) {
        return 0;
    }
    ps = ks_ps(me);
    handler = ps ? ps->sig_handler[sig] : SIG_DFL_ADDR;
    if (handler == SIG_DFL_ADDR || handler == SIG_IGN_ADDR ||
        (ks_id(me)->sig_blocked & (1ull << sig))) {
        return 0;
    }
    if (ks_signal_send(me, sig, why) != 0) {
        return 0;
    }
    return linux_signal_deliver(frame);
}
