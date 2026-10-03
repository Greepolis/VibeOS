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

    printf(ok ? "SIG_OK: handlers, masking, ignoring and default actions\n"
              : "SIG_FAIL: see above\n");
    fflush(stdout);
    return ok ? 0 : 1;
}
