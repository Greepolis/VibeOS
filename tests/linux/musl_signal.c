/* Signals, from a real Linux program built against a real C library.
 *
 * Written against Linux, not against VibeOS: it uses sigaction and raise the
 * way any program would, and the C library supplies the return trampoline the
 * kernel has to jump back through. That is the point - a signal implementation
 * that only satisfies a hand-written test proves very little, because the hard
 * parts are the ones libc assumes rather than the ones a test remembers to
 * check.
 *
 * What it actually checks, in order:
 *
 *   the handler runs at all, with the right signal number
 *   execution resumes where it was interrupted, with locals intact - which is
 *     what the saved register frame is for
 *   a blocked signal stays pending instead of being lost, and arrives when it
 *     is unblocked
 *   an ignored signal really is discarded
 *   the default action still kills, and the parent sees 128 + the signal
 *   (L2) a handler is told why it was called and where, and what it changes
 *     in the frame is what the program resumes with: a fault recovered on
 *     the alternate stack, the vector registers, sigqueue's value; and the
 *     calls that wait for a signal - sigtimedwait, sigsuspend, pause - and
 *     SIGCHLD, which a child's end now raises
 */

/* REG_RIP and the rest of ucontext_t's register names. */
#define _GNU_SOURCE 1

#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <ucontext.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_got;
static volatile sig_atomic_t g_count;

static void on_signal(int sig) {
    g_got = sig;
    g_count++;
}

/* ---- docs/abi/ L2 step 2 --------------------------------------------------------
 *
 * What a handler installed with SA_SIGINFO is told, and that what it changes in
 * the frame is what the program resumes with; and the calls that wait for a
 * signal rather than being interrupted by one. Each prints its own SIG_FAIL. */

/* A store that faults, and where to resume past it. The handler moves the saved
 * rip to fault_resume - which only works if the frame is Linux's ucontext and
 * rt_sigreturn reads it back. rax says whether the store was skipped. */
extern char fault_insn[], fault_resume[];
long fault_try(void *addr);
__asm__(".text\n"
        ".globl fault_try\n"
        "fault_try:\n"
        "    movq $1, %rax\n"
        ".globl fault_insn\n"
        "fault_insn:\n"
        "    movb $1, (%rdi)\n"
        "    movq $0, %rax\n"
        ".globl fault_resume\n"
        "fault_resume:\n"
        "    ret\n");

static char g_alt[16384];
static volatile int g_seg_signo, g_seg_code, g_seg_on_alt, g_seg_rip_ok;
static volatile uintptr_t g_seg_addr;

static void on_segv(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    char here;

    g_seg_signo = sig;
    g_seg_code = si->si_code;
    g_seg_addr = (uintptr_t)si->si_addr;
    g_seg_on_alt = &here > g_alt && &here < g_alt + sizeof(g_alt);
    g_seg_rip_ok = uc->uc_mcontext.gregs[REG_RIP] == (greg_t)(uintptr_t)fault_insn;
    uc->uc_mcontext.gregs[REG_RIP] = (greg_t)(uintptr_t)fault_resume;
}

static volatile int g_info_signo, g_info_code, g_info_pid, g_info_value;

static void on_info(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    g_info_signo = sig;
    g_info_code = si->si_code;
    g_info_pid = si->si_pid;
    g_info_value = si->si_value.sival_int;
}

static void on_clobber(int sig) {
    /* A handler that uses the vector registers, as memcpy does. */
    __asm__ __volatile__("pcmpeqd %%xmm8, %%xmm8" ::: "xmm8");
    g_got = sig;
}

