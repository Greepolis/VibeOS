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

/* The frame's layout is the architecture's: ks_sigframe_push, ks_sigframe_size. */


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

/* Called on the way back to user space. Returns non-zero if the frame was
 * rewritten to enter a handler. May not return at all, if the signal kills. */
int linux_signal_deliver(ks_regs_t *frame) {
    vibeos_task_t *t;
    vibeos_procstate_t *ps;
    int me;
    uint32_t sig;
    uint64_t handler, sp;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return 0;
    }
    me = ks_current();
    t = ks_id(me);
    ps = ks_ps(me);
    for (;;) {
        sig = linux_signal_next(t);
        if (sig == 0u) {
            return 0;
        }
        t->sig_pending &= ~(1ull << sig);

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


    /* Below the red zone, then aligned. The handler is entered as if by a
     * call, so it wants rsp % 16 == 8 once the return address is pushed. */
    sp = ks_regs_sp(frame) - 128ull;
    sp -= ks_sigframe_size();
    sp &= ~15ull;
    sp -= 8ull;   /* room for the return address */

    if (!linux_user_ok(sp, ks_sigframe_size() + 8ull, 1)) {
        /* No usable stack to deliver on. A program cannot be asked to handle
         * that, so the signal takes its default action instead of being
         * silently dropped. */
        ks_task_exit(128ull + sig);
        return 0;
    }

    /* The return address is the C library's trampoline, which issues
     * rt_sigreturn. Without SA_RESTORER there is nothing to return to, and a
     * handler that returns would jump to whatever was on the stack. Checked
     * first, before the stack is touched. */
    if ((ps->sig_flags[sig] & VIBEOS_SA_RESTORER) == 0u ||
        ps->sig_restorer[sig] == 0u) {
        ks_task_exit(128ull + sig);
        return 0;
    }

    /* Built in the kernel and copied out fault-safely: a sibling thread can
     * munmap the stack page between the range check and these writes, and
     * building the frame in place would fault in ring 0 (H-023). On a fault the
     * frame is not left half-written - the task takes SIGSEGV, its default
     * action, rather than the kernel taking the fault. */
    if (ks_sigframe_push(frame, sp, t->sig_blocked, ps->sig_restorer[sig]) != 0) {
        ks_task_exit(128ull + VIBEOS_SIGSEGV);
        return 0;
    }

    /* While the handler runs, this signal is blocked, plus whatever the
     * program asked to block along with it - otherwise a repeating signal
     * re-enters the handler until the stack is gone. */
    t->sig_blocked |= (1ull << sig) | ps->sig_mask[sig];

    ks_regs_enter_handler(frame, handler, sp, sig);
    return 1;
}


