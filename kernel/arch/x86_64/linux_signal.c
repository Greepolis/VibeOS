/* Signal delivery, lifted out of arch_hw.c.
 *
 * Building a frame on a user stack, calling a handler on it, and taking the
 * frame back when the handler returns is Linux ABI, not x86 architecture. What
 * is architectural about it - the selectors, the trap frame layout - is named in
 * arch_hw_internal.h and nothing else here needs to know.
 *
 * The two halves belong together and were four hundred lines apart in the old
 * file, with the crash dumper and half the syscall table between them.
 */

#include "arch_hw_internal.h"

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

/* hw_sigframe_t and its magic are in arch_hw_internal.h. */


/* Which signal to deliver next: the lowest-numbered pending one that is not
 * blocked. Lowest first is what Linux does, and it puts the fatal ones - which
 * are the low numbers - ahead of the informational ones. */
static uint32_t hw_signal_next(hw_task_t *t) {
    uint64_t ready = t->sig_pending & ~t->sig_blocked;
    uint32_t sig;

    /* SIGKILL and SIGSTOP ignore the mask entirely. */
    ready |= t->sig_pending & ((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    if (ready == 0u) {
        return 0;
    }
    for (sig = 1; sig < VIBEOS_HW_NSIG; sig++) {
        if (ready & (1ull << sig)) {
            return sig;
        }
    }
    return 0;
}

/* Called on the way back to user space. Returns non-zero if the frame was
 * rewritten to enter a handler. May not return at all, if the signal kills. */
int hw_signal_deliver(vibeos_x86_64_isr_frame_t *frame) {
    hw_task_t *t;
    uint32_t sig;
    uint64_t handler, sp;
    hw_sigframe_t *sf;

    if (hw_current_task() < 0 || !g_tasks[hw_current_task()].is_user) {
        return 0;
    }
    t = &g_tasks[hw_current_task()];
    for (;;) {
        sig = hw_signal_next(t);
        if (sig == 0u) {
            return 0;
        }
        t->sig_pending &= ~(1ull << sig);

        handler = t->sig_handler[sig];
        if (sig == VIBEOS_SIGKILL || sig == VIBEOS_SIGSTOP) {
            handler = SIG_DFL_ADDR;   /* uncatchable */
        }
        if (handler == SIG_IGN_ADDR) {
            continue;
        }
        if (handler == SIG_DFL_ADDR) {
            if (sig == VIBEOS_SIGSTOP) {
                t->signal_stopped = 1;
                (void)hw_task_set_state((int)(t - g_tasks), HW_TASK_BLOCKED, __func__);
                HW_TASK_MARK(hw_current_task(), ready_by, "sigstop");
                vibeos_x86_64_serial_puts("[SIG] task stopped by SIGSTOP\n");
                return 0;
            }
            if (hw_signal_default_kills(sig)) {
                t->exit_signal = sig;
                hw_task_exit(128ull + sig);   /* does not return */
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
    hw_log(VIBEOS_LOG_DEBUG, 42u, (uint64_t)sig, handler,
           "signal delivered to a handler (a0 = signal, a1 = handler)");
    vibeos_x86_64_serial_lock();
    vibeos_x86_64_serial_puts("[SIG] deliver sig=0x");
    vibeos_x86_64_serial_print_hex(sig);
    vibeos_x86_64_serial_puts(" handler=0x");
    vibeos_x86_64_serial_print_hex(handler);
    vibeos_x86_64_serial_puts("\n");
    vibeos_x86_64_serial_unlock();


    /* Below the red zone, then aligned. The handler is entered as if by a
     * call, so it wants rsp % 16 == 8 once the return address is pushed. */
    sp = frame->rsp - 128ull;
    sp -= sizeof(hw_sigframe_t);
    sp &= ~15ull;
    sp -= 8ull;   /* room for the return address */

    if (!hw_user_range_ok(sp, sizeof(hw_sigframe_t) + 8ull, 1)) {
        /* No usable stack to deliver on. A program cannot be asked to handle
         * that, so the signal takes its default action instead of being
         * silently dropped. */
        hw_task_exit(128ull + sig);
        return 0;
    }

    sf = (hw_sigframe_t *)(uintptr_t)(sp + 8ull);
    sf->magic = HW_SIGFRAME_MAGIC;
    sf->blocked = t->sig_blocked;
    sf->frame = *frame;

    /* The return address is the C library's trampoline, which issues
     * rt_sigreturn. Without SA_RESTORER there is nothing to return to, and a
     * handler that returns would jump to whatever was on the stack. */
    if ((t->sig_flags[sig] & VIBEOS_SA_RESTORER) == 0u || t->sig_restorer[sig] == 0u) {
        hw_task_exit(128ull + sig);
        return 0;
    }
    *(uint64_t *)(uintptr_t)sp = t->sig_restorer[sig];

    /* While the handler runs, this signal is blocked, plus whatever the
     * program asked to block along with it - otherwise a repeating signal
     * re-enters the handler until the stack is gone. */
    t->sig_blocked |= (1ull << sig) | t->sig_mask[sig];

    frame->rip = handler;
    frame->rsp = sp;
    frame->rdi = sig;    /* the handler's first argument */
    frame->rsi = 0;
    frame->rdx = 0;
    frame->rax = 0;
    return 1;
}


