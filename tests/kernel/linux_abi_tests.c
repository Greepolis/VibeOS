/* Host tests for the Linux syscall handlers (docs/abi/ phase A2).
 *
 * The handlers in kernel/abi/linux run here as they run in the kernel, entered
 * through linux_syscall with the six arguments a program passes, on the kernel
 * services of tests/kernel/ksvc_fake.c. Until A2 none of this could run anywhere
 * but a booted guest.
 *
 * Two kinds of test:
 *
 *   - handlers: what works, asserted the way a program would see it;
 *   - gaps: every row the registry calls PARTIAL, written as what Linux does.
 *     Each fails today for the reason the registry names, and that failure is
 *     the expectation - a gap that starts passing is a gap somebody closed, and
 *     this test then fails until the registry line says DONE and the gap
 *     leaves this file. The phase that owns it (L1..L9) turns it green. A row
 *     whose phase is R is partial by decision; its test asserts the decision.
 *
 * Every PARTIAL line in kernel/abi/linux_syscalls.def must have one, which is
 * checked against the registry itself at the end. */

#include <stdio.h>
#include <string.h>

#include "ksvc_fake.h"
#include "vibeos/abi_linux.h"
#include "vibeos/linux_exports.h"

int test_linux_handlers(void);
int test_linux_gaps(void);

static int g_fail;

static void expect(int cond, const char *what) {
    if (!cond) {
        printf("FAIL:linux_abi %s\n", what);
        g_fail = 1;
    }
}

static long sys(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                uint64_t a3, uint64_t a4, uint64_t a5, kf_outcome_t *out) {
    return kf_call(linux_syscall, nr, a0, a1, a2, a3, a4, a5, out);
}

#define SYS0(nr)             sys((nr), 0, 0, 0, 0, 0, 0, 0)
#define SYS1(nr, a)          sys((nr), (a), 0, 0, 0, 0, 0, 0)
#define SYS2(nr, a, b)       sys((nr), (a), (b), 0, 0, 0, 0, 0)
#define SYS3(nr, a, b, c)    sys((nr), (a), (b), (c), 0, 0, 0, 0)

static uint64_t ustr(const char *s) {
    uint64_t u = kf_ualloc(strlen(s) + 1u);
    memcpy(kf_uptr(u), s, strlen(s) + 1u);
    return u;
}

static int fresh(uint32_t pid) {
    int slot;
    kf_reset();
    vibeos_linux_abi_init();
    slot = kf_spawn(pid, pid);
    kf_set_current(slot);
    return slot;
}

/* ---- handlers ------------------------------------------------------------------ */

static void t_identity(void) {
    fresh(42);
    expect(SYS0(39) == 42, "getpid is the thread group");
    expect(SYS0(186) == 42, "gettid of a single-threaded process is its pid");
    expect(SYS0(102) == 0 && SYS0(104) == 0, "one identity, root");
}

static void t_pipe_round_trip(void) {
    uint64_t fds, buf, in;
    int32_t *f;
    kf_outcome_t how;

    fresh(10);
    fds = kf_ualloc(8);
    buf = kf_ualloc(16);
    in = ustr("pipe!");
    f = (int32_t *)kf_uptr(fds);
    expect(SYS2(293, fds, 0) == 0, "pipe2 succeeds");
    expect(f[0] >= 3 && f[1] >= 3 && f[0] != f[1], "pipe2 hands out two descriptors");
    expect(SYS3(1, (uint64_t)f[1], in, 5) == 5, "a pipe takes a write");
    expect(SYS3(0, (uint64_t)f[0], buf, 16) == 5 &&
           memcmp(kf_uptr(buf), "pipe!", 5) == 0, "and gives it back to a read");
    /* Empty with a writer: the read waits, and nothing here would wake it. */
    (void)sys(0, (uint64_t)f[0], buf, 16, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "an empty pipe with a writer waits");
    expect(SYS1(3, (uint64_t)f[1]) == 0, "close the write end");
    expect(SYS3(0, (uint64_t)f[0], buf, 16) == 0, "then the reader sees end of file");
    expect(SYS1(3, (uint64_t)f[1]) == -VIBEOS_EBADF, "a closed descriptor is EBADF");
    expect(kf_lock_imbalance() == 0, "every lock taken was released");
}

