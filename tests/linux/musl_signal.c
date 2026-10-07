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
 *   (L2 step 3) timers fire, at about the time asked: alarm, setitimer's
 *     three, POSIX timers on the machine's clock and the CPU's; and the
 *     clocks a program reads its own CPU time from
 *   (L2 step 4) the limits the kernel enforces are run into: NOFILE,
 *     FSIZE (SIGXFSZ, then EFBIG), CPU (SIGXCPU); priorities, getrusage
 *     and personality
 *   (L2 step 5) waitid, waitpid by group, a pidfd signalled, polled and
 *     waited for, execveat on a descriptor, clone3 with CLONE_PIDFD
 */

/* REG_RIP and the rest of ucontext_t's register names. */
#define _GNU_SOURCE 1

#include <errno.h>
#include <stdint.h>
#include <time.h>
#include <ucontext.h>
#include <sys/time.h>
#include <sys/times.h>
#include <fcntl.h>
#include <sys/personality.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <poll.h>
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

static volatile int g_info_signo, g_info_code, g_info_pid, g_info_value, g_fx_stale;

static void on_info(int sig, siginfo_t *si, void *ctx) {
    const ucontext_t *uc = (const ucontext_t *)ctx;
    const unsigned char *fx = (const unsigned char *)uc->uc_mcontext.fpregs;
    int k;

    /* The FXSAVE area's reserved bytes, 416 to 463, are zero on Linux. They
     * were this kernel's old stack contents until the area was cleared before
     * FXSAVE (external review, 2026-10-07). */
    g_fx_stale = 0;
    for (k = 416; fx && k < 464; k++) {
        g_fx_stale |= fx[k];
    }
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
        if (g_fx_stale) {
            printf("SIG_FAIL: the frame's FXSAVE area carries bytes nobody wrote\n");
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

/* ---- docs/abi/ L2 step 3: timers --------------------------------------------
 *
 * Each timer has to *fire*, at about the time asked, and say what Linux says.
 * Bounded by the wall clock everywhere, so a timer that never fires fails the
 * check instead of hanging the boot. */

static volatile sig_atomic_t g_alrm, g_vt, g_prof;

static void on_alrm(int sig) { (void)sig; g_alrm++; }
static void on_vt(int sig) { (void)sig; g_vt++; }
static void on_prof(int sig) { (void)sig; g_prof++; }

static double now_mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Run on the CPU until `flag` moves or `limit` seconds of wall time pass. */
static void spin_until(volatile sig_atomic_t *flag, double limit) {
    double end = now_mono() + limit;
    volatile unsigned long x = 0;
    sig_atomic_t start = *flag;

    while (*flag == start && now_mono() < end) {
        x++;
    }
}

/* The kernel's sigevent, for SIGEV_THREAD_ID through the raw call: the C
 * library's timer_create does not take that one. */
struct k_sigevent {
    unsigned long value;
    int signo;
    int notify;
    int tid;
    char pad[44];
};

static int timer_checks(void) {
    struct sigaction sa;
    struct itimerval itv;
    struct sigevent sev;
    struct itimerspec its;
    struct timespec res, cpu;
    struct tms tm;
    sigset_t set, old;
    siginfo_t si;
    timer_t t;
    double t0, el;
    int ok = 1;

    printf("SIG_PHASE: timers\n");
    fflush(stdout);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_alrm;
    sigaction(SIGALRM, &sa, NULL);
    sa.sa_handler = on_vt;
    sigaction(SIGVTALRM, &sa, NULL);
    sa.sa_handler = on_prof;
    sigaction(SIGPROF, &sa, NULL);

    /* A child that arms an alarm and exits before it goes off: its timer has to
     * go with it. If it did not, it would fire into a process that no longer
     * exists - ptimer_orphan, which the gate requires to be zero - about the
     * time the alarm below is pausing. */
    {
        pid_t child = fork();
        if (child == 0) {
            alarm(1);
            _exit(0);
        }
        waitpid(child, NULL, 0);
    }

    /* alarm and pause: a second, give or take a tick. */
    t0 = now_mono();
    g_alrm = 0;
    alarm(1);
    pause();
    el = now_mono() - t0;
    if (g_alrm != 1 || el < 0.98 || el > 1.5) {
        printf("SIG_FAIL: alarm(1): fired=%d after %.3fs\n", (int)g_alrm, el);
        ok = 0;
    }

    /* setitimer, periodic: four expiries of 50ms, then disarmed. */
    memset(&itv, 0, sizeof(itv));
    itv.it_value.tv_usec = 50000;
    itv.it_interval.tv_usec = 50000;
    g_alrm = 0;
    t0 = now_mono();
    setitimer(ITIMER_REAL, &itv, NULL);
    while (g_alrm < 4 && now_mono() - t0 < 2.0) {
        pause();
    }
    el = now_mono() - t0;
    memset(&itv, 0, sizeof(itv));
    setitimer(ITIMER_REAL, &itv, NULL);
    getitimer(ITIMER_REAL, &itv);
    if (g_alrm < 4 || el < 0.19 || itv.it_value.tv_sec != 0 || itv.it_value.tv_usec != 0) {
        printf("SIG_FAIL: setitimer periodic: %d expiries in %.3fs\n", (int)g_alrm, el);
        ok = 0;
    }

    /* The CPU-time interval timers fire on time the process spends running. */
    memset(&itv, 0, sizeof(itv));
    itv.it_value.tv_usec = 30000;
    g_vt = 0;
    setitimer(ITIMER_VIRTUAL, &itv, NULL);
    spin_until(&g_vt, 5.0);
    g_prof = 0;
    setitimer(ITIMER_PROF, &itv, NULL);
    spin_until(&g_prof, 5.0);
    if (g_vt != 1 || g_prof != 1) {
        printf("SIG_FAIL: ITIMER_VIRTUAL fired %d, ITIMER_PROF %d\n", (int)g_vt, (int)g_prof);
        ok = 0;
    }

    /* A POSIX timer: SI_TIMER, the value given, no overrun. */
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, &old);
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR2;
    sev.sigev_value.sival_int = 9;
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 20000000;
    if (timer_create(CLOCK_MONOTONIC, &sev, &t) != 0 || timer_settime(t, 0, &its, NULL) != 0) {
        printf("SIG_FAIL: timer_create or timer_settime: errno=%d\n", errno);
        ok = 0;
    } else {
        struct timespec wait = { 2, 0 };
        if (sigtimedwait(&set, &si, &wait) != SIGUSR2 || si.si_code != SI_TIMER ||
            si.si_value.sival_int != 9 || timer_getoverrun(t) != 0) {
            printf("SIG_FAIL: POSIX timer: code=%d value=%d overrun=%d\n",
                   si.si_code, si.si_value.sival_int, timer_getoverrun(t));
            ok = 0;
        }
        timer_delete(t);
    }

    /* On the process's CPU clock: fires on CPU time spent. */
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR2;
    its.it_value.tv_nsec = 30000000;
    if (timer_create(CLOCK_PROCESS_CPUTIME_ID, &sev, &t) != 0 || timer_settime(t, 0, &its, NULL) != 0) {
        printf("SIG_FAIL: a timer on CLOCK_PROCESS_CPUTIME_ID: errno=%d\n", errno);
        ok = 0;
    } else {
        sigset_t pend;
        double end = now_mono() + 5.0;
        volatile unsigned long x = 0;

        do {
            x++;
            sigpending(&pend);
        } while (!sigismember(&pend, SIGUSR2) && now_mono() < end);
        if (!sigismember(&pend, SIGUSR2)) {
            printf("SIG_FAIL: the CPU-time timer never fired\n");
            ok = 0;
        } else {
            struct timespec zero = { 0, 0 };
            sigtimedwait(&set, &si, &zero);
        }
        timer_delete(t);
    }

    /* SIGEV_THREAD_ID, through the raw call: to this thread. */
    {
        struct k_sigevent ks;
        int id = -1;
        struct timespec wait = { 2, 0 };

        memset(&ks, 0, sizeof(ks));
        ks.signo = SIGUSR2;
        ks.notify = 4;   /* SIGEV_THREAD_ID */
        ks.tid = (int)syscall(SYS_gettid);
        its.it_value.tv_nsec = 10000000;
        if (syscall(SYS_timer_create, CLOCK_MONOTONIC, &ks, &id) != 0 ||
            syscall(SYS_timer_settime, id, 0, &its, NULL) != 0 ||
            sigtimedwait(&set, &si, &wait) != SIGUSR2 || si.si_code != SI_TIMER) {
            printf("SIG_FAIL: SIGEV_THREAD_ID: errno=%d\n", errno);
            ok = 0;
        }
        syscall(SYS_timer_delete, id);
    }
    sigprocmask(SIG_SETMASK, &old, NULL);

    /* The clocks. */
    if (clock_getres(CLOCK_MONOTONIC, &res) != 0 || res.tv_sec != 0 || res.tv_nsec != 10000000) {
        printf("SIG_FAIL: clock_getres: %ld.%09ld\n", (long)res.tv_sec, (long)res.tv_nsec);
        ok = 0;
    }
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) != 0 || (cpu.tv_sec == 0 && cpu.tv_nsec == 0) ||
        times(&tm) == (clock_t)-1 || tm.tms_utime == 0) {
        printf("SIG_FAIL: CPU time: %ld.%09ld utime=%ld\n", (long)cpu.tv_sec, (long)cpu.tv_nsec, (long)tm.tms_utime);
        ok = 0;
    }
    {
        struct timeval tv;
        struct timespec rt;
        gettimeofday(&tv, NULL);
        clock_gettime(CLOCK_REALTIME, &rt);
        if (rt.tv_sec - tv.tv_sec > 1 || tv.tv_sec > rt.tv_sec) {
            printf("SIG_FAIL: gettimeofday %ld and CLOCK_REALTIME %ld disagree\n", (long)tv.tv_sec, (long)rt.tv_sec);
            ok = 0;
        }
    }
    signal(SIGALRM, SIG_DFL);
    signal(SIGVTALRM, SIG_DFL);
    signal(SIGPROF, SIG_DFL);
    if (ok) {
        printf("TIMER_OK: alarm, setitimer real virtual prof, POSIX timers, CPU clocks\n");
        fflush(stdout);
    }
    return ok;
}