static int l2_checks(void) {
    struct sigaction sa;
    sigset_t set, old, pend;
    siginfo_t si;
    struct timespec ts;
    stack_t ss;
    int ok = 1;

    /* A fault the program handles, on its alternate stack. */
    printf("SIG_PHASE: fault handler\n");
    fflush(stdout);
    ss.ss_sp = g_alt;
    ss.ss_size = sizeof(g_alt);
    ss.ss_flags = 0;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    if (sigaltstack(&ss, NULL) != 0 || sigaction(SIGSEGV, &sa, NULL) != 0) {
        printf("SIG_FAIL: sigaltstack or sigaction for SIGSEGV\n");
        ok = 0;
    } else if (fault_try((void *)0x40) != 1) {
        printf("SIG_FAIL: the faulting store was not skipped\n");
        ok = 0;
    } else if (g_seg_signo != SIGSEGV || g_seg_code != SEGV_MAPERR || g_seg_addr != 0x40 ||
               !g_seg_on_alt || !g_seg_rip_ok) {
        printf("SIG_FAIL: fault: signo=%d code=%d addr=%lx on_alt=%d rip_ok=%d\n",
               g_seg_signo, g_seg_code, (unsigned long)g_seg_addr, g_seg_on_alt, g_seg_rip_ok);
        ok = 0;
    }
    if (fault_try((void *)0x48) != 1 || g_seg_addr != 0x48) {
        printf("SIG_FAIL: a second fault was not handled the same way\n");
        ok = 0;
    }
    signal(SIGSEGV, SIG_DFL);

    /* The vector registers survive a handler that uses them. */
    {
        unsigned long long out = 0, pattern = 0x0123456789abcdefull;
        long tid = syscall(SYS_gettid);

        signal(SIGUSR1, on_clobber);
        __asm__ __volatile__(
            "movq %[in], %%xmm8\n\t"
            "movl %[nr], %%eax\n\t"
            "movq %[tid], %%rdi\n\t"
            "movl %[sig], %%esi\n\t"
            "syscall\n\t"
            "movq %%xmm8, %[out]\n\t"
            : [out] "=r"(out)
            : [in] "r"(pattern), [nr] "i"(SYS_tkill), [tid] "r"(tid), [sig] "i"(SIGUSR1)
            : "rax", "rdi", "rsi", "rcx", "r11", "xmm8", "memory");
        if (g_got != SIGUSR1 || out != pattern) {
            printf("SIG_FAIL: xmm8 after a handler: %llx\n", out);
            ok = 0;
        }
    }

    /* sigqueue: the value arrives, and says it was queued. */
    printf("SIG_PHASE: siginfo\n");
    fflush(stdout);
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_info;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    {
        union sigval v;
        v.sival_int = 7;
        if (sigqueue(getpid(), SIGUSR1, v) != 0 || g_info_signo != SIGUSR1 ||
            g_info_code != SI_QUEUE || g_info_value != 7 || g_info_pid != getpid()) {
            printf("SIG_FAIL: sigqueue: signo=%d code=%d value=%d pid=%d\n",
                   g_info_signo, g_info_code, g_info_value, g_info_pid);
            ok = 0;
        }
    }

    /* sigpending and sigtimedwait: taken without a handler running. */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, &old);
    raise(SIGUSR2);
    sigemptyset(&pend);
    if (sigpending(&pend) != 0 || !sigismember(&pend, SIGUSR2)) {
        printf("SIG_FAIL: sigpending does not show a blocked signal\n");
        ok = 0;
    }
    ts.tv_sec = 1;
    ts.tv_nsec = 0;
    g_got = 0;
    if (sigtimedwait(&set, &si, &ts) != SIGUSR2 || si.si_signo != SIGUSR2 ||
        si.si_code != SI_TKILL || g_got != 0) {
        printf("SIG_FAIL: sigtimedwait: code=%d got=%d\n", si.si_code, (int)g_got);
        ok = 0;
    }
    ts.tv_sec = 0;
    ts.tv_nsec = 20000000;
    errno = 0;
    if (sigtimedwait(&set, &si, &ts) != -1 || errno != EAGAIN) {
        printf("SIG_FAIL: sigtimedwait with nothing pending: errno=%d\n", errno);
        ok = 0;
    }
    sigprocmask(SIG_SETMASK, &old, NULL);

    /* sigsuspend and pause: woken by a child's signal, and the mask put back. */
    printf("SIG_PHASE: waiting for a signal\n");
    fflush(stdout);
    {
        int round;

        signal(SIGUSR1, on_signal);
        for (round = 0; round < 2; round++) {
            pid_t child;
            int status;
            sigset_t now;

            sigemptyset(&set);
            sigaddset(&set, SIGUSR1);
            sigprocmask(SIG_BLOCK, &set, &old);
            g_got = 0;
            child = fork();
            if (child == 0) {
                usleep(200000);
                kill(getppid(), SIGUSR1);
                _exit(0);
            }
            errno = 0;
            if (round == 0) {
                sigset_t none;
                sigemptyset(&none);
                if (sigsuspend(&none) != -1 || errno != EINTR || g_got != SIGUSR1) {
                    printf("SIG_FAIL: sigsuspend: errno=%d got=%d\n", errno, (int)g_got);
                    ok = 0;
                }
                sigprocmask(SIG_SETMASK, NULL, &now);
                if (!sigismember(&now, SIGUSR1)) {
                    printf("SIG_FAIL: sigsuspend did not put the mask back\n");
                    ok = 0;
                }
            } else {
                sigprocmask(SIG_SETMASK, &old, NULL);
                if (pause() != -1 || errno != EINTR || g_got != SIGUSR1) {
                    printf("SIG_FAIL: pause: errno=%d got=%d\n", errno, (int)g_got);
                    ok = 0;
                }
            }
            waitpid(child, &status, 0);
            sigprocmask(SIG_SETMASK, &old, NULL);
        }
    }

    /* SIGCHLD: raised when a child ends, saying which and how. */
    printf("SIG_PHASE: SIGCHLD\n");
    fflush(stdout);
    {
        pid_t child;
        int status;

        sigemptyset(&set);
        sigaddset(&set, SIGCHLD);
        sigprocmask(SIG_BLOCK, &set, &old);
        child = fork();
        if (child == 0) {
            _exit(5);
        }
        ts.tv_sec = 5;
        ts.tv_nsec = 0;
        memset(&si, 0, sizeof(si));
        if (sigtimedwait(&set, &si, &ts) != SIGCHLD || si.si_pid != child ||
            si.si_code != CLD_EXITED || si.si_status != 5) {
            printf("SIG_FAIL: SIGCHLD: pid=%d (child %d) code=%d status=%d\n",
                   (int)si.si_pid, (int)child, si.si_code, si.si_status);
            ok = 0;
        }
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 5) {
            printf("SIG_FAIL: the child after its SIGCHLD was not reaped\n");
            ok = 0;
        }
        sigprocmask(SIG_SETMASK, &old, NULL);
    }
    if (ok) {
        printf("SIG_L2_OK: siginfo, ucontext, sigaltstack, sigqueue, sigtimedwait, sigsuspend, pause, SIGCHLD\n");
        fflush(stdout);
    }
    return ok;
}