static void t_dup2_redirects_stdout(void) {
    uint64_t fds, buf, in;
    int32_t *f;

    fresh(11);
    fds = kf_ualloc(8);
    buf = kf_ualloc(16);
    in = ustr("to pipe");
    f = (int32_t *)kf_uptr(fds);
    (void)SYS2(293, fds, 0);
    expect(SYS2(33, (uint64_t)f[1], 1) == 1, "dup2 onto stdout");
    expect(SYS3(1, 1, in, 7) == 7, "stdout now writes into the pipe");
    expect(SYS3(0, (uint64_t)f[0], buf, 16) == 7 &&
           memcmp(kf_uptr(buf), "to pipe", 7) == 0, "and the pipe has it");
    expect(strstr(kf_console(), "to pipe") == 0, "not the console");
}

static void t_write_console(void) {
    uint64_t in;
    fresh(12);
    in = ustr("hello\n");
    expect(SYS3(1, 1, in, 6) == 6, "write to the console");
    expect(strstr(kf_console(), "hello") != 0, "the console has it");
}

static void t_pointer_engine(void) {
    uint64_t fds;

    fresh(13);
    expect(SYS3(1, 1, 0x1000, 4) == -VIBEOS_EFAULT, "a buffer outside user memory is EFAULT");
    expect(SYS2(293, 0x1000, 0) == -VIBEOS_EFAULT, "so is pipe's result array");
    /* The range was fine when checked and went away before the copy - a
     * sibling's munmap. The copy fails; the kernel does not. */
    fds = kf_ualloc(8);
    kf_fault(fds, 8);
    expect(SYS2(293, fds, 0) == -VIBEOS_EFAULT, "a copy-out that faults is EFAULT");
    kf_fault(0, 0);
    expect(kf_lock_imbalance() == 0, "and leaves no lock held");
}

static void t_sigprocmask_numbering(void) {
    int me = fresh(14);
    uint64_t set = kf_ualloc(8), old = kf_ualloc(8);

    /* Linux numbers a sigset from bit 0 for signal 1; SIGUSR2 is 12, bit 11. */
    *(uint64_t *)kf_uptr(set) = 1ull << 11;
    expect(SYS3(14, 0, set, 0) == 0, "block SIGUSR2");
    expect(ks_id(me)->sig_blocked == (1ull << 12), "the kernel holds it by signal number");
    expect(SYS3(14, 0, 0, old) == 0 && *(uint64_t *)kf_uptr(old) == (1ull << 11),
           "and reports it back in Linux numbering");
    *(uint64_t *)kf_uptr(set) = (1ull << 8) | (1ull << 18);   /* SIGKILL, SIGSTOP */
    (void)SYS3(14, 2, set, 0);
    expect(ks_id(me)->sig_blocked == 0, "SIGKILL and SIGSTOP cannot be blocked");
}

static void t_sigaction_round_trip(void) {
    uint64_t act, old, handler;
    uint64_t *a, *o;

    fresh(15);
    act = kf_ualloc(32);
    old = kf_ualloc(32);
    handler = kf_ualloc(16);
    a = (uint64_t *)kf_uptr(act);
    o = (uint64_t *)kf_uptr(old);
    a[0] = handler;
    a[1] = VIBEOS_SA_RESTORER;
    a[2] = handler + 8u;
    a[3] = 1ull << 1;   /* SIGINT blocked while it runs */
    expect(SYS3(13, 10, act, 0) == 0, "install a SIGUSR1 handler");
    expect(SYS3(13, 10, 0, old) == 0 && o[0] == handler && o[3] == (1ull << 1),
           "and read it back as installed");
    expect(SYS3(13, 9, act, 0) == -VIBEOS_EINVAL, "SIGKILL cannot be caught");
    a[0] = 0x1000;
    expect(SYS3(13, 10, act, 0) == -VIBEOS_EINVAL, "a handler outside user memory is refused (H-017)");
}

/* A frame rt_sigreturn cannot trust ends the task with SIGSEGV rather than
 * resuming from whatever is on the stack (H-018). */
static void t_sigreturn_forged(void) {
    kf_outcome_t how;
    uint64_t junk;

    fresh(21);
    junk = kf_ualloc(256);
    memset(kf_uptr(junk), 0x41, 256);
    kf_next_sp(junk);
    (void)sys(15, 0, 0, 0, 0, 0, 0, &how);
    expect(how == KF_EXITED && kf_exit_code() == 128u + 11u,
           "a forged signal frame ends the task with SIGSEGV");
    kf_next_sp(0x1000);
    (void)sys(15, 0, 0, 0, 0, 0, 0, &how);
    expect(how == KF_EXITED && kf_exit_code() == 128u + 11u,
           "so does a frame outside user memory");
}