/* ---- docs/abi/ L2 step 4: limits ---------------------------------------------
 *
 * Each limit the kernel claims to enforce is run into, in a child where running
 * into it ends the process. Bounded by the wall clock, as the timers are. */

static volatile sig_atomic_t g_xcpu;

static void on_xcpu(int sig) { (void)sig; g_xcpu++; }

static int limit_checks(void) {
    struct rlimit rl;
    struct rusage ru;
    int ok = 1;

    printf("SIG_PHASE: limits\n");
    fflush(stdout);

    /* NOFILE: the table's limit. */
    {
        struct rlimit old;
        int fds[8], n = 0, fd = 0;

        getrlimit(RLIMIT_NOFILE, &old);
        rl.rlim_cur = 6;
        rl.rlim_max = old.rlim_max;
        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            printf("SIG_FAIL: setrlimit(RLIMIT_NOFILE): errno=%d\n", errno);
            ok = 0;
        }
        while (n < 8 && (fd = open("/tmp", O_RDONLY)) >= 0) {
            fds[n++] = fd;
        }
        if (fd >= 0 || errno != EMFILE || n > 3) {
            printf("SIG_FAIL: RLIMIT_NOFILE of 6: %d opened, then errno=%d\n", n, errno);
            ok = 0;
        }
        while (n > 0) {
            close(fds[--n]);
        }
        setrlimit(RLIMIT_NOFILE, &old);
    }

    /* FSIZE: SIGXFSZ kills a writer that passes it; ignored, the write is EFBIG. */
    {
        int round;

        for (round = 0; round < 2; round++) {
            pid_t child = fork();
            int status = 0;

            if (child == 0) {
                int fd = open("/tmp/limit.f", O_CREAT | O_TRUNC | O_WRONLY, 0644);
                char b[64];
                memset(b, 'x', sizeof(b));
                rl.rlim_cur = rl.rlim_max = 100;
                setrlimit(RLIMIT_FSIZE, &rl);
                if (round == 1) {
                    signal(SIGXFSZ, SIG_IGN);
                }
                if (write(fd, b, 64) != 64 || write(fd, b, 64) != 36) {
                    _exit(2);
                }
                _exit(write(fd, b, 1) == -1 && errno == EFBIG ? 0 : 3);
            }
            waitpid(child, &status, 0);
            if (round == 0 && !(WIFSIGNALED(status) && WTERMSIG(status) == SIGXFSZ)) {
                printf("SIG_FAIL: RLIMIT_FSIZE did not end the writer with SIGXFSZ: status=%x\n", status);
                ok = 0;
            }
            if (round == 1 && !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
                printf("SIG_FAIL: RLIMIT_FSIZE with SIGXFSZ ignored: status=%x\n", status);
                ok = 0;
            }
        }
        unlink("/tmp/limit.f");
    }

    /* CPU: a second of running and SIGXCPU arrives. */
    {
        pid_t child = fork();
        int status = 0;

        if (child == 0) {
            double end;
            volatile unsigned long x = 0;

            signal(SIGXCPU, on_xcpu);
            rl.rlim_cur = 1;
            rl.rlim_max = 3;
            setrlimit(RLIMIT_CPU, &rl);
            end = now_mono() + 10.0;
            while (!g_xcpu && now_mono() < end) {
                x++;
            }
            _exit(g_xcpu ? 0 : 4);
        }
        waitpid(child, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            printf("SIG_FAIL: RLIMIT_CPU sent no SIGXCPU: status=%x\n", status);
            ok = 0;
        }
    }

    /* Priorities, usage, personality. */
    errno = 0;
    {
        int set = setpriority(PRIO_PROCESS, 0, 5);
        int got = getpriority(PRIO_PROCESS, 0);
        long raw = syscall(SYS_getpriority, PRIO_PROCESS, 0);

        if (set != 0 || got != 5) {
            printf("SIG_FAIL: setpriority/getpriority: set=%d got=%d raw=%ld errno=%d\n", set, got, raw, errno);
            ok = 0;
        }
    }
    setpriority(PRIO_PROCESS, 0, 0);
    if (getrusage(RUSAGE_SELF, &ru) != 0 || (ru.ru_utime.tv_sec == 0 && ru.ru_utime.tv_usec == 0)) {
        printf("SIG_FAIL: getrusage reports no CPU time\n");
        ok = 0;
    }
    if (personality(0xffffffff) != 0 || personality(0x0008) != 0 || personality(0xffffffff) != 0x0008) {
        printf("SIG_FAIL: personality is not kept\n");
        ok = 0;
    }
    personality(0);
    if (ok) {
        printf("LIMITS_OK: NOFILE, FSIZE, CPU, priorities, getrusage, personality\n");
        fflush(stdout);
    }
    return ok;
}