int main(void) {
    struct sigaction sa;
    sigset_t block, old;
    volatile long witness = 0x5A5A5A5A;
    int ok = 1;

    /* H-017: the kernel must refuse a handler that is not a canonical user
     * address. At delivery the handler becomes the return rip, and iretq to a
     * non-canonical rip faults in ring 0 - a kernel panic reachable from ring 3
     * with sigaction + raise. Installed through the raw syscall because a C
     * library would object first; refused means the fix is in. If it is
     * accepted this must NOT raise it - reporting the acceptance is the red. */
    {
        unsigned long bad[4] = { 0xdead000000000000UL, 0UL, 0UL, 0UL };
        long r = syscall(SYS_rt_sigaction, SIGUSR1, bad, (void *)0, 8UL);
        if (r == 0) {
            printf("SIG_FAIL: a non-canonical handler was accepted\n");
            fflush(stdout);
            return 1;
        }
    }

    /* M-017: signal 64 has no bit in the 64-bit pending/blocked mask (the mask
     * is keyed by signal number, and bit 64 does not exist in a uint64_t), so
     * the kernel must refuse it rather than accept it and lose it. Through the
     * raw syscall because a C library clamps to its own NSIG first. */
    {
        long r = syscall(SYS_kill, getpid(), 64);
        if (r == 0) {
            printf("SIG_FAIL: signal 64 accepted but unrepresentable\n");
            fflush(stdout);
            return 1;
        }
    }

    printf("SIG_PHASE: sigaction\n");
    fflush(stdout);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        printf("SIG_FAIL: sigaction\n");
        return 1;
    }

    raise(SIGUSR1);
    printf("SIG_PHASE: handler\n");
    fflush(stdout);
    if (g_got != SIGUSR1 || g_count != 1) {
        printf("SIG_FAIL: handler did not run\n");
        ok = 0;
    }
    /* The value below has to survive the trip through the handler. If the
     * kernel restored the wrong frame, this is where it shows. */
    if (witness != 0x5A5A5A5A) {
        printf("SIG_FAIL: registers not restored\n");
        ok = 0;
    }

    /* Blocked means deferred, not dropped. */
    sigemptyset(&block);
    sigaddset(&block, SIGUSR2);
    printf("SIG_PHASE: masking\n");
    fflush(stdout);
    if (sigprocmask(SIG_BLOCK, &block, &old) != 0) {
        printf("SIG_FAIL: sigprocmask\n");
        ok = 0;
    }
    if (sigaction(SIGUSR2, &sa, NULL) != 0) {
        printf("SIG_FAIL: sigaction 2\n");
        ok = 0;
    }
    g_got = 0;
    raise(SIGUSR2);
    if (g_got != 0) {
        printf("SIG_FAIL: blocked signal was delivered\n");
        ok = 0;
    }
    if (sigprocmask(SIG_SETMASK, &old, NULL) != 0) {
        printf("SIG_FAIL: sigprocmask restore\n");
        ok = 0;
    }
    if (g_got != SIGUSR2) {
        printf("SIG_FAIL: pending signal lost while blocked\n");
        ok = 0;
    }

    /* Ignored means gone. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    printf("SIG_PHASE: ignoring\n");
    fflush(stdout);
    sigaction(SIGUSR1, &sa, NULL);
    g_got = 0;
    raise(SIGUSR1);
    if (g_got != 0) {
        printf("SIG_FAIL: ignored signal was delivered\n");
        ok = 0;
    }

    /* The default action for SIGTERM is still to die, and the parent should
     * see that rather than a normal exit. */
    {
        pid_t child = fork();
        printf("SIG_PHASE: fork child=%d\n", (int)child);
        fflush(stdout);
        if (child == 0) {
            printf("SIG_PHASE: child terminate\n");
            fflush(stdout);
            raise(SIGTERM);
            _exit(0);          /* only reached if the signal did nothing */
        } else if (child > 0) {
            int status = 0;
            waitpid(child, &status, 0);
            /* A signal death is not an exit with a large code: WIFSIGNALED is
             * the question, and WTERMSIG is the answer. */
            if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
                printf("SIG_FAIL: default action, status=%d\n", status);
                ok = 0;
            }
        }
    }

    /* SA_RESTART: a wait cut short by a handled signal goes on waiting, and
     * without the flag it fails with EINTR. The child gives the parent a third
     * of a second to be inside waitpid before it signals - if the signal came
     * first, the wait would not be interrupted at all and the second half
     * would have nothing to observe. */
    {
        int with;

        for (with = 1; with >= 0; with--) {
            pid_t child;
            int status = 0;
            pid_t r;

            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = on_signal;
            sa.sa_flags = with ? SA_RESTART : 0;
            sigaction(SIGUSR1, &sa, NULL);
            g_got = 0;
            printf("SIG_PHASE: restart=%d\n", with);
            fflush(stdout);
            child = fork();
            if (child == 0) {
                usleep(300000);
                kill(getppid(), SIGUSR1);
                usleep(300000);
                _exit(7);
            }
            errno = 0;
            r = waitpid(child, &status, 0);
            if (g_got != SIGUSR1) {
                printf("SIG_FAIL: restart=%d: the handler did not run\n", with);
                ok = 0;
            }
            if (with && (r != child || !WIFEXITED(status) || WEXITSTATUS(status) != 7)) {
                printf("SIG_FAIL: SA_RESTART: waitpid returned %d errno=%d\n", (int)r, errno);
                ok = 0;
            }
            if (!with && (r != -1 || errno != EINTR)) {
                printf("SIG_FAIL: no SA_RESTART: waitpid returned %d errno=%d, not EINTR\n", (int)r, errno);
                ok = 0;
            }
            if (r != child) {
                waitpid(child, &status, 0);
            }
        }
    }


    /* ---- L2 step 2: what a handler is told, and the calls that wait ---------- */
    ok &= l2_checks();

    printf(ok ? "SIG_OK: handlers, masking, ignoring and default actions\n"
              : "SIG_FAIL: see above\n");
    fflush(stdout);
    return ok ? 0 : 1;
}