static void t_kill_permission(void) {
    int a, b;

    kf_reset();
    vibeos_linux_abi_init();
    a = kf_spawn(20, 20);
    b = kf_spawn(30, 30);   /* another session */
    kf_set_current(a);
    expect(SYS2(62, 30, 15) == -VIBEOS_EPERM, "no signal across sessions");
    expect(SYS2(62, 20, 0) == 0, "a process may probe itself");
    expect(SYS2(62, 999, 0) == -VIBEOS_ESRCH, "no such process");
    ks_id(b)->sid = 20;
    expect(SYS2(62, 30, 15) == 0 && (ks_id(b)->sig_pending & (1ull << 15)) != 0,
           "within a session it is delivered");
}

static void t_registry_answers(void) {
    uint64_t before;

    fresh(16);
    before = g_abi_refused;
    expect(SYS3(172, 3, 0, 0) == -VIBEOS_EPERM, "iopl is refused with its registry errno");
    expect(g_abi_refused == before + 1u, "and counted as a refusal");
    before = g_abi_deferred;
    expect(SYS0(101) == -VIBEOS_ENOSYS && g_abi_deferred == before + 1u &&
           g_abi_deferred_nr == 101u, "ptrace is deferred, counted and named");
    before = g_abi_unimplemented;
    expect(SYS0(80) == -VIBEOS_ENOSYS && g_abi_unimplemented == before + 1u &&
           g_abi_last_nr == 80u, "chdir is missing, counted and named");
}

static void t_uname_and_clock(void) {
    uint64_t u, ts;
    const char *p;

    fresh(17);
    u = kf_ualloc(6u * 65u);
    ts = kf_ualloc(16);
    expect(SYS1(63, u) == 0, "uname");
    p = (const char *)kf_uptr(u);
    expect(strcmp(p, "Linux") == 0 && strcmp(p + 4 * 65, "x86_64") == 0,
           "uname says Linux on x86_64");
    expect(SYS2(228, 1, ts) == 0, "clock_gettime");
}

static void t_exit_and_wait(void) {
    kf_outcome_t how;
    uint64_t st;

    fresh(18);
    (void)sys(60, 7, 0, 0, 0, 0, 0, &how);
    expect(how == KF_EXITED && kf_exit_code() == 7, "exit ends the task with its code");
    fresh(19);
    st = kf_ualloc(4);
    expect(SYS3(61, (uint64_t)-1, st, 1) == -VIBEOS_ECHILD, "wait with no children is ECHILD");
}

static void t_fork_publishes_a_child(void) {
    int parent = fresh(50), child = -1;
    uint32_t i;
    long pid;

    pid = SYS0(57);

    expect(pid > 0 && pid != 50, "fork returns the child's pid");
    for (i = 0; i < KF_SLOTS; i++) {
        if ((int)i != parent && ks_id((int)i)->pid == (uint32_t)pid) {
            child = (int)i;
        }
    }
    expect(child >= 0 && vibeos_task_state((uint32_t)child) == VIBEOS_TASK_READY,
           "a runnable child");
    expect(child >= 0 && ks_id(child)->ppid == 50 && ks_ps(child) != ks_ps(parent),
           "whose parent is the caller, with a process of its own");
    expect(kf_illegal_transitions() == 0, "through legal transitions only");
}

int test_linux_handlers(void) {
    g_fail = 0;
    t_identity();
    t_pipe_round_trip();
    t_dup2_redirects_stdout();
    t_write_console();
    t_pointer_engine();
    t_sigprocmask_numbering();
    t_sigaction_round_trip();
    t_sigreturn_forged();
    t_kill_permission();
    t_registry_answers();
    t_uname_and_clock();
    t_exit_and_wait();
    t_fork_publishes_a_child();
    return g_fail ? -1 : 0;
}

/* ---- gaps ----------------------------------------------------------------------------- */

static uint32_t g_gap_nr[64];
static uint32_t g_gaps;

/* `linux_ok` is what Linux does, evaluated against this kernel. False is the gap
 * the registry describes, still open, which is expected. True is a gap closed:
 * the registry line should now say DONE, and this expectation should go. */
static void gap(uint32_t nr, int linux_ok, const char *what) {
    if (g_gaps < sizeof(g_gap_nr) / sizeof(g_gap_nr[0])) {
        g_gap_nr[g_gaps++] = nr;
    }
    if (linux_ok) {
        printf("FAIL:linux_gap %u closed - %s: mark the registry line DONE and "
               "remove this gap\n", nr, what);
        g_fail = 1;
    }
}

/* A row that is partial by decision (phase R): assert the decision itself. */
static void decided(uint32_t nr, int holds, const char *what) {
    if (g_gaps < sizeof(g_gap_nr) / sizeof(g_gap_nr[0])) {
        g_gap_nr[g_gaps++] = nr;
    }
    expect(holds, what);
}