/* ---- docs/abi/ L2 step 5: processes -------------------------------------------
 *
 * Through the raw calls, by number: they are newer than the C library's
 * wrappers for some of them, and the numbers are what the kernel sees. */

struct k_clone_args {
    unsigned long long flags, pidfd, child_tid, parent_tid, exit_signal, stack,
        stack_size, tls, set_tid, set_tid_size, cgroup;
};

static int process_checks(void) {
    siginfo_t si;
    int ok = 1, status;
    pid_t c;

    printf("SIG_PHASE: processes\n");
    fflush(stdout);

    /* waitid: look first (WNOWAIT), then reap. */
    c = fork();
    if (c == 0) {
        _exit(6);
    }
    memset(&si, 0, sizeof(si));
    if (waitid(P_PID, (id_t)c, &si, WEXITED | WNOWAIT) != 0 || si.si_pid != c ||
        si.si_code != CLD_EXITED || si.si_status != 6 ||
        waitid(P_PID, (id_t)c, &si, WEXITED) != 0 || si.si_pid != c ||
        waitpid(c, &status, WNOHANG) != -1 || errno != ECHILD) {
        printf("SIG_FAIL: waitid: pid=%d code=%d status=%d errno=%d\n",
               (int)si.si_pid, si.si_code, si.si_status, errno);
        ok = 0;
    }

    /* waitpid(-pgid): a child that made its own group. */
    c = fork();
    if (c == 0) {
        setpgid(0, 0);
        _exit(7);
    }
    setpgid(c, c);   /* either order: the group exists before the wait */
    if (waitpid(-c, &status, 0) != c || !WIFEXITED(status) || WEXITSTATUS(status) != 7) {
        printf("SIG_FAIL: waitpid(-pgid): errno=%d\n", errno);
        ok = 0;
    }

    /* A pidfd: signalled through it, readable once the child is gone, waited
     * for by it. */
    c = fork();
    if (c == 0) {
        for (;;) {
            pause();
        }
    }
    {
        int pfd = (int)syscall(434 /* pidfd_open */, c, 0);
        struct pollfd p;

        p.fd = pfd;
        p.events = POLLIN;
        p.revents = 0;
        if (pfd < 0 || poll(&p, 1, 0) != 0 ||
            syscall(424 /* pidfd_send_signal */, pfd, SIGTERM, NULL, 0) != 0 ||
            poll(&p, 1, 5000) != 1 || !(p.revents & POLLIN) ||
            waitid((idtype_t)3 /* P_PIDFD */, (id_t)pfd, &si, WEXITED) != 0 ||
            si.si_pid != c || si.si_code != CLD_KILLED || si.si_status != SIGTERM) {
            printf("SIG_FAIL: pidfd: fd=%d revents=%x code=%d status=%d errno=%d\n",
                   pfd, p.revents, si.si_code, si.si_status, errno);
            ok = 0;
        }
        close(pfd);
    }

    /* execveat with AT_EMPTY_PATH: run the program a descriptor names. */
    c = fork();
    if (c == 0) {
        static char *const argv[] = { "true", 0 };
        static char *const envp[] = { 0 };
        int fd = open("/EFI/BOOT/BUSYBOX.ELF", O_RDONLY);
        syscall(322 /* execveat */, fd, "", argv, envp, 0x1000 /* AT_EMPTY_PATH */);
        _exit(9);
    }
    if (waitpid(c, &status, 0) != c || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("SIG_FAIL: execveat(AT_EMPTY_PATH): status=%x\n", status);
        ok = 0;
    }

    /* clone3, as a fork with CLONE_PIDFD. */
    {
        struct k_clone_args a;
        int pfd = -1;
        long r;

        memset(&a, 0, sizeof(a));
        a.flags = 0x1000;   /* CLONE_PIDFD */
        a.pidfd = (unsigned long long)(unsigned long)&pfd;
        a.exit_signal = SIGCHLD;
        r = syscall(435 /* clone3 */, &a, sizeof(a));
        if (r == 0) {
            _exit(8);
        }
        if (r < 0 || pfd < 0 || waitid((idtype_t)3, (id_t)pfd, &si, WEXITED) != 0 ||
            si.si_pid != r || si.si_status != 8) {
            printf("SIG_FAIL: clone3: r=%ld pidfd=%d errno=%d\n", r, pfd, errno);
            ok = 0;
        }
        if (pfd >= 0) {
            close(pfd);
        }
    }
    if (ok) {
        printf("PROC_OK: waitid, waitpid by group, pidfds, execveat, clone3\n");
        fflush(stdout);
    }
    return ok;
}