static int gap_covered(uint32_t nr) {
    uint32_t i;
    for (i = 0; i < g_gaps; i++) {
        if (g_gap_nr[i] == nr) {
            return 1;
        }
    }
    return 0;
}

int test_linux_gaps(void) {
    uint32_t i;
    long r;
    kf_outcome_t how;

    g_fail = 0;
    g_gaps = 0;

    /* open (2), L1: a process has at least 64 descriptors on Linux. */
    {
        uint64_t path = 0;
        int opened = 0;
        fresh(60);
        path = ustr("f.txt");
        for (i = 0; i < 16u; i++) {
            if (SYS3(2, path, 0101u /* O_WRONLY|O_CREAT */, 0644u) >= 3) {
                opened++;
            }
        }
        gap(2, opened == 16, "sixteen files open at once");
    }

    /* mmap (9), L3: MAP_FIXED|MAP_ANONYMOUS maps at the address given. */
    fresh(61);
    r = sys(9, 0x30000000ull, 4096, 3, 0x32 /* MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS */,
            (uint64_t)-1, 0, 0);
    gap(9, r == 0x30000000l, "MAP_FIXED maps at the address asked for");

    /* ioctl (16), L1: TCGETS on a terminal succeeds. */
    fresh(62);
    gap(16, SYS3(16, 0, 0x5401u, kf_ualloc(60)) == 0, "TCGETS on the console");

    /* readv (19) and writev (20), L5: a datagram is one read, scattered; a
     * gathered write is one datagram. */
    {
        uint64_t iov, b1, b2, sa;
        uint64_t *v;
        uint8_t *s;
        long fd;
        fresh(63);
        kf_net_up(0x0A00020Fu, 0x0A000202u);
        iov = kf_ualloc(32);
        b1 = kf_ualloc(5);
        b2 = kf_ualloc(5);
        sa = kf_ualloc(16);
        s = (uint8_t *)kf_uptr(sa);
        v = (uint64_t *)kf_uptr(iov);
        s[0] = 2; s[1] = 0; s[2] = 0x10; s[3] = 0x92;   /* AF_INET, port 4242 */
        fd = SYS2(41, 2, 2);                              /* UDP */
        expect(fd >= 3, "a UDP socket");
        expect(SYS2(49, (uint64_t)fd, sa) == 0, "bound to 4242");
        kf_net_deliver_udp(0x0A000202u, 9999, 4242, "helloworld", 10);
        v[0] = b1; v[1] = 5; v[2] = b2; v[3] = 5;
        r = sys(19, (uint64_t)fd, iov, 2, 0, 0, 0, &how);
        gap(19, how == KF_RETURNED && r == 10 &&
                memcmp(kf_uptr(b1), "hello", 5) == 0 && memcmp(kf_uptr(b2), "world", 5) == 0,
            "readv scatters one datagram");

        s[2] = 0x27; s[3] = 0x0F;                          /* the peer's 9999 */
        s[4] = 10; s[5] = 0; s[6] = 2; s[7] = 2;
        {
            long c = sys(42, (uint64_t)fd, sa, 16, 0, 0, 0, &how);
            uint32_t sent_before = kf_net_udp_sent(0), last = 0;
            memcpy(kf_uptr(b1), "hello", 5);
            memcpy(kf_uptr(b2), "world", 5);
            r = (c == 0) ? sys(20, (uint64_t)fd, iov, 2, 0, 0, 0, &how) : c;
            gap(20, c == 0 && r == 10 && kf_net_udp_sent(&last) == sent_before + 1u && last == 10u,
                "writev on a connected UDP socket sends one datagram");
        }
    }

    /* sendfile (40), L1: a count of zero copies nothing and succeeds. */
    fresh(64);
    kf_fs_add("/src", "abc", 3, 0);
    {
        long in = SYS2(2, ustr("src"), 0);
        gap(40, sys(40, 1, (uint64_t)in, 0, 0, 0, 0, 0) == 0, "sendfile of zero bytes");
    }

    /* clone (56), L6: vfork-like sharing - CLONE_VM|CLONE_VFORK|SIGCHLD - makes a child. */
    fresh(65);
    r = sys(56, 0x4111u, 0, 0, 0, 0, 0, &how);
    gap(56, how == KF_RETURNED && r > 0, "clone(CLONE_VM|CLONE_VFORK) creates a child");

    /* uname (63), L8: sethostname changes what uname reports. */
    {
        uint64_t u = 0;
        fresh(66);
        u = kf_ualloc(6u * 65u);
        (void)SYS2(170, ustr("box"), 3);
        (void)SYS1(63, u);
        gap(63, strcmp((const char *)kf_uptr(u) + 65, "box") == 0, "uname reports the hostname set");
    }

    /* getcwd (79), L1: after chdir, getcwd says where. */
    {
        uint64_t buf = 0;
        fresh(67);
        kf_fs_add("/bin", 0, 0, 1);
        buf = kf_ualloc(32);
        r = SYS1(80, ustr("/bin"));
        (void)SYS2(79, buf, 32);
        gap(79, r == 0 && strcmp((const char *)kf_uptr(buf), "/bin") == 0, "getcwd after chdir");
    }

    /* setuid (105) and setgid (106), L2: root may become another user. */
    fresh(68);
    gap(105, SYS1(105, 1000) == 0 && SYS0(102) == 1000, "setuid(1000) from root");
    fresh(69);
    gap(106, SYS1(106, 1000) == 0 && SYS0(104) == 1000, "setgid(1000) from root");

    /* futex (202), L6: FUTEX_CMP_REQUEUE with nobody waiting requeues nobody. */
    {
        uint64_t w = 0;
        fresh(70);
        w = kf_ualloc(8);
        r = sys(202, w, 4 /* FUTEX_CMP_REQUEUE */, 1, 1, w + 4u, 0, 0);
        gap(202, r == 0, "FUTEX_CMP_REQUEUE");
    }

    /* getdents64 (217), L1: names longer than FAT's are listed whole. */
    {
        uint64_t buf = 0;
        long fd;
        fresh(71);
        kf_fs_add("/d", 0, 0, 1);
        kf_fs_add("/d/a_name_longer_than_fat.txt", "x", 1, 0);
        buf = kf_ualloc(256);
        fd = SYS2(2, ustr("d"), 0);
        r = SYS3(217, (uint64_t)fd, buf, 256);
        gap(217, r > 19 && strcmp((const char *)kf_uptr(buf) + 19, "a_name_longer_than_fat.txt") == 0,
            "getdents64 lists a long name whole");
    }

    /* openat (257), L1: a path relative to a directory descriptor. */
    {
        long dfd;
        fresh(72);
        kf_fs_add("/etc", 0, 0, 1);
        kf_fs_add("/etc/motd", "hi", 2, 0);
        dfd = SYS2(2, ustr("etc"), 0);
        gap(257, SYS3(257, (uint64_t)dfd, ustr("motd"), 0) >= 3, "openat relative to a directory");
    }

    /* prlimit64 (302), L2: a limit that is set is the limit reported. */
    {
        uint64_t nl = 0, ol = 0;
        fresh(73);
        nl = kf_ualloc(16);
        ol = kf_ualloc(16);
        ((uint64_t *)kf_uptr(nl))[0] = 10;
        ((uint64_t *)kf_uptr(nl))[1] = 10;
        r = sys(302, 0, 7 /* RLIMIT_NOFILE */, nl, 0, 0, 0, 0);
        (void)sys(302, 0, 7, 0, ol, 0, 0, 0);
        gap(302, r == 0 && ((uint64_t *)kf_uptr(ol))[0] == 10u, "prlimit64 sets RLIMIT_NOFILE");
    }

    /* rseq (334), R: ENOSYS by decision - the library takes its fallback. */
    fresh(74);
    decided(334, sys(334, kf_ualloc(32), 32, 0, 0x53053053u, 0, 0, 0) == -VIBEOS_ENOSYS,
            "rseq answers ENOSYS, by decision");

    /* Every PARTIAL line has an expectation here, and nothing here is for a
     * line that is not PARTIAL - the registry is the list, this file follows. */
    for (i = 0; i < vibeos_linux_syscall_count(); i++) {
        const vibeos_sys_entry_t *e = vibeos_linux_syscall_at(i);
        if (e->state == VIBEOS_SYS_PARTIAL && !gap_covered(e->nr)) {
            printf("FAIL:linux_gap %u (%s) is PARTIAL and has no expectation here\n", e->nr, e->name);
            g_fail = 1;
        }
    }
    for (i = 0; i < g_gaps; i++) {
        const vibeos_sys_entry_t *e = vibeos_linux_syscall(g_gap_nr[i]);
        if (!e || e->state != VIBEOS_SYS_PARTIAL) {
            printf("FAIL:linux_gap %u has an expectation and is not PARTIAL\n", g_gap_nr[i]);
            g_fail = 1;
        }
    }
    expect(kf_lock_imbalance() == 0, "the gap calls released every lock they took");
    return g_fail ? -1 : 0;
}