/* ---- docs/abi/ L2 step 6: /proc, /dev, random numbers - and the three defects
 * step 5's LTP run found: a futex shared through a MAP_SHARED page, clone's
 * exit signal, and orphans that nobody adopted ---------------------------- */

static int read_file(const char *path, char *buf, int cap) {
    int fd = open(path, O_RDONLY), n, total = 0;

    if (fd < 0) {
        return -errno;
    }
    while (total < cap - 1 && (n = (int)read(fd, buf + total, (size_t)(cap - 1 - total))) > 0) {
        total += n;
    }
    buf[total] = 0;
    close(fd);
    return total;
}

static volatile sig_atomic_t g_usr2;

static void on_usr2(int s) {
    (void)s;
    g_usr2 = 1;
}

static int procdev_checks(void) {
    static char buf[8192];
    char path[64], want[32], link[256];
    unsigned char r1[16], r2[16], z[16];
    int ok = 1, n, status, fd, i, seen = 0;
    pid_t c;

    printf("SIG_PHASE: proc and dev\n");
    fflush(stdout);

    /* The asking process, by the link that names it, and its files. */
    n = (int)readlink("/proc/self", link, sizeof(link) - 1);
    snprintf(want, sizeof(want), "%d", (int)getpid());
    if (n <= 0 || (link[n] = 0, strcmp(link, want) != 0)) {
        printf("SIG_FAIL: /proc/self: n=%d '%s'\n", n, n > 0 ? link : "");
        ok = 0;
    }
    n = read_file("/proc/self/stat", buf, sizeof(buf));
    if (n <= 0 || atoi(buf) != (int)getpid() || !strstr(buf, ") R ")) {
        printf("SIG_FAIL: /proc/self/stat: n=%d '%.60s'\n", n, n > 0 ? buf : "");
        ok = 0;
    }
    n = (int)readlink("/proc/self/exe", link, sizeof(link) - 1);
    if (n <= 0 || link[0] != '/') {
        printf("SIG_FAIL: /proc/self/exe: n=%d\n", n);
        ok = 0;
    }
    n = (int)readlink("/proc/self/fd/1", link, sizeof(link) - 1);
    if (n <= 0) {
        printf("SIG_FAIL: /proc/self/fd/1: n=%d errno=%d\n", n, errno);
        ok = 0;
    }
    snprintf(want, sizeof(want), "Pid:\t%d\n", (int)getpid());
    if (read_file("/proc/self/status", buf, sizeof(buf)) <= 0 || !strstr(buf, want) ||
        read_file("/proc/self/cmdline", buf, sizeof(buf)) <= 0 ||
        read_file("/proc/self/maps", buf, sizeof(buf)) <= 0 || !strstr(buf, "[stack]") ||
        read_file("/proc/cpuinfo", buf, sizeof(buf)) <= 0 || !strstr(buf, "processor\t: 0\n") ||
        !strstr(buf, "flags\t\t: ") ||
        read_file("/proc/mounts", buf, sizeof(buf)) <= 0 || !strstr(buf, " /proc proc ") ||
        !strstr(buf, " /dev devtmpfs ")) {
        printf("SIG_FAIL: a /proc file: '%.80s'\n", buf);
        ok = 0;
    }

    /* A child that waits shows S - what LTP's harness waits to see before it
     * signals a child that pauses. */
    c = fork();
    if (c == 0) {
        for (;;) {
            pause();
        }
    }
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)c);
    for (i = 0; i < 300 && !seen; i++) {
        if (read_file(path, buf, sizeof(buf)) > 0) {
            char *p = strrchr(buf, ')');
            seen = p && p[1] == ' ' && p[2] == 'S';
        }
        if (!seen) {
            usleep(10000);
        }
    }
    kill(c, SIGKILL);
    waitpid(c, &status, 0);
    if (!seen) {
        printf("SIG_FAIL: a paused child never showed S: '%.60s'\n", buf);
        ok = 0;
    }

    /* /dev and getrandom. */
    fd = open("/dev/null", O_RDWR);
    if (fd < 0 || write(fd, "abcde", 5) != 5 || read(fd, buf, 16) != 0) {
        printf("SIG_FAIL: /dev/null: fd=%d errno=%d\n", fd, errno);
        ok = 0;
    }
    close(fd);
    memset(z, 0xAA, sizeof(z));
    fd = open("/dev/zero", O_RDONLY);
    if (fd < 0 || read(fd, z, sizeof(z)) != (ssize_t)sizeof(z) || z[0] != 0 || z[15] != 0) {
        printf("SIG_FAIL: /dev/zero\n");
        ok = 0;
    }
    close(fd);
    fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, r1, sizeof(r1)) != (ssize_t)sizeof(r1) ||
        syscall(318 /* getrandom */, r2, sizeof(r2), 0) != (long)sizeof(r2) ||
        memcmp(r1, r2, sizeof(r1)) == 0) {
        printf("SIG_FAIL: /dev/urandom or getrandom: errno=%d\n", errno);
        ok = 0;
    }
    close(fd);
    fd = open("/dev/full", O_WRONLY);
    if (fd < 0 || write(fd, "x", 1) != -1 || errno != ENOSPC) {
        printf("SIG_FAIL: /dev/full\n");
        ok = 0;
    }
    close(fd);

    /* A futex shared between two processes through a MAP_SHARED page of a
     * file, as LTP's checkpoint is; woken by the one, waited on by the other. */
    fd = open("/tmp/sig-futex", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, 4096) != 0) {
        printf("SIG_FAIL: futex file: errno=%d\n", errno);
        ok = 0;
    } else {
        volatile uint32_t *w = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        if (w == MAP_FAILED) {
            printf("SIG_FAIL: futex mmap: errno=%d\n", errno);
            ok = 0;
        } else {
            *w = 0;
            c = fork();
            if (c == 0) {
                struct timespec t = {5, 0};
                long r = syscall(SYS_futex, w, 0 /* FUTEX_WAIT */, 0, &t, 0, 0);
                _exit(r == 0 ? 0 : (errno == ETIMEDOUT ? 3 : 4));
            }
            for (i = 0; i < 500; i++) {
                if (syscall(SYS_futex, w, 1 /* FUTEX_WAKE */, 1, 0, 0, 0) == 1) {
                    break;
                }
                usleep(10000);
            }
            waitpid(c, &status, 0);
            if (i == 500 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                printf("SIG_FAIL: shared futex: tries=%d status=%x\n", i, status);
                ok = 0;
            }
            {
                struct timespec t = {0, 50000000};
                long r = syscall(SYS_futex, w, 128 /* FUTEX_WAIT_PRIVATE */, 0, &t, 0, 0);
                if (r != -1 || errno != ETIMEDOUT) {
                    printf("SIG_FAIL: a timed futex wait did not time out: r=%ld errno=%d\n", r, errno);
                    ok = 0;
                }
            }
            munmap((void *)w, 4096);
        }
        close(fd);
        unlink("/tmp/sig-futex");
    }

    /* clone with another exit signal: the parent is sent that, not SIGCHLD. */
    signal(SIGUSR2, on_usr2);
    g_usr2 = 0;
    c = (pid_t)syscall(SYS_clone, SIGUSR2, 0, 0, 0, 0);
    if (c == 0) {
        _exit(0);
    }
    waitpid(c, &status, __WALL);
    for (i = 0; i < 100 && !g_usr2; i++) {
        usleep(10000);
    }
    signal(SIGUSR2, SIG_DFL);
    if (c < 0 || !g_usr2) {
        printf("SIG_FAIL: clone's exit signal: c=%d usr2=%d\n", (int)c, (int)g_usr2);
        ok = 0;
    }

    /* An orphan is given to init: its parent changes when its own ends. */
    {
        int p[2];
        char r = 0;

        if (pipe(p) == 0) {
            c = fork();
            if (c == 0) {
                pid_t me = getpid();
                if (fork() == 0) {
                    for (i = 0; i < 300 && getppid() == me; i++) {
                        usleep(10000);
                    }
                    r = (getppid() != me && getppid() != 0) ? 'Y' : 'N';
                    write(p[1], &r, 1);
                    _exit(0);
                }
                _exit(0);
            }
            close(p[1]);
            waitpid(c, &status, 0);
            if (read(p[0], &r, 1) != 1 || r != 'Y') {
                printf("SIG_FAIL: an orphan was not adopted: r=%c\n", r ? r : '-');
                ok = 0;
            }
            close(p[0]);
        }
    }
    if (ok) {
        printf("PROCDEV_OK: /proc, /dev, getrandom, a shared futex, clone's exit signal, orphans\n");
        fflush(stdout);
    }
    return ok;
}

/* ---- docs/abi/ L2 step 7: what a stop means, and the orphaned process group
 * step 6's LTP run found ----------------------------------------------------- */

/* The state letter /proc/<pid>/stat gives, or 0. */
static int proc_state(pid_t pid) {
    char path[64], buf[512], *p;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    if (read_file(path, buf, sizeof(buf)) <= 0 || !(p = strrchr(buf, ')')) || p[1] != ' ') {
        return 0;
    }
    return p[2];
}

static int wait_state(pid_t pid, int want) {
    int i;

    for (i = 0; i < 300; i++) {
        if (proc_state(pid) == want) {
            return 1;
        }
        usleep(10000);
    }
    return 0;
}

static int g_job_fd = -1;

static void on_usr1_note(int s) {
    (void)s;
    (void)write(g_job_fd, "U", 1);
}

/* A stopped process stays stopped when another signal comes, and takes it when
 * it is continued. Any signal used to wake it, and it ran its handler - and the
 * rest of its program - with nobody having sent SIGCONT.
 *
 * Twice: a child waiting in pause() takes its stop on a system call's way out,
 * and one running its program takes it on the timer's. Different code holds
 * each (the system call's exit waits; the timer's path relies on nothing waking
 * a stopped task), and a sabotage of the second went red only through the
 * first's transition table until the running child was asked about too. */
static int stop_holds(int running) {
    const char *how = running ? "running" : "waiting";
    int ok = 1, status = 0, p[2], i;
    pid_t c;
    char r = 0;

    if (pipe(p) != 0) {
        printf("SIG_FAIL: jobs: pipe errno=%d\n", errno);
        return 0;
    }
    c = fork();
    if (c == 0) {
        close(p[0]);
        g_job_fd = p[1];
        signal(SIGUSR1, on_usr1_note);
        (void)write(p[1], "R", 1);   /* the handler is in place */
        for (;;) {
            if (!running) {
                pause();
            }
        }
    }
    close(p[1]);
    if (read(p[0], &r, 1) != 1 || r != 'R' || (!running && !wait_state(c, 'S')) ||
        kill(c, SIGSTOP) != 0 || !wait_state(c, 'T')) {
        printf("SIG_FAIL: a stopped child (%s) never showed T: %c\n", how, proc_state(c) ? proc_state(c) : '-');
        ok = 0;
    }
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    kill(c, SIGUSR1);
    usleep(200000);
    if (read(p[0], &r, 1) == 1 || proc_state(c) != 'T') {
        printf("SIG_FAIL: a stopped child (%s) ran on a SIGUSR1: state %c\n", how,
               proc_state(c) ? proc_state(c) : '-');
        ok = 0;
    }
    kill(c, SIGCONT);
    for (i = 0; i < 300 && read(p[0], &r, 1) != 1; i++) {
        usleep(10000);
    }
    if (i == 300) {
        printf("SIG_FAIL: the SIGUSR1 was not taken after SIGCONT (%s)\n", how);
        ok = 0;
    }
    kill(c, SIGSTOP);
    (void)wait_state(c, 'T');
    kill(c, SIGKILL);
    if (waitpid(c, &status, 0) != c || !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        printf("SIG_FAIL: SIGKILL did not end a stopped child (%s): status=%x\n", how, status);
        ok = 0;
    }
    close(p[0]);
    return ok;
}

static int job_checks(void) {
    int ok = 1, status = 0, p[2];
    pid_t c, k = 0;
    char r = 0;

    printf("SIG_PHASE: jobs\n");
    fflush(stdout);

    ok &= stop_holds(0);
    ok &= stop_holds(1);

    /* A group left orphaned with a stopped member is sent SIGHUP and then
     * SIGCONT, as POSIX requires: the middle process puts itself in a group
     * of its own, stops a child there and ends - and with it the group's last
     * parent outside the group. The child holds the pipe's write end, so its
     * death is the end of the pipe; continued without the hangup, it writes. */
    if (pipe(p) != 0) {
        printf("SIG_FAIL: jobs: pipe errno=%d\n", errno);
        return 0;
    }
    c = fork();
    if (c == 0) {
        close(p[0]);
        setpgid(0, 0);
        k = fork();
        if (k == 0) {
            kill(getpid(), SIGSTOP);
            (void)write(p[1], "C", 1);
            _exit(0);
        }
        (void)write(p[1], &k, sizeof(k));
        _exit(wait_state(k, 'T') ? 0 : 2);
    }
    close(p[1]);
    waitpid(c, &status, 0);
    if (read(p[0], &k, sizeof(k)) != (ssize_t)sizeof(k) || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("SIG_FAIL: orphaned group: the child did not stop: status=%x\n", status);
        ok = 0;
    } else {
        struct pollfd pf = {p[0], POLLIN, 0};
        int n = poll(&pf, 1, 3000);

        r = 0;
        if (n != 1 || read(p[0], &r, 1) != 0) {
            printf("SIG_FAIL: a stopped child in an orphaned group was not hung up: poll=%d r=%c state=%c\n",
                   n, r ? r : '-', proc_state(k) ? proc_state(k) : '-');
            kill(k, SIGKILL);
            ok = 0;
        }
    }
    close(p[0]);
    if (ok) {
        printf("JOBS_OK: a stop holds until SIGCONT, orphaned groups are hung up\n");
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
    /* ---- L2 step 3: timers ---------------------------------------------------- */
    ok &= timer_checks();
    /* ---- L2 step 4: limits ---------------------------------------------------- */
    ok &= limit_checks();
    /* ---- L2 step 5: processes ------------------------------------------------- */
    ok &= process_checks();
    ok &= procdev_checks();
    /* ---- L2 step 7: stops, and orphaned process groups --------------------- */
    ok &= job_checks();

    printf(ok ? "SIG_OK: handlers, masking, ignoring and default actions\n"
              : "SIG_FAIL: see above\n");
    fflush(stdout);
    return ok ? 0 : 1;
}
