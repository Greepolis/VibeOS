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
#include <stdlib.h>
#include <string.h>

#include "ksvc_fake.h"
#include "vibeos/abi_linux.h"
#include "vibeos/linux_exports.h"
#include "vibeos/linux_layout.h"
#include "vibeos/vfs.h"
#include "vibeos/ksvc.h"
#include "vibeos/mm_stats.h"
#include "vibeos/frame.h"
#include "vibeos/vmspace.h"
#include "vibeos/mbz.h"
#include "vibeos/random.h"

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
#define SYS4(nr, a, b, c, d) sys((nr), (a), (b), (c), (d), 0, 0, 0)

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
    expect(SYS4(14, 0, set, 0, 8) == 0, "block SIGUSR2");
    expect(ks_id(me)->sig_blocked == (1ull << 12), "the kernel holds it by signal number");
    expect(SYS4(14, 0, 0, old, 8) == 0 && *(uint64_t *)kf_uptr(old) == (1ull << 11),
           "and reports it back in Linux numbering");
    *(uint64_t *)kf_uptr(set) = (1ull << 8) | (1ull << 18);   /* SIGKILL, SIGSTOP */
    (void)SYS4(14, 2, set, 0, 8);
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
    expect(SYS4(13, 10, act, 0, 8) == 0, "install a SIGUSR1 handler");
    expect(SYS4(13, 10, 0, old, 8) == 0 && o[0] == handler && o[3] == (1ull << 1),
           "and read it back as installed");
    expect(SYS4(13, 9, act, 0, 8) == -VIBEOS_EINVAL, "SIGKILL cannot be caught");
    a[0] = 0x1000;
    expect(SYS4(13, 10, act, 0, 8) == -VIBEOS_EINVAL, "a handler outside user memory is refused (H-017)");
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
    /* A session is not a wall (L2 step 7): BusyBox's timeout probes its program
     * with kill(pid, 0) from a session of its own, and was refused. */
    expect(SYS2(62, 30, 0) == 0 && SYS2(62, 30, 15) == 0 && (ks_id(b)->sig_pending & (1ull << 15)) != 0,
           "the superuser signals a process of another session");
    expect(SYS2(62, 20, 0) == 0, "a process may probe itself");
    expect(SYS2(62, 999, 0) == -VIBEOS_ESRCH, "no such process");
    ks_id(b)->sig_pending = 0;
    expect(SYS1(105, 1000) == 0, "the sender gives up root");
    expect(SYS2(62, 30, 15) == -VIBEOS_EPERM && SYS2(62, 30, 18) == -VIBEOS_EPERM &&
           ks_id(b)->sig_pending == 0, "a user's signal to root's process in another session is refused, SIGCONT too");
    expect(SYS2(62, -30, 15) == -VIBEOS_EPERM && SYS2(62, -999, 15) == -VIBEOS_ESRCH,
           "so is one to its group: EPERM for a group nobody may be sent to, ESRCH for none");
    ks_id(b)->sid = 20;
    expect(SYS2(62, 30, 15) == -VIBEOS_EPERM, "within a session, owners still decide");
    expect(SYS2(62, 30, 18) == 0 && (ks_id(b)->sig_pending & (1ull << 18)) != 0,
           "except for SIGCONT, which a session allows");
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
    expect(SYS0(161) == -VIBEOS_ENOSYS && g_abi_unimplemented == before + 1u &&
           g_abi_last_nr == 161u, "chroot is missing, counted and named");
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


/* ---- descriptors as Linux has them (A3) -------------------------------------------- */

/* Two descriptors dup made share one offset - which is what a shell's `2>&1`
 * and every `exec 3<file` rely on. The table held entries by value until A3, so
 * the second read started from the beginning again. */
static void t_dup_shares_offset(void) {
    uint64_t buf;
    long a, b;

    fresh(80);
    kf_fs_add("/f", "abcdef", 6, 0);
    buf = kf_ualloc(8);
    a = SYS2(2, ustr("f"), 0);
    b = SYS1(32, (uint64_t)a);
    expect(a >= 3 && b > a, "dup gives a second descriptor");
    expect(SYS3(0, (uint64_t)a, buf, 2) == 2 && memcmp(kf_uptr(buf), "ab", 2) == 0, "read two bytes");
    expect(SYS3(0, (uint64_t)b, buf, 2) == 2 && memcmp(kf_uptr(buf), "cd", 2) == 0,
           "the duplicate continues where the original stopped");
    expect(SYS3(8, (uint64_t)b, 0, 1) == 4, "and reports the shared position");
    expect(SYS1(3, (uint64_t)a) == 0 && SYS3(0, (uint64_t)b, buf, 8) == 2,
           "closing one leaves the other open, with the offset");
}

/* A program opens two hundred files. The table held four. */
static void t_many_descriptors(void) {
    uint32_t i;
    int ok = 1;
    uint64_t path;

    fresh(81);
    kf_fs_add("/f", "x", 1, 0);
    path = ustr("f");
    for (i = 0; i < 200u; i++) {
        if (SYS2(2, path, 0) != (long)(3u + i)) {
            ok = 0;
        }
    }
    expect(ok, "two hundred files open at once, each at the next number");
    expect(SYS1(3, 50) == 0 && SYS2(2, path, 0) == 50, "a closed number is the next one handed out");
    expect(SYS1(3, 1) == 0 && SYS2(2, path, 0) == 1,
           "closing stdout and opening a file gives the file 1 - a shell's redirection");
}

/* A pipe is a FIFO, has no position, and O_NONBLOCK makes an empty read say so
 * instead of waiting. */
static void t_pipe_is_a_fifo(void) {
    uint64_t fds, st, buf;
    int32_t *f;
    kf_outcome_t how;

    fresh(82);
    fds = kf_ualloc(8);
    st = kf_ualloc(144);
    buf = kf_ualloc(8);
    f = (int32_t *)kf_uptr(fds);
    expect(SYS2(293, fds, 0x800 /* O_NONBLOCK */) == 0, "a non-blocking pipe");
    expect(SYS2(5, (uint64_t)f[0], st) == 0 &&
           (*(uint32_t *)((uint8_t *)kf_uptr(st) + 24) & 0170000u) == 0010000u,
           "fstat says FIFO, not a regular file");
    expect(SYS3(8, (uint64_t)f[0], 0, 1) == -VIBEOS_ESPIPE, "lseek on a pipe is ESPIPE");
    (void)sys(0, (uint64_t)f[0], buf, 8, 0, 0, 0, &how);
    expect(how == KF_RETURNED && sys(0, (uint64_t)f[0], buf, 8, 0, 0, 0, 0) == -VIBEOS_EAGAIN,
           "an empty non-blocking pipe answers EAGAIN instead of waiting");
    expect(SYS3(72, (uint64_t)f[0], 3 /* F_GETFL */, 0) & 0x800, "F_GETFL reports O_NONBLOCK");
    expect(SYS3(72, (uint64_t)f[0], 4 /* F_SETFL */, 0) == 0 &&
           !(SYS3(72, (uint64_t)f[0], 3, 0) & 0x800), "and F_SETFL clears it");
}

/* Close-on-exec belongs to the number, not the file. */
static void t_cloexec(void) {
    uint64_t path;
    long a, b;

    fresh(83);
    kf_fs_add("/f", "x", 1, 0);
    path = ustr("f");
    a = SYS2(2, path, 0x80000 /* O_CLOEXEC */);
    expect(SYS3(72, (uint64_t)a, 1 /* F_GETFD */, 0) == 1, "O_CLOEXEC sets FD_CLOEXEC");
    b = SYS3(292, (uint64_t)a, 40, 0);
    expect(b == 40 && SYS3(72, 40, 1, 0) == 0, "dup3 without O_CLOEXEC gives a number without it");
    expect(SYS3(292, (uint64_t)a, (uint64_t)a, 0) == -VIBEOS_EINVAL, "dup3 onto itself is EINVAL");
    expect(SYS3(72, (uint64_t)a, 1030 /* F_DUPFD_CLOEXEC */, 100) == 100 && SYS3(72, 100, 1, 0) == 1,
           "F_DUPFD_CLOEXEC honours its minimum and the flag");
    expect(SYS3(436, 40, 100, 4 /* CLOSE_RANGE_CLOEXEC */) == 0 && SYS3(72, 40, 1, 0) == 1,
           "close_range can mark a range close-on-exec");
    expect(SYS3(436, 40, ~0ull, 0) == 0 && SYS1(3, 40) == -VIBEOS_EBADF && SYS1(3, 100) == -VIBEOS_EBADF,
           "and close it");
    expect(SYS2(33, (uint64_t)a, (uint64_t)a) == a, "dup2 onto itself answers the number");
}

/* A forked child shares its parent's descriptions: one offset between them. */
static void t_fork_shares_offsets(void) {
    uint64_t buf;
    long fd, pid;
    int parent, child = -1;
    uint32_t i;

    parent = fresh(84);
    kf_fs_add("/f", "0123456789", 10, 0);
    buf = kf_ualloc(8);
    fd = SYS2(2, ustr("f"), 0);
    pid = SYS0(57);
    for (i = 0; i < KF_SLOTS; i++) {
        if ((int)i != parent && ks_id((int)i)->pid == (uint32_t)pid) {
            child = (int)i;
        }
    }
    expect(child >= 0, "a child");
    if (child < 0) {
        return;
    }
    (void)SYS3(0, (uint64_t)fd, buf, 3);
    kf_set_current(child);
    expect(SYS3(0, (uint64_t)fd, buf, 3) == 3 && memcmp(kf_uptr(buf), "345", 3) == 0,
           "the child reads on from where the parent stopped");
    kf_set_current(parent);
}

/* ---- paths (A4) --------------------------------------------------------------------- */

/* The working directory is where relative paths start, and getcwd says where it
 * is. Until A4 there was none: getcwd answered "/" and chdir was ENOSYS. */
static void t_working_directory(void) {
    uint64_t buf;

    fresh(90);
    kf_fs_add("/usr", 0, 0, 1);
    kf_fs_add("/usr/share", 0, 0, 1);
    kf_fs_add("/usr/share/note", "hi", 2, 0);
    buf = kf_ualloc(64);
    expect(SYS1(80, ustr("/usr/share")) == 0, "chdir into a directory");
    expect(SYS2(79, buf, 64) == 11 && strcmp((const char *)kf_uptr(buf), "/usr/share") == 0,
           "getcwd answers it, with the terminator counted");
    expect(SYS2(79, buf, 5) == -VIBEOS_ERANGE, "a buffer too small is ERANGE");
    expect(SYS2(2, ustr("note"), 0) >= 3, "a relative open starts there");
    expect(SYS2(2, ustr("../share/./note"), 0) >= 3, "'..' and '.' are walked");
    expect(SYS1(80, ustr("..")) == 0 && SYS2(79, buf, 64) == 5 &&
           strcmp((const char *)kf_uptr(buf), "/usr") == 0, "chdir .. goes up one");
    expect(SYS1(80, ustr("../../../..")) == 0 && SYS2(79, buf, 64) == 2,
           "and never above the root");
    expect(SYS1(80, ustr("/usr/share/note")) == -VIBEOS_ENOTDIR, "chdir onto a file is ENOTDIR");
    expect(SYS1(80, ustr("/nowhere")) == -VIBEOS_ENOENT, "chdir onto nothing is ENOENT");
}

/* The errors a walk reports, as Linux reports them. */
static void t_path_errors(void) {
    char longname[300];
    uint32_t i;

    fresh(91);
    kf_fs_add("/f", "x", 1, 0);
    expect(SYS2(2, ustr("/f/x"), 0) == -VIBEOS_ENOTDIR, "a file used as a directory is ENOTDIR");
    expect(SYS2(2, ustr("/no/x"), 0) == -VIBEOS_ENOENT, "a missing directory is ENOENT");
    expect(SYS2(2, ustr(""), 0) == -VIBEOS_ENOENT, "an empty path is ENOENT");
    for (i = 0; i < sizeof(longname) - 1u; i++) {
        longname[i] = 'a';
    }
    longname[sizeof(longname) - 1u] = 0;
    expect(SYS2(2, ustr(longname), 0) == -VIBEOS_ENAMETOOLONG, "a path over PATH_MAX is ENAMETOOLONG");
}

/* The *at calls resolve against a directory descriptor, and fchdir moves there. */
static void t_at_calls(void) {
    uint64_t buf, st;
    long dfd, ffd;

    fresh(92);
    kf_fs_add("/etc", 0, 0, 1);
    kf_fs_add("/etc/motd", "hello", 5, 0);
    buf = kf_ualloc(64);
    st = kf_ualloc(144);
    dfd = SYS2(2, ustr("/etc"), 0);
    ffd = SYS2(2, ustr("/etc/motd"), 0);
    expect(SYS3(257, (uint64_t)dfd, ustr("motd"), 0) >= 3, "openat relative to a directory descriptor");
    expect(sys(262, (uint64_t)dfd, ustr("motd"), st, 0, 0, 0, 0) == 0 &&
           *(uint64_t *)((uint8_t *)kf_uptr(st) + 48) == 5u, "newfstatat relative to it");
    expect(SYS3(257, (uint64_t)ffd, ustr("motd"), 0) == -VIBEOS_ENOTDIR,
           "a file descriptor as dirfd is ENOTDIR");
    expect(SYS3(257, 77, ustr("motd"), 0) == -VIBEOS_EBADF, "a closed one is EBADF");
    expect(SYS3(257, 77, ustr("/etc/motd"), 0) >= 3, "an absolute path ignores dirfd");
    expect(SYS2(258, (uint64_t)dfd, ustr("new.d")) == 0, "mkdirat relative to a directory");
    expect(SYS2(258, (uint64_t)dfd, ustr("new.d")) == -VIBEOS_EEXIST, "twice is EEXIST");
    expect(SYS3(263, (uint64_t)dfd, ustr("new.d"), 0) == -VIBEOS_EISDIR, "unlink of a directory is EISDIR");
    expect(SYS1(81, (uint64_t)dfd) == 0 && SYS2(79, buf, 64) == 5 &&
           strcmp((const char *)kf_uptr(buf), "/etc") == 0, "fchdir moves to the descriptor's directory");
    expect(SYS1(81, (uint64_t)ffd) == -VIBEOS_ENOTDIR, "fchdir on a file is ENOTDIR");
    expect(SYS3(263, (uint64_t)(uint32_t)-100, ustr("motd"), 0) == 0 &&
           SYS2(2, ustr("motd"), 0) == -VIBEOS_ENOENT, "unlinkat from the working directory");
}

/* ---- L1 step 3: writing through a descriptor, on /tmp ------------------------------ */


static long tmp_open(const char *path, uint64_t flags, uint64_t mode) {
    return SYS3(2, ustr(path), flags, mode);
}

/* The size and mode a path's stat reports. */
static uint64_t tmp_size(const char *path) {
    uint64_t st = kf_ualloc(144);
    if (sys(262, (uint64_t)(uint32_t)-100, ustr(path), st, 0, 0, 0, 0) != 0) {
        return ~0ull;
    }
    return *(uint64_t *)((uint8_t *)kf_uptr(st) + 48);
}

static uint32_t tmp_mode(const char *path) {
    uint64_t st = kf_ualloc(144);
    if (sys(262, (uint64_t)(uint32_t)-100, ustr(path), st, 0, 0, 0, 0) != 0) {
        return ~0u;
    }
    return *(uint32_t *)((uint8_t *)kf_uptr(st) + 24);
}

/* open's flags mean what Linux means by them. */
static void t_open_flags(void) {
    uint64_t buf = 0, big = 0;
    long fd, fd2;
    uint32_t i;

    fresh(93);
    buf = kf_ualloc(64);
    big = kf_ualloc(5000);
    expect(tmp_open("/tmp/a", 0, 0) == -VIBEOS_ENOENT, "opening a missing file is ENOENT");
    expect(tmp_open("/tmp/a", 1, 0) == -VIBEOS_ENOENT,
           "O_WRONLY without O_CREAT does not make one - it used to, on release");
    fd = tmp_open("/tmp/a", 0x41 /* O_CREAT|O_WRONLY */, 0644);
    expect(fd >= 3 && tmp_size("/tmp/a") == 0u, "O_CREAT makes the file now, not when it is closed");
    expect(tmp_open("/tmp/a", 0xC1 /* O_CREAT|O_EXCL|O_WRONLY */, 0644) == -VIBEOS_EEXIST,
           "O_EXCL refuses an existing file");
    memcpy(kf_uptr(buf), "hello", 5);
    expect(SYS3(1, (uint64_t)fd, buf, 5) == 5 && tmp_size("/tmp/a") == 5u,
           "a write is in the file when write returns");
    for (i = 0; i < 5000u; i++) {
        ((uint8_t *)kf_uptr(big))[i] = (uint8_t)(i * 13u + 1u);
    }
    expect(SYS3(1, (uint64_t)fd, big, 5000) == 5000 && tmp_size("/tmp/a") == 5005u,
           "a write longer than 512 bytes is whole - the old buffer's ceiling is gone");
    fd2 = tmp_open("/tmp/a", 0, 0);
    expect(SYS3(8, (uint64_t)fd2, 5, 0) == 5 && SYS3(0, (uint64_t)fd2, big, 5000) == 5000 &&
           ((uint8_t *)kf_uptr(big))[4999] == (uint8_t)(4999u * 13u + 1u),
           "and reads back through another descriptor");
    /* O_RDWR reads what it wrote. */
    fd = tmp_open("/tmp/a", 2 /* O_RDWR */, 0);
    memcpy(kf_uptr(buf), "HE", 2);
    expect(fd >= 3 && SYS3(1, (uint64_t)fd, buf, 2) == 2 && SYS3(8, (uint64_t)fd, 0, 0) == 0 &&
           SYS3(0, (uint64_t)fd, buf, 5) == 5 && memcmp(kf_uptr(buf), "HEllo", 5) == 0 &&
           tmp_size("/tmp/a") == 5005u,
           "O_RDWR writes into the middle of a file and leaves the rest");
    /* O_APPEND goes to the end as it is now. */
    fd = tmp_open("/tmp/a", 0x401 /* O_APPEND|O_WRONLY */, 0);
    memcpy(kf_uptr(buf), "!", 1);
    expect(SYS3(1, (uint64_t)fd, buf, 1) == 1 && tmp_size("/tmp/a") == 5006u, "O_APPEND writes at the end");
    expect(SYS3(1, (uint64_t)fd2, buf, 1) == -VIBEOS_EBADF, "a read-only descriptor cannot write");
    fd2 = tmp_open("/tmp/a", 0x401, 0);
    expect(SYS3(1, (uint64_t)fd2, buf, 1) == 1 && SYS3(1, (uint64_t)fd, buf, 1) == 1 &&
           tmp_size("/tmp/a") == 5008u,
           "two appenders interleave: each write finds the end where the other left it");
    expect(tmp_open("/tmp/a", 0x201 /* O_TRUNC|O_WRONLY */, 0) >= 3 && tmp_size("/tmp/a") == 0u,
           "O_TRUNC empties the file at open");
    expect(tmp_open("/tmp/a", 0x10000 /* O_DIRECTORY */, 0) == -VIBEOS_ENOTDIR,
           "O_DIRECTORY on a file is ENOTDIR");
    expect(tmp_open("/tmp", 1, 0) == -VIBEOS_EISDIR, "a directory opened to write is EISDIR");
    expect(tmp_open("/tmp/a", 3, 0) == -VIBEOS_EINVAL, "access mode 3 is EINVAL");
    /* umask and creat. */
    expect(SYS1(95, 077) == 022 && SYS1(95, 07777) == 077 && SYS1(95, 077) == 0777,
           "umask returns the one before, and keeps permission bits only");
    expect(SYS2(85, ustr("/tmp/m"), 0666) >= 3 && tmp_mode("/tmp/m") == 0100600u,
           "a created file has the mode asked for, less the umask");
    expect(tmp_mode("/tmp/a") == 0100644u, "and the earlier one kept its own");
}

/* pread, pwrite and the vector forms: at an offset, the position untouched. */
static void t_positional(void) {
    uint64_t buf = 0, iov = 0, fds = 0;
    uint64_t *v;
    long fd;

    fresh(94);
    buf = kf_ualloc(64);
    iov = kf_ualloc(32);
    fds = kf_ualloc(8);
    fd = tmp_open("/tmp/p", 0x42 /* O_CREAT|O_RDWR */, 0644);
    memcpy(kf_uptr(buf), "0123456789", 10);
    expect(SYS3(1, (uint64_t)fd, buf, 10) == 10, "ten bytes");
    memcpy(kf_uptr(buf), "XY", 2);
    expect(SYS4(18, (uint64_t)fd, buf, 2, 3) == 2 && SYS3(8, (uint64_t)fd, 0, 1) == 10,
           "pwrite64 writes at its offset and leaves the position alone");
    expect(SYS4(17, (uint64_t)fd, buf, 5, 2) == 5 && memcmp(kf_uptr(buf), "2XY56", 5) == 0 &&
           SYS3(8, (uint64_t)fd, 0, 1) == 10, "pread64 reads there, and does the same");
    expect(SYS4(18, (uint64_t)fd, buf, 1, 5000) == 1 && tmp_size("/tmp/p") == 5001u &&
           SYS4(17, (uint64_t)fd, buf, 4, 4000) == 4 && ((char *)kf_uptr(buf))[0] == 0,
           "a write past the end leaves a hole that reads as zeros");
    expect(SYS4(17, (uint64_t)fd, buf, 4, (uint64_t)-5) == -VIBEOS_EINVAL, "a negative offset is EINVAL");
    {
        long afd = tmp_open("/tmp/p", 0x401 /* O_WRONLY|O_APPEND */, 0);

        expect(afd >= 0 && SYS4(18, (uint64_t)afd, buf, 2, 0) == 2 && tmp_size("/tmp/p") == 5003u,
               "under O_APPEND pwrite64 writes at the end whatever its offset, as Linux does (pwrite04)");
        (void)SYS1(3, (uint64_t)afd);
    }
    expect(SYS1(22, fds) == 0 &&
           SYS4(17, (uint64_t)((int *)kf_uptr(fds))[0], buf, 4, 0) == -VIBEOS_ESPIPE,
           "pread64 on a pipe is ESPIPE");
    /* The vector forms. */
    v = (uint64_t *)kf_uptr(iov);
    v[0] = buf; v[1] = 3; v[2] = buf + 8; v[3] = 2;
    expect(sys(295, (uint64_t)fd, iov, 2, 0, 0, 0, 0) == 5 &&
           memcmp(kf_uptr(buf), "012", 3) == 0 && memcmp((char *)kf_uptr(buf) + 8, "XY", 2) == 0,
           "preadv fills each buffer in turn from the offset");
    memcpy(kf_uptr(buf), "abc", 3);
    memcpy((char *)kf_uptr(buf) + 8, "de", 2);
    expect(sys(296, (uint64_t)fd, iov, 2, 20, 0, 0, 0) == 5 &&
           SYS4(17, (uint64_t)fd, buf + 16, 5, 20) == 5 &&
           memcmp((char *)kf_uptr(buf) + 16, "abcde", 5) == 0, "pwritev gathers them at the offset");
    expect(sys(327, (uint64_t)fd, iov, 2, 0, 0, 2 /* RWF_DSYNC */, 0) == -VIBEOS_EOPNOTSUPP,
           "preadv2 refuses a flag rather than ignoring what it promises");
    expect(SYS3(8, (uint64_t)fd, 0, 0) == 0 && sys(327, (uint64_t)fd, iov, 2, (uint64_t)-1, 0, 0, 0) == 5 &&
           SYS3(8, (uint64_t)fd, 0, 1) == 5, "preadv2 at -1 uses and moves the position");
}

/* Sizes, durability, and the advice calls. */
static void t_truncate_and_sync(void) {
    uint64_t buf = 0, fds = 0;
    long fd, ro, dfd;
    int pr;

    fresh(95);
    buf = kf_ualloc(64);
    fds = kf_ualloc(8);
    fd = tmp_open("/tmp/t", 0x42, 0644);
    memcpy(kf_uptr(buf), "0123456789", 10);
    expect(SYS3(1, (uint64_t)fd, buf, 10) == 10, "ten bytes");
    expect(SYS2(77, (uint64_t)fd, 4) == 0 && tmp_size("/tmp/t") == 4u, "ftruncate shrinks");
    expect(SYS2(77, (uint64_t)fd, 8) == 0 && tmp_size("/tmp/t") == 8u &&
           SYS4(17, (uint64_t)fd, buf, 8, 0) == 8 && memcmp(kf_uptr(buf), "0123\0\0\0\0", 8) == 0,
           "and grows with zeros, not with what was cut off");
    expect(SYS2(77, (uint64_t)fd, (uint64_t)-1) == -VIBEOS_EINVAL, "a negative length is EINVAL");
    ro = tmp_open("/tmp/t", 0, 0);
    expect(SYS2(77, (uint64_t)ro, 1) == -VIBEOS_EINVAL && tmp_size("/tmp/t") == 8u,
           "ftruncate on a read-only descriptor is EINVAL");
    {
        /* A reader sees the file as it is now, not as it was when it opened. */
        uint64_t st = kf_ualloc(144);
        expect(SYS2(77, (uint64_t)fd, 300) == 0 && SYS2(5, (uint64_t)ro, st) == 0 &&
               *(uint64_t *)((uint8_t *)kf_uptr(st) + 48) == 300u &&
               SYS3(8, (uint64_t)ro, 0, 2) == 300,
               "fstat and SEEK_END through a read-only descriptor follow the file as it grows");
        expect(SYS2(77, (uint64_t)fd, 8) == 0, "back to eight");
    }
    expect(SYS2(76, ustr("/tmp/t"), 2) == 0 && tmp_size("/tmp/t") == 2u, "truncate by path");
    expect(SYS2(76, ustr("/tmp"), 0) == -VIBEOS_EISDIR, "truncate of a directory is EISDIR");
    expect(SYS2(76, ustr("/tmp/none"), 0) == -VIBEOS_ENOENT, "truncate of nothing is ENOENT");
    /* durability */
    dfd = tmp_open("/tmp", 0, 0);
    expect(SYS1(74, (uint64_t)fd) == 0 && SYS1(75, (uint64_t)fd) == 0 && SYS1(74, (uint64_t)dfd) == 0,
           "fsync and fdatasync succeed on a file and on a directory");
    expect(SYS1(22, fds) == 0, "a pipe");
    pr = ((int *)kf_uptr(fds))[0];
    expect(SYS1(74, (uint64_t)pr) == -VIBEOS_EINVAL, "fsync on a pipe is EINVAL");
    expect(SYS1(74, 99) == -VIBEOS_EBADF, "on nothing, EBADF");
    expect(SYS0(162) == 0 && SYS1(306, (uint64_t)fd) == 0, "sync and syncfs");
    /* fallocate and the advice calls */
    expect(SYS4(285, (uint64_t)fd, 0, 0, 100) == 0 && tmp_size("/tmp/t") == 100u,
           "fallocate makes the file at least that long");
    expect(SYS4(285, (uint64_t)fd, 0, 0, 10) == 0 && tmp_size("/tmp/t") == 100u, "and never shorter");
    expect(SYS4(285, (uint64_t)fd, 1 /* KEEP_SIZE */, 0, 500) == 0 && tmp_size("/tmp/t") == 100u,
           "KEEP_SIZE leaves the size");
    expect(SYS4(285, (uint64_t)fd, 0, 0, 0) == -VIBEOS_EINVAL, "a zero length is EINVAL");
    expect(SYS4(285, (uint64_t)pr, 0, 0, 10) == -VIBEOS_ESPIPE, "fallocate on a pipe is ESPIPE");
    expect(SYS4(221, (uint64_t)fd, 0, 0, 2) == 0 && SYS4(221, (uint64_t)fd, 0, 0, 9) == -VIBEOS_EINVAL &&
           SYS4(221, (uint64_t)pr, 0, 0, 0) == -VIBEOS_ESPIPE,
           "fadvise accepts Linux's advice, refuses a number it does not have, and a pipe");
    expect(SYS3(187, (uint64_t)fd, 0, 10) == 0 && SYS3(187, (uint64_t)pr, 0, 10) == -VIBEOS_EINVAL,
           "readahead on a file, and not on a pipe");
}

/* sendfile and copy_file_range: the kernel does the copying. */
static void t_kernel_copies(void) {
    uint64_t buf = 0, off = 0, off2 = 0, fds = 0;
    long in, out;
    int pr, pw;
    uint32_t i;

    fresh(96);
    buf = kf_ualloc(6000);
    off = kf_ualloc(8);
    off2 = kf_ualloc(8);
    fds = kf_ualloc(8);
    in = tmp_open("/tmp/src", 0x42, 0644);
    for (i = 0; i < 6000u; i++) {
        ((uint8_t *)kf_uptr(buf))[i] = (uint8_t)(i * 7u + 5u);
    }
    expect(SYS3(1, (uint64_t)in, buf, 6000) == 6000, "a source of 6000 bytes");
    out = tmp_open("/tmp/dst", 0x42, 0644);
    *(uint64_t *)kf_uptr(off) = 100;
    expect(SYS4(40, (uint64_t)out, (uint64_t)in, off, 5000) == 5000 &&
           *(uint64_t *)kf_uptr(off) == 5100u && tmp_size("/tmp/dst") == 5000u,
           "sendfile copies from the offset given and moves it, across a page boundary");
    memset(kf_uptr(buf), 0, 6000);
    expect(SYS4(17, (uint64_t)out, buf, 5000, 0) == 5000 &&
           ((uint8_t *)kf_uptr(buf))[0] == (uint8_t)(100u * 7u + 5u) &&
           ((uint8_t *)kf_uptr(buf))[4999] == (uint8_t)(5099u * 7u + 5u),
           "and the bytes are the source's");
    expect(SYS3(8, (uint64_t)in, 0, 1) == 6000, "the source's position did not move: an offset was given");
    expect(SYS3(8, (uint64_t)in, 5990, 0) == 5990 && SYS4(40, (uint64_t)out, (uint64_t)in, 0, 100) == 10 &&
           SYS3(8, (uint64_t)in, 0, 1) == 6000,
           "without one it reads from the position, moves it, and stops at the end");
    expect(SYS4(40, (uint64_t)out, (uint64_t)in, 0, 0) == 0, "a count of zero copies nothing");
    expect(SYS1(22, fds) == 0, "a pipe");
    pr = ((int *)kf_uptr(fds))[0];
    pw = ((int *)kf_uptr(fds))[1];
    *(uint64_t *)kf_uptr(off) = 0;
    expect(SYS4(40, (uint64_t)pw, (uint64_t)in, off, 64) == 64 &&
           SYS3(0, (uint64_t)pr, buf, 64) == 64 && ((uint8_t *)kf_uptr(buf))[63] == (uint8_t)(63u * 7u + 5u),
           "sendfile into a pipe: any file that can be written");
    expect(SYS4(40, (uint64_t)out, (uint64_t)pr, 0, 10) == -VIBEOS_EINVAL,
           "but not out of one: the source needs positions");
    expect(SYS4(40, 99, (uint64_t)in, 0, 10) == -VIBEOS_EBADF, "a closed descriptor is EBADF");
    /* copy_file_range */
    *(uint64_t *)kf_uptr(off) = 10;
    *(uint64_t *)kf_uptr(off2) = 7000;
    expect(sys(326, (uint64_t)in, off, (uint64_t)out, off2, 20, 0, 0) == 20 &&
           *(uint64_t *)kf_uptr(off) == 30u && *(uint64_t *)kf_uptr(off2) == 7020u &&
           tmp_size("/tmp/dst") == 7020u, "copy_file_range between two offsets, both moved");
    expect(SYS4(17, (uint64_t)out, buf, 20, 7000) == 20 &&
           ((uint8_t *)kf_uptr(buf))[0] == (uint8_t)(10u * 7u + 5u), "the bytes arrived");
    *(uint64_t *)kf_uptr(off) = 0;
    *(uint64_t *)kf_uptr(off2) = 10;
    expect(sys(326, (uint64_t)in, off, (uint64_t)in, off2, 20, 0, 0) == -VIBEOS_EINVAL,
           "a range copied onto itself is EINVAL");
    expect(sys(326, (uint64_t)in, off, (uint64_t)out, off2, 20, 1, 0) == -VIBEOS_EINVAL,
           "a flag is EINVAL");
    expect(sys(326, (uint64_t)in, off, (uint64_t)pw, 0, 20, 0, 0) == -VIBEOS_EINVAL,
           "copy_file_range is between regular files");
}

/* The root filesystem, beside /tmp: the fake's own, a second implementation of
 * the same operations, as FAT is beside tmpfs. */
static void t_root_filesystem_writes(void) {
    uint64_t buf = 0;
    long fd;

    fresh(97);
    buf = kf_ualloc(64);
    fd = SYS3(2, ustr("/w"), 0x41, 0644);
    memcpy(kf_uptr(buf), "whole", 5);
    expect(fd >= 3 && SYS3(1, (uint64_t)fd, buf, 5) == 5 && SYS1(3, (uint64_t)fd) == 0,
           "a file written on the root filesystem");
    fd = SYS2(2, ustr("/w"), 0);
    expect(fd >= 3 && SYS3(0, (uint64_t)fd, buf + 16, 16) == 5 &&
           memcmp((char *)kf_uptr(buf) + 16, "whole", 5) == 0, "is there");
    expect(SYS4(17, (uint64_t)fd, buf, 3, 1) == 3 && memcmp(kf_uptr(buf), "hol", 3) == 0,
           "and pread64 works on it too");
    /* An existing one, written into: the open that finds the file, where a
     * new file takes the create path. O_WRONLY without O_TRUNC keeps what the
     * write does not cover - it used to replace the whole file. */
    fd = SYS2(2, ustr("/w"), 1);
    memcpy(kf_uptr(buf), "WH", 2);
    expect(fd >= 3 && SYS3(1, (uint64_t)fd, buf, 2) == 2 && SYS1(3, (uint64_t)fd) == 0,
           "an existing file on the root filesystem opens to be written");
    fd = SYS2(2, ustr("/w"), 0);
    expect(fd >= 3 && SYS3(0, (uint64_t)fd, buf + 32, 16) == 5 &&
           memcmp((char *)kf_uptr(buf) + 32, "WHole", 5) == 0,
           "and a write without O_TRUNC changes the bytes it covers and keeps the rest");
    {
        uint64_t iov = kf_ualloc(16);
        uint64_t *v = (uint64_t *)kf_uptr(iov);
        fd = SYS2(2, ustr("/w"), 2);
        memcpy(kf_uptr(buf), "XY", 2);
        v[0] = buf; v[1] = 2;
        expect(SYS4(18, (uint64_t)fd, buf, 2, 3) == 2 && sys(296, (uint64_t)fd, iov, 1, 0, 0, 0, 0) == 2 &&
               SYS4(17, (uint64_t)fd, buf + 48, 5, 0) == 5 &&
               memcmp((char *)kf_uptr(buf) + 48, "XYoXY", 5) == 0,
               "pwrite64 and pwritev write at their offsets there too");
        expect(SYS2(77, (uint64_t)fd, 100) == 0 && SYS2(76, ustr("/w"), 3) == 0 &&
               SYS3(8, (uint64_t)fd, 0, 2) == 3, "ftruncate grows it and truncate cuts it");
    }
}

/* A forked child starts where its parent is. */
static void t_fork_inherits_cwd(void) {
    uint64_t buf;
    long pid;
    int parent, child = -1;
    uint32_t i;

    parent = fresh(93);
    kf_fs_add("/w", 0, 0, 1);
    buf = kf_ualloc(16);
    (void)SYS1(80, ustr("/w"));
    pid = SYS0(57);
    for (i = 0; i < KF_SLOTS; i++) {
        if ((int)i != parent && ks_id((int)i)->pid == (uint32_t)pid) {
            child = (int)i;
        }
    }
    if (child < 0) {
        expect(0, "fork made a child");
        return;
    }
    kf_set_current(child);
    expect(SYS2(79, buf, 16) == 3 && strcmp((const char *)kf_uptr(buf), "/w") == 0,
           "the child's working directory is the parent's");
    (void)SYS1(80, ustr("/"));
    kf_set_current(parent);
    expect(SYS2(79, buf, 16) == 3, "and a chdir in the child does not move the parent");
}

/* ---- L1 step 5: names and metadata -------------------------------------------------- */

#define SYS5(nr, a, b, c, d, e) sys((nr), (a), (b), (c), (d), (e), 0, 0)
#define CWD ((uint64_t)(uint32_t)-100)
#define NOID ((uint64_t)(uint32_t)-1)

static long tmp_stat(const char *path, int follow, linux_stat_t *out) {
    uint64_t st = kf_ualloc(sizeof(*out));
    long r = SYS2(follow ? 4 : 6, ustr(path), st);
    memcpy(out, kf_uptr(st), sizeof(*out));
    return r;
}

static void tmp_put(const char *path, const char *text) {
    long fd = tmp_open(path, 0x241 /* O_CREAT|O_WRONLY|O_TRUNC */, 0644);
    (void)SYS3(1, (uint64_t)fd, ustr(text), strlen(text));
    (void)SYS1(3, (uint64_t)fd);
}

static int tmp_is(const char *path, const char *text) {
    uint64_t buf = kf_ualloc(64);
    long fd = tmp_open(path, 0, 0);
    long n = fd < 0 ? -1 : SYS3(0, (uint64_t)fd, buf, 64);
    if (fd >= 0) {
        (void)SYS1(3, (uint64_t)fd);
    }
    return n == (long)strlen(text) && memcmp(kf_uptr(buf), text, strlen(text)) == 0;
}

static int tmp_gone(const char *path) {
    linux_stat_t st;
    return tmp_stat(path, 0, &st) == -VIBEOS_ENOENT;
}

/* A filesystem that writes nothing, mounted at /ro: one empty directory. What
 * every call here answers from it is EROFS, and none of them was asked which
 * filesystem it was talking to. */
static int ro_lookup(void *fs, const char *path, vibeos_fs_node_t *out) {
    (void)fs;
    if (path[0] != 0 && !(path[0] == '/' && path[1] == 0)) {
        return -1;
    }
    out->id = 1;
    out->is_dir = 1;
    return 0;
}

static long ro_read_at(void *fs, const vibeos_fs_node_t *node, uint64_t offset, void *buf,
                       uint32_t len) {
    (void)fs; (void)node; (void)offset; (void)buf; (void)len;
    return 0;
}

static const vibeos_fs_ops_t g_ro_ops = { .lookup = ro_lookup, .read_at = ro_read_at };
static vibeos_fsmount_t g_ro;

static void mount_ro(void) {
    (void)vibeos_fs_mount(&g_ro, &g_ro_ops, 0, "ro");
    (void)vibeos_fs_attach("/ro", &g_ro);
}

static void t_rename(void) {
    long dfd;

    fresh(100);
    tmp_put("/tmp/a", "one");
    expect(SYS2(82, ustr("/tmp/a"), ustr("/tmp/b")) == 0 && tmp_gone("/tmp/a") && tmp_is("/tmp/b", "one"),
           "rename moves a name");
    tmp_put("/tmp/c", "two");
    expect(SYS2(82, ustr("/tmp/b"), ustr("/tmp/c")) == 0 && tmp_gone("/tmp/b") && tmp_is("/tmp/c", "one"),
           "rename over a file replaces it");
    tmp_put("/tmp/b", "x");
    expect(SYS5(316, CWD, ustr("/tmp/b"), CWD, ustr("/tmp/c"), 1) == -VIBEOS_EEXIST &&
           tmp_is("/tmp/b", "x") && tmp_is("/tmp/c", "one"),
           "RENAME_NOREPLACE refuses an existing name and changes nothing");
    expect(SYS5(316, CWD, ustr("/tmp/b"), CWD, ustr("/tmp/z"), 1) == 0 && tmp_is("/tmp/z", "x"),
           "and moves onto a free one");
    expect(SYS5(316, CWD, ustr("/tmp/z"), CWD, ustr("/tmp/y"), 8) == -VIBEOS_EINVAL,
           "a rename flag Linux does not have is EINVAL");
    expect(SYS2(82, ustr("/tmp/c"), ustr("/tmp/c")) == 0 && tmp_is("/tmp/c", "one"),
           "a name renamed onto itself is left alone");
    expect(SYS2(82, ustr("/tmp/nope"), ustr("/tmp/y")) == -VIBEOS_ENOENT, "renaming nothing is ENOENT");
    expect(SYS2(83, ustr("/tmp/d"), 0755) == 0, "a directory to rename against");
    expect(SYS2(82, ustr("/tmp/c"), ustr("/tmp/d")) == -VIBEOS_EISDIR, "a file over a directory is EISDIR");
    expect(SYS2(82, ustr("/tmp/d"), ustr("/tmp/c")) == -VIBEOS_ENOTDIR, "a directory over a file is ENOTDIR");
    expect(SYS2(82, ustr("/tmp/d"), ustr("/tmp/d/sub")) == -VIBEOS_EINVAL,
           "a directory into itself is EINVAL");
    expect(SYS2(82, ustr("/tmp/c"), ustr("/moved")) == -VIBEOS_EXDEV && tmp_is("/tmp/c", "one"),
           "a rename across two mounts is EXDEV");
    expect(SYS2(82, ustr("/tmp"), ustr("/tmp/x")) == -VIBEOS_EBUSY, "a mount's root is EBUSY");
    tmp_put("/tmp/d/in", "in");
    dfd = tmp_open("/tmp/d", 0x10000 /* O_DIRECTORY */, 0);
    expect(dfd >= 3 && SYS4(264, (uint64_t)dfd, ustr("in"), CWD, ustr("/tmp/out")) == 0 &&
           tmp_is("/tmp/out", "in") && tmp_gone("/tmp/d/in"), "renameat resolves against its directory");
    tmp_put("/tmp/d/kept", "kept");
    expect(SYS2(82, ustr("/tmp/d"), ustr("/tmp/e")) == 0 && tmp_is("/tmp/e/kept", "kept") &&
           tmp_gone("/tmp/d"), "a directory moves with what is in it");
    /* A filesystem that writes and cannot rename: the fake's root. */
    kf_fs_add("/f", "x", 1, 0);
    expect(SYS2(82, ustr("/f"), ustr("/g")) == -VIBEOS_EPERM,
           "a filesystem that cannot rename says EPERM, not ENOSYS");
    /* And there too an existing name is what NOREPLACE finds first: tmpfs
     * honours the flag itself, so it could not say whether the handler does. */
    kf_fs_add("/g2", "y", 1, 0);
    expect(SYS5(316, CWD, ustr("/f"), CWD, ustr("/g2"), 1) == -VIBEOS_EEXIST,
           "RENAME_NOREPLACE finds the existing name before the filesystem is asked");
    mount_ro();
    expect(SYS2(82, ustr("/ro"), ustr("/ro/x")) == -VIBEOS_EBUSY, "and a read-only mount's root is EBUSY too");
    expect(kf_lock_imbalance() == 0, "rename released every lock it took");
}

static void t_rmdir_and_links(void) {
    linux_stat_t a, b;
    uint64_t buf;
    long dfd;

    fresh(101);
    buf = kf_ualloc(64);
    expect(SYS2(83, ustr("/tmp/d"), 0700) == 0 && tmp_stat("/tmp/d", 1, &a) == 0 &&
           a.st_mode == (VIBEOS_S_IFDIR | 0700u), "mkdir keeps the mode it was given");
    expect(SYS2(83, ustr("/tmp/open"), 0777) == 0 && tmp_stat("/tmp/open", 1, &a) == 0 &&
           a.st_mode == (VIBEOS_S_IFDIR | 0755u), "less the umask");
    tmp_put("/tmp/d/f", "f");
    expect(SYS1(84, ustr("/tmp/d")) == -VIBEOS_ENOTEMPTY, "rmdir of a directory with something in it is ENOTEMPTY");
    expect(SYS1(84, ustr("/tmp/d/f")) == -VIBEOS_ENOTDIR, "rmdir of a file is ENOTDIR");
    expect(SYS1(87, ustr("/tmp/d/f")) == 0, "empty it");
    expect(SYS1(84, ustr("/tmp/d/.")) == -VIBEOS_EINVAL, "rmdir of '.' is EINVAL");
    expect(SYS1(84, ustr("/tmp/d/..")) == -VIBEOS_ENOTEMPTY, "rmdir of '..' is ENOTEMPTY");
    expect(SYS1(84, ustr("/tmp")) == -VIBEOS_EBUSY && SYS1(84, ustr("/")) == -VIBEOS_EBUSY,
           "rmdir of a mount's root is EBUSY");
    expect(SYS1(84, ustr("/tmp/nope")) == -VIBEOS_ENOENT, "rmdir of nothing is ENOENT");
    expect(SYS1(84, ustr("/tmp/d/")) == 0 && tmp_gone("/tmp/d"), "rmdir removes an empty directory");
    expect(SYS3(263, CWD, ustr("/tmp/open"), 0x200) == 0 && tmp_gone("/tmp/open"),
           "and unlinkat(AT_REMOVEDIR) is rmdir");
    expect(SYS3(263, CWD, ustr("/tmp/open"), 0x400) == -VIBEOS_EINVAL, "an unlinkat flag Linux lacks is EINVAL");

    /* A second name for one file. */
    tmp_put("/tmp/a", "shared");
    expect(SYS2(86, ustr("/tmp/a"), ustr("/tmp/l")) == 0 && tmp_stat("/tmp/a", 1, &a) == 0 &&
           tmp_stat("/tmp/l", 1, &b) == 0 && a.st_nlink == 2u && b.st_nlink == 2u &&
           a.st_ino == b.st_ino && a.st_dev == b.st_dev, "link makes a second name: one inode, two links");
    expect(SYS2(86, ustr("/tmp/a"), ustr("/tmp/l")) == -VIBEOS_EEXIST, "link onto an existing name is EEXIST");
    expect(SYS1(87, ustr("/tmp/a")) == 0 && tmp_is("/tmp/l", "shared") &&
           tmp_stat("/tmp/l", 1, &b) == 0 && b.st_nlink == 1u, "the file outlives its first name");
    expect(SYS2(83, ustr("/tmp/dir"), 0755) == 0 &&
           SYS2(86, ustr("/tmp/dir"), ustr("/tmp/dir2")) == -VIBEOS_EPERM, "a hard link to a directory is EPERM");
    expect(SYS2(86, ustr("/tmp/l"), ustr("/elsewhere")) == -VIBEOS_EXDEV, "a link across two mounts is EXDEV");
    kf_fs_add("/f", "x", 1, 0);
    expect(SYS2(86, ustr("/f"), ustr("/g")) == -VIBEOS_EPERM, "a filesystem without links says EPERM");

    /* A name whose contents are a path. */
    expect(SYS2(88, ustr("a-target"), ustr("/tmp/s")) == 0 && tmp_stat("/tmp/s", 0, &a) == 0 &&
           (a.st_mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK && a.st_size == 8,
           "symlink makes a link, as long as its target's name");
    expect(tmp_stat("/tmp/s", 1, &a) == -VIBEOS_ENOENT, "whose target need not exist");
    memset(kf_uptr(buf), '#', 64);
    expect(SYS3(89, ustr("/tmp/s"), buf, 64) == 8 && memcmp(kf_uptr(buf), "a-target#", 9) == 0,
           "readlink gives its contents, not terminated");
    expect(SYS3(89, ustr("/tmp/s"), buf, 3) == 3 && memcmp(kf_uptr(buf), "a-ta", 4) == 0,
           "and cuts them to the buffer without complaint");
    expect(SYS3(89, ustr("/tmp/l"), buf, 64) == -VIBEOS_EINVAL, "readlink of what is not a link is EINVAL");
    expect(SYS2(88, ustr("l"), ustr("/tmp/s2")) == 0 && tmp_is("/tmp/s2", "shared"),
           "a relative target is resolved from the link's directory");
    expect(SYS2(88, ustr("x"), ustr("/tmp/s2")) == -VIBEOS_EEXIST, "symlink onto an existing name is EEXIST");
    /* Asked of the fake's root, which has no symbolic links: tmpfs refuses an
     * empty target itself, and so could not tell whether the handler does. */
    expect(SYS2(88, ustr(""), ustr("/s3")) == -VIBEOS_ENOENT,
           "an empty target is ENOENT, whatever the filesystem would have said");
    dfd = tmp_open("/tmp/dir", 0x10000, 0);
    expect(SYS3(266, ustr("../l"), (uint64_t)dfd, ustr("up")) == 0 && tmp_is("/tmp/dir/up", "shared"),
           "symlinkat resolves the new name against its directory");
    kf_fs_add("/h", "x", 1, 0);
    expect(SYS2(88, ustr("h"), ustr("/hs")) == -VIBEOS_EPERM, "a filesystem without symbolic links says EPERM");
    /* linkat links the link itself unless told to follow it. */
    expect(SYS5(265, CWD, ustr("/tmp/s2"), CWD, ustr("/tmp/h1"), 0) == 0 && tmp_stat("/tmp/h1", 0, &a) == 0 &&
           (a.st_mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK, "linkat links a symbolic link itself");
    expect(SYS5(265, CWD, ustr("/tmp/s2"), CWD, ustr("/tmp/h2"), 0x400) == 0 &&
           tmp_stat("/tmp/h2", 0, &a) == 0 && (a.st_mode & VIBEOS_S_IFMT) == VIBEOS_S_IFREG,
           "and what it points at with AT_SYMLINK_FOLLOW");
    expect(SYS5(265, CWD, ustr("/tmp/l"), CWD, ustr("/tmp/h3"), 0x100) == -VIBEOS_EINVAL,
           "a linkat flag that is not linkat's is EINVAL");
    mount_ro();
    expect(SYS2(83, ustr("/ro/d"), 0755) == -VIBEOS_EROFS && SYS2(88, ustr("t"), ustr("/ro/s")) == -VIBEOS_EROFS &&
           SYS3(133, ustr("/ro/n"), 0100644, 0) == -VIBEOS_EROFS,
           "making anything on a filesystem that writes nothing is EROFS");
    expect(kf_lock_imbalance() == 0, "the name calls released every lock they took");
}

static void t_metadata(void) {
    linux_stat_t st, st2;
    linux_statx_t *sx;
    linux_statfs_t *sf;
    linux_timespec_t *ts;
    linux_timeval_t *tv;
    linux_utimbuf_t *ut;
    uint64_t ubuf, ufs, uts, fds;
    long fd, dfd;

    fresh(102);
    ubuf = kf_ualloc(sizeof(linux_statx_t));
    ufs = kf_ualloc(sizeof(linux_statfs_t));
    uts = kf_ualloc(32);
    fds = kf_ualloc(8);
    sx = (linux_statx_t *)kf_uptr(ubuf);
    sf = (linux_statfs_t *)kf_uptr(ufs);
    ts = (linux_timespec_t *)kf_uptr(uts);
    tv = (linux_timeval_t *)kf_uptr(uts);
    ut = (linux_utimbuf_t *)kf_uptr(uts);
    tmp_put("/tmp/m", "12345");
    kf_fs_add("/f", "x", 1, 0);
    (void)SYS2(88, ustr("m"), ustr("/tmp/s"));
    (void)SYS2(83, ustr("/tmp/d"), 0755);

    /* stat, lstat, statx. */
    expect(tmp_stat("/tmp/m", 1, &st) == 0 && st.st_mode == (VIBEOS_S_IFREG | 0644u) && st.st_size == 5,
           "stat reports a file");
    expect(tmp_stat("/tmp/s", 1, &st2) == 0 && st2.st_ino == st.st_ino &&
           tmp_stat("/tmp/s", 0, &st2) == 0 && (st2.st_mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK,
           "stat follows a link and lstat does not");
    expect(tmp_stat("/f", 1, &st2) == 0 && st.st_dev != 0u && st2.st_dev != 0u && st.st_dev != st2.st_dev,
           "two filesystems are two devices");
    expect(SYS5(332, CWD, ustr("/tmp/m"), 0, 0x7ff, ubuf) == 0 && sx->stx_mask == 0x7ffu &&
           sx->stx_mode == (VIBEOS_S_IFREG | 0644u) && sx->stx_size == 5u && sx->stx_ino == st.st_ino &&
           sx->stx_dev_minor == st.st_dev && sx->stx_nlink == 1u, "statx reports what stat does");
    expect(SYS5(332, CWD, ustr("/tmp/s"), 0x100, 0x7ff, ubuf) == 0 &&
           (sx->stx_mode & VIBEOS_S_IFMT) == VIBEOS_S_IFLNK, "and the link itself with AT_SYMLINK_NOFOLLOW");
    expect(SYS5(332, CWD, ustr("/tmp/m"), 0x6000, 0x7ff, ubuf) == -VIBEOS_EINVAL &&
           SYS5(332, CWD, ustr("/tmp/m"), 0, 0x80000000u, ubuf) == -VIBEOS_EINVAL &&
           SYS5(332, CWD, ustr("/tmp/m"), 0x10, 0x7ff, ubuf) == -VIBEOS_EINVAL,
           "both sync types at once, the reserved mask bit and an unknown flag are EINVAL");
    fd = tmp_open("/tmp/m", 2, 0);
    expect(SYS5(332, (uint64_t)fd, ustr(""), 0x1000, 0x7ff, ubuf) == 0 && sx->stx_size == 5u,
           "statx of a descriptor with AT_EMPTY_PATH");
    expect(SYS5(332, (uint64_t)fd, ustr(""), 0, 0x7ff, ubuf) == -VIBEOS_ENOENT,
           "and ENOENT for an empty path without it");
    (void)SYS2(293, fds, 0);
    expect(SYS5(332, (uint64_t)((int32_t *)kf_uptr(fds))[0], ustr(""), 0x1000, 0x7ff, ubuf) == 0 &&
           (sx->stx_mode & VIBEOS_S_IFMT) == VIBEOS_S_IFIFO && sx->stx_dev_minor == 0u,
           "a pipe is a FIFO to statx too, on no device");

    /* access. */
    expect(SYS2(21, ustr("/tmp/m"), 0) == 0 && SYS2(21, ustr("/tmp/m"), 6) == 0, "access: it exists, root reads and writes");
    expect(SYS2(21, ustr("/tmp/nope"), 0) == -VIBEOS_ENOENT, "access of nothing is ENOENT");
    expect(SYS2(21, ustr("/tmp/m"), 8) == -VIBEOS_EINVAL, "a mode bit access does not have is EINVAL");
    expect(SYS2(21, ustr("/tmp/m"), 1) == -VIBEOS_EACCES, "a file nobody may run is EACCES even for root");
    expect(SYS2(21, ustr("/tmp/d"), 1) == 0, "a directory may be searched");
    expect(SYS4(439, CWD, ustr("/tmp/m"), 4, 0x200) == 0 && SYS4(439, CWD, ustr("/tmp/m"), 4, 0x8000) == -VIBEOS_EINVAL &&
           SYS3(269, CWD, ustr("/tmp/m"), 4) == 0, "faccessat2 takes AT_EACCESS and refuses a flag it lacks");

    /* chmod. */
    expect(SYS2(90, ustr("/tmp/m"), 0755) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_mode == (VIBEOS_S_IFREG | 0755u) && SYS2(21, ustr("/tmp/m"), 1) == 0,
           "chmod changes the permission bits, and the file may now be run");
    expect(SYS2(90, ustr("/tmp/s"), 0700) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           (st.st_mode & 0777u) == 0700u, "chmod through a link changes what it points at");
    expect(SYS2(91, (uint64_t)fd, 0600) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 && (st.st_mode & 0777u) == 0600u,
           "fchmod changes the file a descriptor names");
    expect(SYS2(91, (uint64_t)((int32_t *)kf_uptr(fds))[0], 0600) == -VIBEOS_EINVAL && SYS2(91, 99, 0600) == -VIBEOS_EBADF,
           "fchmod of a pipe is EINVAL and of nothing EBADF");
    expect(SYS4(452, CWD, ustr("/tmp/s"), 0777, 0x100) == -VIBEOS_EOPNOTSUPP,
           "a symbolic link has no mode of its own to change");
    expect(SYS4(452, CWD, ustr("/tmp/m"), 0640, 0x100) == 0 && SYS3(268, CWD, ustr("/tmp/m"), 0644) == 0 &&
           tmp_stat("/tmp/m", 1, &st) == 0 && (st.st_mode & 0777u) == 0644u, "fchmodat and fchmodat2 on a file");
    expect(SYS2(90, ustr("/tmp/nope"), 0644) == -VIBEOS_ENOENT, "chmod of nothing is ENOENT");

    /* chown. */
    expect(SYS2(90, ustr("/tmp/m"), 06755) == 0 && SYS3(92, ustr("/tmp/m"), 1000, 1001) == 0 &&
           tmp_stat("/tmp/m", 1, &st) == 0 && st.st_uid == 1000u && st.st_gid == 1001u,
           "chown changes owner and group");
    expect((st.st_mode & 07777u) == 0755u, "and takes set-user-id and set-group-id off the file");
    expect(SYS3(92, ustr("/tmp/m"), NOID, 5) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_uid == 1000u && st.st_gid == 5u, "an id of -1 is left alone");
    expect(SYS3(94, ustr("/tmp/s"), 7, 7) == 0 && tmp_stat("/tmp/s", 0, &st2) == 0 && st2.st_uid == 7u &&
           tmp_stat("/tmp/m", 1, &st) == 0 && st.st_uid == 1000u, "lchown changes the link and not its target");
    expect(SYS3(93, (uint64_t)fd, 8, 9) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 && st.st_uid == 8u && st.st_gid == 9u,
           "fchown changes the file a descriptor names");
    expect(SYS5(260, (uint64_t)fd, ustr(""), 10, 11, 0x1000) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_uid == 10u, "fchownat with AT_EMPTY_PATH is fchown");
    expect(SYS5(260, CWD, ustr("/tmp/m"), 1, 1, 0x200) == -VIBEOS_EINVAL, "a fchownat flag it lacks is EINVAL");
    expect(SYS3(92, ustr("/f"), 1, 1) == -VIBEOS_EPERM, "a filesystem with no owners says EPERM");

    /* Times. */
    ts[0].tv_sec = 100; ts[0].tv_nsec = 5;
    ts[1].tv_sec = 200; ts[1].tv_nsec = 7;
    expect(SYS4(280, CWD, ustr("/tmp/m"), uts, 0) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_atime == 100u && st.st_atime_nsec == 5u && st.st_mtime == 200u && st.st_mtime_nsec == 7u,
           "utimensat sets both times to the nanosecond");
    ts[0].tv_nsec = LINUX_UTIME_OMIT;
    ts[1].tv_sec = 300; ts[1].tv_nsec = 0;
    expect(SYS4(280, CWD, ustr("/tmp/m"), uts, 0) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_atime == 100u && st.st_atime_nsec == 5u && st.st_mtime == 300u, "UTIME_OMIT leaves a time alone");
    ts[0].tv_nsec = LINUX_UTIME_OMIT; ts[1].tv_nsec = LINUX_UTIME_OMIT;
    expect(SYS4(280, CWD, ustr("/tmp/m"), uts, 0) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 && st.st_mtime == 300u,
           "both omitted changes nothing");
    ts[0].tv_sec = 1; ts[0].tv_nsec = 1000000000;
    expect(SYS4(280, CWD, ustr("/tmp/m"), uts, 0) == -VIBEOS_EINVAL, "a nanosecond field out of range is EINVAL");
    ts[0].tv_sec = 11; ts[0].tv_nsec = 0; ts[1].tv_sec = 12; ts[1].tv_nsec = 0;
    expect(SYS4(280, (uint64_t)fd, 0, uts, 0) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_atime == 11u && st.st_mtime == 12u, "no path at all is the descriptor: futimens");
    expect(SYS4(280, CWD, ustr("/tmp/m"), 0, 0) == 0 && SYS4(280, CWD, ustr("/tmp/m"), uts, 0x200) == -VIBEOS_EINVAL,
           "no times at all is now, and a flag it lacks is EINVAL");
    ut->actime = 400; ut->modtime = 500;
    expect(SYS2(132, ustr("/tmp/m"), uts) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_atime == 400u && st.st_mtime == 500u && st.st_mtime_nsec == 0u, "utime sets them to the second");
    tv[0].tv_sec = 1; tv[0].tv_usec = 999999; tv[1].tv_sec = 2; tv[1].tv_usec = 0;
    expect(SYS2(235, ustr("/tmp/m"), uts) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_atime == 1u && st.st_atime_nsec == 999999000u && st.st_mtime == 2u,
           "utimes sets them to the microsecond");
    tv[0].tv_usec = 1000000;
    expect(SYS2(235, ustr("/tmp/m"), uts) == -VIBEOS_EINVAL, "a microsecond field out of range is EINVAL");
    tv[0].tv_sec = 21; tv[0].tv_usec = 0; tv[1].tv_sec = 22;
    dfd = tmp_open("/tmp", 0x10000, 0);
    expect(SYS3(261, (uint64_t)dfd, ustr("m"), uts) == 0 && tmp_stat("/tmp/m", 1, &st) == 0 &&
           st.st_atime == 21u && st.st_mtime == 22u, "futimesat resolves against its directory");

    /* statfs. */
    expect(SYS2(137, ustr("/tmp/m"), ufs) == 0 && sf->f_type == 0x01021994 && sf->f_bsize == 4096 &&
           sf->f_blocks > 0 && sf->f_bfree > 0 && sf->f_bfree <= sf->f_blocks && sf->f_namelen == 255 &&
           sf->f_flags == 0 && sf->f_fsid[0] == (int32_t)st.st_dev, "statfs describes the filesystem a file is on");
    memset(sf, 0xFF, sizeof(*sf));
    expect(SYS2(138, (uint64_t)fd, ufs) == 0 && sf->f_type == 0x01021994, "fstatfs the one a descriptor is on");
    expect(SYS2(138, (uint64_t)((int32_t *)kf_uptr(fds))[0], ufs) == -VIBEOS_ENOSYS && SYS2(138, 99, ufs) == -VIBEOS_EBADF,
           "a pipe is on none");
    expect(SYS2(137, ustr("/tmp/nope"), ufs) == -VIBEOS_ENOENT, "statfs of nothing is ENOENT");
    mount_ro();
    expect(SYS2(137, ustr("/ro"), ufs) == 0 && (sf->f_flags & 1) != 0, "a filesystem that writes nothing is ST_RDONLY");
    expect(SYS2(21, ustr("/ro"), 2) == -VIBEOS_EROFS && SYS2(21, ustr("/ro"), 4) == 0,
           "and writing there is EROFS to access, reading is not");
    expect(SYS2(90, ustr("/ro"), 0700) == -VIBEOS_EROFS && SYS3(92, ustr("/ro"), 1, 1) == -VIBEOS_EROFS &&
           SYS4(280, CWD, ustr("/ro"), 0, 0) == -VIBEOS_EROFS, "chmod, chown and utimensat there are EROFS");
    expect(kf_lock_imbalance() == 0, "the metadata calls released every lock they took");
}

static void t_mknod_and_openat2(void) {
    linux_stat_t st;
    linux_open_how_t *how;
    uint64_t uhow;
    long fd, dfd;

    fresh(103);
    uhow = kf_ualloc(64);
    how = (linux_open_how_t *)kf_uptr(uhow);
    expect(SYS3(133, ustr("/tmp/n"), 0100640, 0) == 0 && tmp_stat("/tmp/n", 1, &st) == 0 &&
           st.st_mode == (VIBEOS_S_IFREG | 0640u) && st.st_size == 0, "mknod makes a regular file");
    expect(SYS3(133, ustr("/tmp/n"), 0100640, 0) == -VIBEOS_EEXIST, "and refuses an existing name");
    expect(SYS3(133, ustr("/tmp/n0"), 0666, 0) == 0 && tmp_stat("/tmp/n0", 1, &st) == 0 &&
           st.st_mode == (VIBEOS_S_IFREG | 0644u), "no type at all is a regular file, less the umask");
    expect(SYS3(133, ustr("/tmp/nd"), 0040755, 0) == -VIBEOS_EPERM, "mknod does not make directories");
    expect(SYS3(133, ustr("/tmp/nx"), 0150644, 0) == -VIBEOS_EINVAL, "a type that is not one is EINVAL");
    (void)SYS2(83, ustr("/tmp/d"), 0755);
    dfd = tmp_open("/tmp/d", 0x10000, 0);
    expect(SYS4(259, (uint64_t)dfd, ustr("x"), 0100600, 0) == 0 && tmp_stat("/tmp/d/x", 1, &st) == 0,
           "mknodat resolves against its directory");

    memset(how, 0, 64);
    how->flags = 2;   /* O_RDWR */
    fd = SYS4(437, CWD, ustr("/tmp/n"), uhow, 24);
    expect(fd >= 3 && SYS3(1, (uint64_t)fd, ustr("ok"), 2) == 2 && tmp_is("/tmp/n", "ok"), "openat2 opens");
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 16) == -VIBEOS_EINVAL, "a structure shorter than the first is EINVAL");
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 40) >= 3, "a longer one whose extra is zero is accepted");
    ((uint8_t *)how)[33] = 1;
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 40) == -VIBEOS_E2BIG, "and E2BIG when the extra says something");
    ((uint8_t *)how)[33] = 0;
    how->flags = 2 | 0x8000;   /* O_LARGEFILE, which every 64-bit open carries */
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 24) >= 3, "a flag Linux has is accepted");
    how->flags = 2 | 0x4;
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 24) == -VIBEOS_EINVAL, "a flag Linux lacks is EINVAL - open would ignore it");
    how->flags = 2; how->mode = 0644;
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 24) == -VIBEOS_EINVAL, "a mode without O_CREAT is EINVAL");
    how->flags = 0x42; how->mode = 0600;
    expect(SYS4(437, CWD, ustr("/tmp/made"), uhow, 24) >= 3 && tmp_stat("/tmp/made", 1, &st) == 0 &&
           (st.st_mode & 0777u) == 0600u, "with O_CREAT the mode is the file's");
    how->flags = 0; how->mode = 0; how->resolve = 2;   /* RESOLVE_NO_MAGICLINKS */
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 24) >= 3, "RESOLVE_NO_MAGICLINKS is honoured: there are none");
    how->resolve = 0x40;
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 24) == -VIBEOS_EINVAL, "a resolve flag Linux lacks is EINVAL");
    how->resolve = 0x18;
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 24) == -VIBEOS_EINVAL, "BENEATH with IN_ROOT is EINVAL");
    how->resolve = 0x20;
    expect(SYS4(437, CWD, ustr("/tmp/n"), uhow, 24) == -VIBEOS_EAGAIN, "RESOLVE_CACHED is EAGAIN: ask again without it");
    expect(SYS4(437, CWD, ustr("/tmp/n"), 0x1000, 24) == -VIBEOS_EFAULT, "a structure outside user memory is EFAULT");
    expect(kf_lock_imbalance() == 0, "mknod and openat2 released every lock they took");
}

/* ---- L1 step 6: locks, directories, extended attributes ----------------------------- */

static long lock_op(long fd, int cmd, int type, int64_t start, int64_t len, linux_flock_t *out) {
    uint64_t u = kf_ualloc(sizeof(linux_flock_t));
    linux_flock_t *fl = (linux_flock_t *)kf_uptr(u);
    long r;

    memset(fl, 0, sizeof(*fl));
    fl->l_type = (int16_t)type;
    fl->l_start = start;
    fl->l_len = len;
    r = SYS3(72, (uint64_t)fd, (uint64_t)cmd, u);
    if (out) {
        *out = *fl;
    }
    return r;
}

static int slot_of_pid(long pid) {
    uint32_t i;
    for (i = 0; i < KF_SLOTS; i++) {
        if (ks_id((int)i)->pid == (uint32_t)pid) {
            return (int)i;
        }
    }
    return -1;
}

static void t_record_locks(void) {
    linux_flock_t fl;
    kf_outcome_t how;
    int parent, child;
    long fd, fd2, cfd, pid;
    uint64_t u;

    parent = fresh(110);
    tmp_put("/tmp/db", "0123456789");
    fd = tmp_open("/tmp/db", 2, 0);
    expect(lock_op(fd, 6 /* F_SETLK */, 1 /* F_WRLCK */, 0, 4, 0) == 0, "F_SETLK takes a write lock");
    expect(lock_op(fd, 5 /* F_GETLK */, 1, 0, 10, &fl) == 0 && fl.l_type == 2 /* F_UNLCK */,
           "the holder's own lock is not in its way");
    pid = SYS0(57);
    expect(pid > 0, "fork");
    child = slot_of_pid(pid);
    kf_set_current(child);
    cfd = tmp_open("/tmp/db", 2, 0);
    expect(lock_op(cfd, 6, 0 /* F_RDLCK */, 2, 2, 0) == -VIBEOS_EAGAIN, "another process is refused the range");
    expect(lock_op(fd, 6, 0, 2, 2, 0) == -VIBEOS_EAGAIN,
           "through the inherited descriptor too: the lock is the process's, not the descriptor's");
    expect(lock_op(cfd, 5, 0, 0, 0, &fl) == 0 && fl.l_type == 1 && fl.l_whence == 0 && fl.l_start == 0 &&
           fl.l_len == 4 && fl.l_pid == 110, "F_GETLK names the lock in the way and who holds it");
    expect(lock_op(cfd, 6, 1, 4, 0, 0) == 0, "the rest of the file, to its end, is free");
    kf_set_current(parent);
    expect(lock_op(fd, 5, 0, 100, 1, &fl) == 0 && fl.l_type == 1 && fl.l_start == 4 && fl.l_len == 0 &&
           fl.l_pid == (int32_t)pid, "a lock to the end of the file is reported with length 0");
    /* Waiting: the child holds [4, end); the parent asks and waits. */
    u = kf_ualloc(sizeof(fl));
    memset(kf_uptr(u), 0, sizeof(fl));
    ((linux_flock_t *)kf_uptr(u))->l_type = 1;
    ((linux_flock_t *)kf_uptr(u))->l_start = 6;
    ((linux_flock_t *)kf_uptr(u))->l_len = 1;
    (void)sys(72, (uint64_t)fd, 7 /* F_SETLKW */, u, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "F_SETLKW waits for a lock it cannot have");
    kf_set_current(child);
    ((linux_flock_t *)kf_uptr(u))->l_start = 1;
    expect(SYS3(72, (uint64_t)cfd, 7, u) == -VIBEOS_EDEADLK,
           "and the holder waiting for the waiter is EDEADLK, not two waits for ever");
    /* Unlocking the middle of a range. */
    expect(lock_op(cfd, 6, 2 /* F_UNLCK */, 6, 2, 0) == 0, "the child gives back two bytes of its range");
    kf_set_current(parent);
    expect(lock_op(fd, 6, 1, 6, 2, 0) == 0 && lock_op(fd, 6, 1, 8, 1, 0) == -VIBEOS_EAGAIN &&
           lock_op(fd, 6, 1, 5, 1, 0) == -VIBEOS_EAGAIN, "exactly those two are free");
    /* POSIX's rule: closing any descriptor for the file drops the process's locks on it. */
    fd2 = tmp_open("/tmp/db", 0, 0);
    expect(SYS1(3, (uint64_t)fd2) == 0, "the parent closes another descriptor for the file");
    kf_set_current(child);
    expect(lock_op(cfd, 6, 1, 0, 4, 0) == 0, "and its locks on the file are gone, whichever descriptor took them");
    /* Access modes, ranges and bad requests - on a second file, where nobody
     * holds anything yet. */
    {
        long p2, r2, c2;
        uint64_t w = kf_ualloc(sizeof(fl));
        linux_flock_t *p = (linux_flock_t *)kf_uptr(w);

        kf_set_current(parent);
        tmp_put("/tmp/db2", "0123456789");
        p2 = tmp_open("/tmp/db2", 2, 0);
        r2 = tmp_open("/tmp/db2", 0, 0);
        expect(lock_op(r2, 6, 1, 20, 1, 0) == -VIBEOS_EBADF, "a write lock needs a descriptor open for writing");
        expect(lock_op(r2, 6, 0, 20, 1, 0) == 0, "a read lock does not");
        expect(lock_op(p2, 6, 1, -1, 1, 0) == -VIBEOS_EINVAL, "a range that starts before the file is EINVAL");
        expect(lock_op(p2, 6, 1, 30, -5, 0) == 0, "a negative length is the bytes before the start");
        kf_set_current(child);
        c2 = tmp_open("/tmp/db2", 2, 0);
        expect(lock_op(c2, 5, 1, 22, 20, &fl) == 0 && fl.l_type == 1 && fl.l_start == 25 && fl.l_len == 5,
               "exactly those");
        expect(lock_op(c2, 6, 3, 0, 1, 0) == -VIBEOS_EINVAL && lock_op(c2, 5, 2, 0, 1, 0) == -VIBEOS_EINVAL,
               "a lock type that is none, and F_GETLK asked about an unlock, are EINVAL");
        expect(SYS3(72, 99, 6, kf_ualloc(32)) == -VIBEOS_EBADF && SYS3(72, (uint64_t)c2, 6, 0x1000) == -VIBEOS_EFAULT,
               "no descriptor is EBADF, no structure EFAULT");
        /* A lock at SEEK_END is where the file ends now. */
        memset(p, 0, sizeof(*p));
        p->l_type = 1; p->l_whence = 2 /* SEEK_END */; p->l_start = 0; p->l_len = 5;
        expect(SYS3(72, (uint64_t)c2, 6, w) == 0, "a lock from the end of the file");
        kf_set_current(parent);
        expect(lock_op(p2, 5, 1, 0, 20, &fl) == 0 && fl.l_type == 1 && fl.l_start == 10 && fl.l_len == 5 &&
               fl.l_pid == (int32_t)pid, "is at the file's size");
    }
    /* Exit gives everything back: what the architecture's exit path calls
     * when the last thread of a process has left. */
    linux_locks_exit((uint32_t)pid);
    expect(lock_op(fd, 6, 1, 0, 0, 0) == 0, "a process that exits leaves no locks behind");
    expect(kf_lock_imbalance() == 0, "the lock calls released every kernel lock they took");
}

static void t_description_locks(void) {
    linux_flock_t fl;
    kf_outcome_t how;
    long a, b, c;

    fresh(111);
    tmp_put("/tmp/f", "x");
    a = tmp_open("/tmp/f", 2, 0);
    b = tmp_open("/tmp/f", 2, 0);
    /* flock: the description's, whole-file. */
    expect(SYS2(73, (uint64_t)a, 2 /* LOCK_EX */) == 0, "flock takes the file");
    expect(SYS2(73, (uint64_t)b, 2 | 4 /* LOCK_NB */) == -VIBEOS_EAGAIN &&
           SYS2(73, (uint64_t)b, 1 | 4) == -VIBEOS_EAGAIN,
           "another description of the same process is refused: the lock is the description's");
    (void)sys(73, (uint64_t)b, 1, 0, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "and waits without LOCK_NB");
    c = SYS1(32, (uint64_t)a);
    expect(SYS2(73, (uint64_t)c, 2 | 4) == 0, "a dup is the same description and already holds it");
    expect(SYS2(73, (uint64_t)a, 1) == 0 && SYS2(73, (uint64_t)b, 1 | 4) == 0,
           "converted to shared, another description shares it");
    expect(SYS2(73, (uint64_t)a, 2 | 4) == -VIBEOS_EAGAIN, "and it cannot go back to exclusive while shared");
    expect(SYS2(73, (uint64_t)b, 8 /* LOCK_UN */) == 0 && SYS2(73, (uint64_t)a, 2 | 4) == 0,
           "until the other lets go");
    expect(SYS1(3, (uint64_t)a) == 0 && SYS2(73, (uint64_t)b, 2 | 4) == -VIBEOS_EAGAIN,
           "closing one of two descriptors for a description keeps its lock");
    expect(SYS1(3, (uint64_t)c) == 0 && SYS2(73, (uint64_t)b, 2 | 4) == 0,
           "closing the last gives it back");
    expect(SYS2(73, (uint64_t)b, 3) == -VIBEOS_EINVAL && SYS2(73, (uint64_t)b, 0) == -VIBEOS_EINVAL &&
           SYS2(73, 99, 1) == -VIBEOS_EBADF, "two operations at once or none are EINVAL, no descriptor EBADF");
    /* flock and fcntl do not see each other. */
    expect(lock_op(a = tmp_open("/tmp/f", 2, 0), 6, 1, 0, 0, 0) == 0,
           "a record lock on a file somebody has flocked is granted");
    /* OFD locks: record locks a description owns. */
    c = tmp_open("/tmp/f", 2, 0);
    expect(lock_op(c, 37 /* F_OFD_SETLK */, 1, 0, 1, 0) == -VIBEOS_EAGAIN,
           "an OFD lock conflicts with the process's own record lock: one space, two owners");
    expect(lock_op(a, 6, 2, 0, 0, 0) == 0 && lock_op(c, 37, 1, 0, 1, 0) == 0, "and is granted once that is gone");
    expect(lock_op(a, 6, 1, 0, 1, 0) == -VIBEOS_EAGAIN && lock_op(a, 5, 1, 0, 1, &fl) == 0 && fl.l_pid == -1,
           "the process is refused in turn, by a lock with no process: pid -1");
    {
        uint64_t u = kf_ualloc(sizeof(fl));
        linux_flock_t *p = (linux_flock_t *)kf_uptr(u);
        memset(p, 0, sizeof(*p));
        p->l_type = 1; p->l_pid = 5;
        expect(SYS3(72, (uint64_t)c, 37, u) == -VIBEOS_EINVAL, "an OFD lock with l_pid set is EINVAL");
    }
    b = tmp_open("/tmp/f", 0, 0);
    expect(SYS1(3, (uint64_t)b) == 0 && lock_op(a, 6, 1, 0, 1, 0) == -VIBEOS_EAGAIN,
           "closing another descriptor for the file does not drop an OFD lock");
    expect(SYS1(3, (uint64_t)c) == 0 && lock_op(a, 6, 1, 0, 1, 0) == 0, "closing its description does");
    /* A pipe can be locked too. */
    {
        uint64_t fds = kf_ualloc(8), fds2 = kf_ualloc(8);
        (void)SYS2(293, fds, 0);
        (void)SYS2(293, fds2, 0);
        expect(SYS2(73, (uint64_t)((int32_t *)kf_uptr(fds))[0], 2 | 4) == 0, "flock on a pipe");
        expect(SYS2(73, (uint64_t)((int32_t *)kf_uptr(fds))[1], 2 | 4) == -VIBEOS_EAGAIN,
               "its other end is the same pipe, another description: refused");
        expect(SYS2(73, (uint64_t)((int32_t *)kf_uptr(fds2))[0], 2 | 4) == 0,
               "and another pipe is another file");
    }
    expect(kf_lock_imbalance() == 0, "the description locks released every kernel lock they took");
}

/* One getdents64 record at `off` in a buffer. */
static const linux_dirent64_t *dent(uint64_t buf, long off) {
    return (const linux_dirent64_t *)(const void *)((const uint8_t *)kf_uptr(buf) + off);
}

/* The entry called `name` among `n` bytes of records, or null. */
static const linux_dirent64_t *dent_named(uint64_t buf, long n, const char *name) {
    long off = 0;
    while (off < n) {
        const linux_dirent64_t *d = dent(buf, off);
        if (strcmp(d->d_name, name) == 0) {
            return d;
        }
        off += d->d_reclen;
    }
    return 0;
}

static void t_directories(void) {
    static const char longname[] = "a name far longer than fifteen bytes, with spaces.txt";
    char path[96];
    linux_stat_t st;
    const linux_dirent64_t *d;
    uint64_t buf;
    long fd, n, n2, off, count;

    fresh(112);
    buf = kf_ualloc(4096);
    (void)SYS2(83, ustr("/tmp/d"), 0755);
    (void)SYS2(83, ustr("/tmp/d/sub"), 0755);
    snprintf(path, sizeof(path), "/tmp/d/%s", longname);
    tmp_put(path, "x");
    tmp_put("/tmp/d/f", "y");
    (void)SYS2(88, ustr("f"), ustr("/tmp/d/link"));
    fd = tmp_open("/tmp/d", 0x10000, 0);
    n = SYS3(217, (uint64_t)fd, buf, 4096);
    expect(n > 0, "getdents64 lists a directory");
    d = dent_named(buf, n, longname);
    expect(d != 0 && d->d_type == 8, "a long name is listed whole - it was cut at fifteen bytes");
    expect(d != 0 && tmp_stat(path, 0, &st) == 0 && d->d_ino == st.st_ino && d->d_ino != 0u,
           "with the inode number stat reports");
    d = dent_named(buf, n, "link");
    expect(d != 0 && d->d_type == 10, "a symbolic link is listed as one, not as a file");
    d = dent_named(buf, n, "sub");
    expect(d != 0 && d->d_type == 4, "a directory as a directory");
    d = dent_named(buf, n, ".");
    expect(d != 0 && d->d_type == 4 && tmp_stat("/tmp/d", 1, &st) == 0 && d->d_ino == st.st_ino,
           "'.' is there, and is the directory");
    d = dent_named(buf, n, "..");
    expect(d != 0 && d->d_type == 4 && tmp_stat("/tmp", 1, &st) == 0 && d->d_ino == st.st_ino,
           "'..' is there, and is its parent");
    for (off = 0, count = 0, n2 = 0; off < n; off += dent(buf, off)->d_reclen) {
        count++;
        /* Not consecutive: a position where the filesystem stores its own "."
         * has no record. Increasing is what a program may rely on. */
        expect((dent(buf, off)->d_reclen & 7u) == 0u && dent(buf, off)->d_off > n2,
               "every record is a multiple of 8 long and carries a later position than the one before");
        n2 = (long)dent(buf, off)->d_off;
    }
    expect(count == 6, "six entries: the two dots and the four made");
    expect(SYS3(217, (uint64_t)fd, buf, 4096) == 0, "and then the end, which is 0");
    /* The position is the description's, and lseek moves it. */
    expect(SYS3(8, (uint64_t)fd, 0, 0) == 0 && SYS3(217, (uint64_t)fd, buf, 4096) == n,
           "lseek to 0 starts the directory again");
    d = dent_named(buf, n, "..");
    off = d ? (long)d->d_off : 0;
    expect(SYS3(8, (uint64_t)fd, (uint64_t)off, 0) == off && (n2 = SYS3(217, (uint64_t)fd, buf, 4096)) > 0 &&
           dent_named(buf, n2, ".") == 0 && dent_named(buf, n2, "..") == 0 && dent_named(buf, n2, "sub") != 0 &&
           dent_named(buf, n2, "link") != 0,
           "and to the position a record named resumes after that record");
    /* A buffer that holds one record at a time lists the same entries. */
    (void)SYS3(8, (uint64_t)fd, 0, 0);
    for (count = 0; (n2 = SYS3(217, (uint64_t)fd, buf, 80)) > 0; ) {
        for (off = 0; off < n2; off += dent(buf, off)->d_reclen) {
            count++;
        }
    }
    expect(count == 6 && n2 == 0, "a small buffer gets the same six, a few at a time");
    (void)SYS3(8, (uint64_t)fd, 0, 0);
    expect(SYS3(217, (uint64_t)fd, buf, 16) == -VIBEOS_EINVAL, "a buffer too small for one record is EINVAL");
    /* The call before getdents64: the type is the record's last byte. */
    (void)SYS3(8, (uint64_t)fd, 0, 0);
    n = SYS3(78, (uint64_t)fd, buf, 4096);
    for (off = 0, count = 0; off < n; ) {
        const linux_dirent_t *o = (const linux_dirent_t *)(const void *)((const uint8_t *)kf_uptr(buf) + off);
        uint8_t type = ((const uint8_t *)kf_uptr(buf))[off + o->d_reclen - 1];
        count++;
        if (strcmp(o->d_name, longname) == 0) {
            expect(type == 8 && o->d_ino != 0u && o->d_off > 2u, "getdents: the long name, a file");
            count += 100;
        }
        if (strcmp(o->d_name, "link") == 0) {
            expect(type == 10, "getdents: the link's type is in the record's last byte");
            count += 100;
        }
        off += o->d_reclen;
    }
    expect(n > 0 && count == 206, "getdents lists the same six entries in its own record");
    expect(SYS3(78, (uint64_t)fd, buf, 4096) == 0, "and then the end");
    /* Not a directory. */
    fd = tmp_open("/tmp/d/f", 0, 0);
    expect(SYS3(217, (uint64_t)fd, buf, 4096) == -VIBEOS_ENOTDIR && SYS3(78, (uint64_t)fd, buf, 4096) == -VIBEOS_ENOTDIR &&
           SYS3(217, 99, buf, 4096) == -VIBEOS_EBADF, "a file is ENOTDIR, no descriptor EBADF");
    /* The fake's root lists '.' itself, as FAT does in every directory but its
     * root: it must come out once. */
    kf_fs_add("/r", 0, 0, 1);
    kf_fs_add("/r/.", 0, 0, 1);
    kf_fs_add("/r/..", 0, 0, 1);
    kf_fs_add("/r/x", "1", 1, 0);
    fd = SYS2(2, ustr("/r"), 0x10000);
    n = SYS3(217, (uint64_t)fd, buf, 4096);
    for (off = 0, count = 0; off < n; off += dent(buf, off)->d_reclen) {
        count++;
    }
    expect(count == 3 && dent_named(buf, n, "x") != 0,
           "a filesystem that stores '.' and '..' does not get them listed twice");
    expect(kf_lock_imbalance() == 0, "getdents released every lock it took");
}

static void t_xattr_and_unshare(void) {
    uint64_t buf, big;
    long fd;
    int me;

    me = fresh(113);
    buf = kf_ualloc(64);
    big = kf_ualloc(300);
    memset(kf_uptr(big), 'n', 299);
    ((char *)kf_uptr(big))[299] = 0;
    tmp_put("/tmp/x", "x");
    fd = tmp_open("/tmp/x", 2, 0);
    expect(sys(188, ustr("/tmp/x"), ustr("user.k"), buf, 4, 0, 0, 0) == -VIBEOS_EOPNOTSUPP &&
           sys(189, ustr("/tmp/x"), ustr("user.k"), buf, 4, 0, 0, 0) == -VIBEOS_EOPNOTSUPP &&
           sys(190, (uint64_t)fd, ustr("user.k"), buf, 4, 0, 0, 0) == -VIBEOS_EOPNOTSUPP,
           "setting an attribute is EOPNOTSUPP: no filesystem here stores them");
    expect(SYS4(191, ustr("/tmp/x"), ustr("user.k"), buf, 64) == -VIBEOS_EOPNOTSUPP &&
           SYS4(192, ustr("/tmp/x"), ustr("user.k"), buf, 64) == -VIBEOS_EOPNOTSUPP &&
           SYS4(193, (uint64_t)fd, ustr("user.k"), buf, 64) == -VIBEOS_EOPNOTSUPP, "so is getting one");
    expect(SYS3(194, ustr("/tmp/x"), buf, 64) == 0 && SYS3(195, ustr("/tmp/x"), buf, 64) == 0 &&
           SYS3(196, (uint64_t)fd, buf, 64) == 0, "the list is empty, which is how ls and cp find out");
    expect(SYS2(197, ustr("/tmp/x"), ustr("user.k")) == -VIBEOS_EOPNOTSUPP &&
           SYS2(198, ustr("/tmp/x"), ustr("user.k")) == -VIBEOS_EOPNOTSUPP &&
           SYS2(199, (uint64_t)fd, ustr("user.k")) == -VIBEOS_EOPNOTSUPP, "removing one is EOPNOTSUPP");
    expect(sys(188, ustr("/tmp/nope"), ustr("user.k"), buf, 4, 0, 0, 0) == -VIBEOS_ENOENT &&
           SYS4(191, ustr("/tmp/nope"), ustr("user.k"), buf, 64) == -VIBEOS_ENOENT &&
           SYS3(194, ustr("/tmp/nope"), buf, 64) == -VIBEOS_ENOENT &&
           SYS2(199, 99, ustr("user.k")) == -VIBEOS_EBADF && SYS3(196, 99, buf, 64) == -VIBEOS_EBADF,
           "a file that is not there is said first: ENOENT, EBADF");
    expect(sys(188, ustr("/tmp/x"), ustr("user.k"), buf, 4, 4, 0, 0) == -VIBEOS_EINVAL,
           "a setxattr flag Linux lacks is EINVAL");
    expect(sys(188, ustr("/tmp/x"), ustr(""), buf, 4, 0, 0, 0) == -VIBEOS_ERANGE &&
           SYS4(191, ustr("/tmp/x"), big, buf, 64) == -VIBEOS_ERANGE, "an empty or over-long name is ERANGE");
    expect(sys(188, ustr("/tmp/x"), ustr("user.k"), buf, 65537, 0, 0, 0) == -VIBEOS_E2BIG,
           "a value over 64 KiB is E2BIG");
    expect(SYS4(191, ustr("/tmp/x"), 0x1000, buf, 64) == -VIBEOS_EFAULT, "a name outside user memory is EFAULT");
    (void)SYS2(88, ustr("nowhere"), ustr("/tmp/dangling"));
    expect(SYS4(192, ustr("/tmp/dangling"), ustr("user.k"), buf, 64) == -VIBEOS_EOPNOTSUPP &&
           SYS4(191, ustr("/tmp/dangling"), ustr("user.k"), buf, 64) == -VIBEOS_ENOENT,
           "the l forms ask about the link, the others about what it points at");

    /* close_range's UNSHARE: nothing to do for a process of one thread. */
    expect(SYS1(32, (uint64_t)fd) > fd &&
           SYS3(436, (uint64_t)fd, ~0ull, 2 /* CLOSE_RANGE_UNSHARE */) == 0 &&
           SYS1(3, (uint64_t)fd) == -VIBEOS_EBADF,
           "CLOSE_RANGE_UNSHARE in a single-threaded process closes the range");
    ks_ps(me)->files_users = 2u;
    fd = tmp_open("/tmp/x", 0, 0);
    expect(SYS3(436, (uint64_t)fd, ~0ull, 2) == -VIBEOS_EINVAL && SYS1(3, (uint64_t)fd) == 0,
           "and is refused, closing nothing, when other threads share the table");
    ks_ps(me)->files_users = 1u;
}

/* ---- L1 step 7: the console as a terminal ---------------------------------------------- */

static long tty_get(linux_termios_t *out) {
    uint64_t u = kf_ualloc(sizeof(*out));
    long r = SYS3(16, 0, 0x5401 /* TCGETS */, u);
    memcpy(out, kf_uptr(u), sizeof(*out));
    return r;
}

static long tty_set(const linux_termios_t *in, uint64_t req) {
    uint64_t u = kf_ualloc(sizeof(*in));
    memcpy(kf_uptr(u), in, sizeof(*in));
    return SYS3(16, 0, req, u);
}

/* Read from the console: the bytes, or the outcome of a call that did not return. */
static long tty_read(char *out, uint64_t cap, kf_outcome_t *how) {
    uint64_t u = kf_ualloc(cap);
    long r = sys(0, 0, u, cap, 0, 0, 0, how);
    if (r > 0) {
        memcpy(out, kf_uptr(u), (size_t)r);
        out[r] = 0;
    }
    return r;
}

static void t_terminal_modes(void) {
    linux_termios_t t, raw;
    linux_winsize_t *ws;
    kf_outcome_t how;
    char got[80];
    uint64_t u, fds;
    size_t mark;
    long fd;

    fresh(120);
    u = kf_ualloc(64);
    fds = kf_ualloc(8);
    ws = (linux_winsize_t *)kf_uptr(u);
    expect(tty_get(&t) == 0 && (t.c_lflag & 0xBu) == 0xBu && (t.c_iflag & 0x100u) && (t.c_oflag & 5u) == 5u &&
           t.c_cc[6] == 1 && t.c_cc[2] == 127 && t.c_cc[4] == 4,
           "TCGETS: a terminal, canonical and echoing, as one starts");
    expect(SYS3(16, 0, 0x5413 /* TIOCGWINSZ */, u) == 0 && ws->ws_row == 25 && ws->ws_col == 80,
           "TIOCGWINSZ: 25 by 80");
    ws->ws_row = 50; ws->ws_col = 132;
    expect(SYS3(16, 0, 0x5414 /* TIOCSWINSZ */, u) == 0 && (ws->ws_row = 0, SYS3(16, 0, 0x5413, u) == 0) &&
           ws->ws_row == 50 && ws->ws_col == 132, "and TIOCSWINSZ changes it");
    expect(SYS3(16, 0, 0x5401, 0x1000) == -VIBEOS_EFAULT && SYS3(16, 0, 0x5402, 0x1000) == -VIBEOS_EFAULT &&
           SYS3(16, 0, 0x5413, 0x1000) == -VIBEOS_EFAULT, "a structure outside user memory is EFAULT");
    tmp_put("/tmp/f", "abcdef");
    fd = tmp_open("/tmp/f", 0, 0);
    (void)SYS2(293, fds, 0);
    expect(SYS3(16, (uint64_t)fd, 0x5401, u) == -VIBEOS_ENOTTY &&
           SYS3(16, (uint64_t)((int32_t *)kf_uptr(fds))[0], 0x5413, u) == -VIBEOS_ENOTTY &&
           SYS3(16, 99, 0x5401, u) == -VIBEOS_EBADF,
           "a file and a pipe are not terminals: ENOTTY, which is how a C library finds out");

    /* Canonical: a line at a time, erasable until Enter. */
    kf_type("ab\bc\nrest");
    expect(tty_read(got, 64, 0) == 3 && strcmp(got, "ac\n") == 0, "a read returns the line, the erased character gone");
    expect(strstr(kf_console(), "ab\b \bc") != 0, "what was typed was echoed, and the erase rubbed it out");
    (void)tty_read(got, 64, &how);
    expect(how == KF_BLOCKED, "half a line is not something to read: the read waits - it used to return it");
    kf_type("\n");
    expect(tty_read(got, 64, 0) == 5 && strcmp(got, "rest\n") == 0, "and returns the whole line once it is finished");
    kf_type("abcdef\n");
    expect(tty_read(got, 4, 0) == 4 && strcmp(got, "abcd") == 0 && tty_read(got, 64, 0) == 3 &&
           strcmp(got, "ef\n") == 0, "a line longer than the buffer is read in pieces");
    kf_type("one\ntwo\n");
    expect(tty_read(got, 64, 0) == 4 && strcmp(got, "one\n") == 0 && tty_read(got, 64, 0) == 4 &&
           strcmp(got, "two\n") == 0, "two lines typed ahead are two reads");
    kf_type("abc\025xy\n");
    expect(tty_read(got, 64, 0) == 3 && strcmp(got, "xy\n") == 0, "the kill character erases the line so far");
    kf_type("ok\r");
    expect(tty_read(got, 64, 0) == 3 && strcmp(got, "ok\n") == 0, "a carriage return typed is a newline read");
    kf_type("\004");
    expect(tty_read(got, 64, &how) == 0 && how == KF_RETURNED, "end-of-file on an empty line is a read of nothing");
    kf_type("ab\004");
    expect(tty_read(got, 64, 0) == 2 && strcmp(got, "ab") == 0, "and after some bytes it hands those over as they are");

    /* ECHO off. */
    t.c_lflag &= ~0x8u;
    expect(tty_set(&t, 0x5402 /* TCSETS */) == 0, "TCSETS");
    mark = strlen(kf_console());
    kf_type("secret\n");
    expect(tty_read(got, 64, 0) == 7 && strcmp(got, "secret\n") == 0 && strstr(kf_console() + mark, "secret") == 0,
           "with ECHO off what is typed is read and not shown");
    t.c_lflag |= 0x8u;

    /* Raw: a byte at a time, as typed. */
    raw = t;
    raw.c_lflag &= ~0xAu;   /* ICANON and ECHO off */
    raw.c_cc[6] = 1;        /* VMIN */
    expect(tty_set(&raw, 0x5402) == 0 && tty_get(&t) == 0 && (t.c_lflag & 0xAu) == 0u && (t.c_lflag & 1u),
           "the modes set are the modes got back, the ones not honoured too");
    mark = strlen(kf_console());
    kf_type("x");
    expect(tty_read(got, 64, 0) == 1 && got[0] == 'x', "raw: one key is one read, with no Enter");
    kf_type("\b\n");
    expect(tty_read(got, 64, 0) == 2 && got[0] == '\b' && got[1] == '\n', "an erase is a byte like any other");
    kf_type("pq");
    expect(tty_read(got, 1, 0) == 1 && got[0] == 'p' && tty_read(got, 1, 0) == 1 && got[0] == 'q',
           "a read for one byte takes one and leaves the next");
    expect(kf_console()[mark] == 0, "and nothing was echoed");
    (void)tty_read(got, 64, &how);
    expect(how == KF_BLOCKED, "with MIN 1 and nothing typed, the read waits");
    raw.c_cc[6] = 0;
    expect(tty_set(&raw, 0x5403 /* TCSETSW */) == 0 && tty_read(got, 64, &how) == 0 && how == KF_RETURNED,
           "with MIN 0 it returns at once, with nothing");

    /* Changing modes with a line half typed, and flushing. */
    raw.c_cc[6] = 1;
    t.c_lflag |= 0xAu;
    (void)tty_set(&t, 0x5402);
    kf_type("par");
    (void)tty_read(got, 64, &how);
    expect(how == KF_BLOCKED, "canonical again: half a line waits");
    expect(tty_set(&raw, 0x5402) == 0 && tty_read(got, 64, 0) == 3 && strncmp(got, "par", 3) == 0,
           "leaving canonical mode hands over what was typed so far");
    (void)tty_set(&t, 0x5402);
    kf_type("junk");
    (void)tty_read(got, 64, &how);
    expect(tty_set(&t, 0x5404 /* TCSETSF */) == 0, "TCSETSF");
    kf_type("\n");
    expect(tty_read(got, 64, 0) == 1 && got[0] == '\n', "TCSETSF throws away what was typed and not read");

    /* Two lines that reached the terminal while it was raw - counted by
     * FIONREAD, which takes what was typed - are still two reads once it is
     * canonical again. */
    {
        uint64_t n = kf_ualloc(4);
        (void)tty_set(&raw, 0x5402);
        kf_type("a\nb\n");
        expect(SYS3(16, 0, 0x541B, n) == 0 && *(int32_t *)kf_uptr(n) == 4, "four bytes typed ahead in raw mode");
        (void)tty_set(&t, 0x5402);
        expect(tty_read(got, 64, 0) == 2 && strcmp(got, "a\n") == 0 && tty_read(got, 64, 0) == 2 &&
               strcmp(got, "b\n") == 0, "canonical again, they are read a line at a time");
    }

    /* Output: a newline goes out as CR LF until the program says otherwise. */
    mark = strlen(kf_console());
    (void)SYS3(1, 1, ustr("a\n"), 2);
    expect(strstr(kf_console() + mark, "a\r\n") != 0, "a newline written goes out as CR LF");
    t.c_oflag &= ~1u;   /* OPOST off */
    (void)tty_set(&t, 0x5402);
    mark = strlen(kf_console());
    (void)SYS3(1, 1, ustr("b\n"), 2);
    expect(strstr(kf_console() + mark, "b\n") != 0 && strstr(kf_console() + mark, "b\r\n") == 0,
           "and as it is with output processing off");
    expect(kf_lock_imbalance() == 0, "the terminal released every lock it took");
}

static void t_descriptor_requests_and_poll(void) {
    linux_pollfd_t *p;
    kf_outcome_t how;
    char got[16];
    uint64_t u, up, fds, iov;
    int32_t *v, *f;
    long fd;

    fresh(121);
    u = kf_ualloc(16);
    up = kf_ualloc(4 * sizeof(linux_pollfd_t));
    fds = kf_ualloc(8);
    iov = kf_ualloc(32);
    v = (int32_t *)kf_uptr(u);
    p = (linux_pollfd_t *)kf_uptr(up);
    f = (int32_t *)kf_uptr(fds);
    tmp_put("/tmp/f", "abcdef");
    fd = tmp_open("/tmp/f", 0, 0);
    (void)SYS2(293, fds, 0);

    /* The four requests any descriptor answers. */
    expect(SYS3(16, (uint64_t)fd, 0x5451 /* FIOCLEX */, 0) == 0 && SYS3(72, (uint64_t)fd, 1, 0) == 1 &&
           SYS3(16, (uint64_t)fd, 0x5450 /* FIONCLEX */, 0) == 0 && SYS3(72, (uint64_t)fd, 1, 0) == 0,
           "FIOCLEX and FIONCLEX set and clear close-on-exec");
    (void)SYS3(0, (uint64_t)fd, kf_ualloc(2), 2);
    expect(SYS3(16, (uint64_t)fd, 0x541B /* FIONREAD */, u) == 0 && *v == 4, "FIONREAD on a file: what is left to read");
    (void)SYS3(1, (uint64_t)f[1], ustr("xyz"), 3);
    expect(SYS3(16, (uint64_t)f[0], 0x541B, u) == 0 && *v == 3, "on a pipe: what is in it");
    kf_type("abc\n");
    expect(SYS3(16, 0, 0x541B, u) == 0 && *v == 4, "on the terminal: the finished line, read or not yet looked at");
    expect(tty_read(got, 16, 0) == 4, "which a read then gets");
    *v = 1;
    expect(SYS3(16, 0, 0x5421 /* FIONBIO */, u) == 0 && tty_read(got, 16, &how) == -VIBEOS_EAGAIN && how == KF_RETURNED,
           "FIONBIO makes a read with nothing typed EAGAIN where it waited");
    *v = 0;
    (void)SYS3(16, 0, 0x5421, u);
    (void)tty_read(got, 16, &how);
    expect(how == KF_BLOCKED, "and back");
    expect(SYS3(16, (uint64_t)fd, 0x541B, 0x1000) == -VIBEOS_EFAULT && SYS3(16, 0, 0x5421, 0x1000) == -VIBEOS_EFAULT,
           "their pointers are judged too");

    /* poll. */
    p[0].fd = 0; p[0].events = 1 /* POLLIN */; p[0].revents = 0x7fff;
    /* "Returned 0" and "did not return" are told apart on purpose: a call that
     * waits leaves 0 behind too, and a timeout of 0 that waited was NOT RED. */
    expect(sys(7, up, 1, 0, 0, 0, 0, &how) == 0 && how == KF_RETURNED && p[0].revents == 0,
           "poll: nothing typed, nothing to read, and revents says so - at once, with a timeout of 0");
    kf_type("li");
    expect(sys(7, up, 1, 0, 0, 0, 0, &how) == 0 && how == KF_RETURNED,
           "half a line is not readable in canonical mode");
    kf_type("ne\n");
    expect(SYS3(7, up, 1, 0) == 1 && p[0].revents == 1, "a finished line is");
    p[0].events = 4 /* POLLOUT */;
    expect(SYS3(7, up, 1, 0) == 1 && p[0].revents == 4,
           "asked only whether it can be written, it is not told that it can be read");
    expect(tty_read(got, 16, 0) == 5 && strncmp(got, "line\n", 5) == 0, "and poll took none of it");
    p[0].fd = 1; p[0].events = 4 /* POLLOUT */;
    p[1].fd = f[0]; p[1].events = 1;
    p[2].fd = -1; p[2].events = 1; p[2].revents = 0x7fff;
    p[3].fd = 99; p[3].events = 1;
    expect(SYS3(7, up, 4, 0) == 3 && p[0].revents == 4 && p[1].revents == 1 && p[2].revents == 0 &&
           p[3].revents == 0x20, "the console can be written, the pipe read; a negative fd is skipped, a closed one is POLLNVAL");
    (void)SYS3(0, (uint64_t)f[0], kf_ualloc(8), 8);
    p[0].fd = f[0]; p[0].events = 1;
    p[1].fd = f[1]; p[1].events = 4;
    expect(SYS3(7, up, 2, 0) == 1 && p[0].revents == 0 && p[1].revents == 4,
           "an empty pipe: nothing to read, room to write");
    (void)sys(7, up, 1, (uint64_t)-1, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "with no timeout poll waits for one of them");
    (void)SYS1(3, (uint64_t)f[1]);
    expect(SYS3(7, up, 1, 0) == 1 && p[0].revents == 0x10,
           "an empty pipe nobody can write any more is hung up, and only that (Linux's pipe_poll, LTP's poll03)");
    expect(SYS3(7, up, 2000, 0) == -VIBEOS_EINVAL && SYS3(7, 0x1000, 1, 0) == -VIBEOS_EFAULT,
           "more descriptors than a process may have is EINVAL, an array outside user memory EFAULT");

    /* A writev of two pieces is one write: one line on the console. */
    {
        uint64_t *q = (uint64_t *)kf_uptr(iov);
        q[0] = ustr("TOGE"); q[1] = 4;
        q[2] = ustr("THER\n"); q[3] = 5;
        expect(SYS3(20, 1, iov, 2) == 9 && strstr(kf_console(), "write(ring3): TOGETHER") != 0,
               "a writev of two pieces reaches the console as one write");
    }
    expect(kf_lock_imbalance() == 0, "poll and the descriptor requests released every lock they took");
}

/* ---- L3 step 1: mmap by the rules -------------------------------------------------------
 *
 * The fake kernel has real page tables since this step (ksvc_fake.c): a mapped
 * address is read and written through them with kf_peek and kf_poke. */

#define MAP_PRIV_ANON 0x22u
#define MMAP(addr, len, prot, flags, fd, off) \
    sys(9, (uint64_t)(addr), (uint64_t)(len), (uint64_t)(prot), (uint64_t)(flags), (uint64_t)(fd), (uint64_t)(off), 0)

static int mem_is(uint64_t va, uint8_t byte, uint64_t n) {
    uint8_t b = 0;
    uint64_t i;
    for (i = 0; i < n; i++) {
        if (kf_peek(va + i, &b, 1) != 0 || b != byte) {
            return 0;
        }
    }
    return 1;
}

static void t_mmap_placement(void) {
    long a, b, c, h;
    uint8_t x = 'A', y = 0;

    fresh(130);
    a = MMAP(0, 8192, 3, MAP_PRIV_ANON, -1, 0);
    expect(a >= 0x20000000l && (a & 0xFFF) == 0 && mem_is((uint64_t)a, 0, 8192),
           "an anonymous mapping: page aligned, in the arena, zeroed");
    expect(kf_poke((uint64_t)a + 8191u, &x, 1) == 0 && kf_peek((uint64_t)a + 8191u, &y, 1) == 0 && y == 'A',
           "and writable to its last byte");
    expect(kf_poke((uint64_t)a + 8192u, &x, 1) != 0, "and not one byte past it");
    b = MMAP(0, 4096, 3, MAP_PRIV_ANON, -1, 0);
    expect(b >= a + 8192 || b + 4096 <= a, "a second mapping does not overlap the first");
    expect(MMAP(0, 4096, 3, 0x20u, -1, 0) == -VIBEOS_EINVAL, "neither private nor shared is EINVAL");
    expect(MMAP(0, 0, 3, MAP_PRIV_ANON, -1, 0) == -VIBEOS_EINVAL, "no length is EINVAL");
    expect(MMAP(0, ~0ull, 3, MAP_PRIV_ANON, -1, 0) == -VIBEOS_ENOMEM, "a length that wraps is ENOMEM");
    expect(MMAP(0, 4096, 3, MAP_PRIV_ANON, 0, 0) > 0, "MAP_ANONYMOUS ignores the descriptor, whatever it is");

    /* At an address the program names. */
    c = MMAP(0x30000000ull, 3 * 4096, 3, MAP_PRIV_ANON | 0x10u, -1, 0);
    expect(c == 0x30000000l, "MAP_FIXED maps at the address asked for");
    (void)kf_poke(0x30000000ull, &x, 1);
    (void)kf_poke(0x30001000ull, &x, 1);
    (void)kf_poke(0x30002000ull, &x, 1);
    {
        /* What was there is given back, not buried: replacing a page with a
         * page leaves the machine with as many free frames as before. Reading
         * zeros is not enough to know - mapping over the old page without
         * releasing it reads zeros too, and went NOT RED. */
        uint64_t free_before = vibeos_mm_stats()->frames_free;
        expect(MMAP(0x30001000ull, 4096, 3, MAP_PRIV_ANON | 0x10u, -1, 0) == 0x30001000l &&
               mem_is(0x30001000ull, 0, 4096) && mem_is(0x30000000ull, 'A', 1) && mem_is(0x30002000ull, 'A', 1),
               "MAP_FIXED over the middle of a mapping replaces that page and leaves its neighbours");
        expect(vibeos_mm_stats()->frames_free == free_before,
               "and the page that was there went back to the allocator");
    }
    expect(MMAP(0x30000800ull, 4096, 3, MAP_PRIV_ANON | 0x10u, -1, 0) == -VIBEOS_EINVAL,
           "a fixed address that is not page aligned is EINVAL");
    expect(MMAP(0x1000ull, 4096, 3, MAP_PRIV_ANON | 0x10u, -1, 0) == -VIBEOS_ENOMEM,
           "a fixed address where no mapping may go is ENOMEM");
    expect(MMAP(0x30002000ull, 4096, 3, MAP_PRIV_ANON | 0x100000u, -1, 0) == -VIBEOS_EEXIST &&
           mem_is(0x30002000ull, 'A', 1), "MAP_FIXED_NOREPLACE refuses a range in use and touches nothing");
    expect(MMAP(0x30003000ull, 4096, 3, MAP_PRIV_ANON | 0x100000u, -1, 0) == 0x30003000l,
           "and maps a free one where it was asked");
    /* A fixed mapping that covers a mapped page and a free one is one region
     * afterwards, described as it was asked for. The pages take care of
     * themselves - mapping over a page releases it - so what removing the old
     * region first is for is the *list*: left alone, it refuses the new region
     * as an overlap, and the page past the old one is mapped and described by
     * nothing, which mprotect then refuses. */
    expect(MMAP(0x30003000ull, 8192, 3, MAP_PRIV_ANON | 0x10u, -1, 0) == 0x30003000l &&
           SYS3(10, 0x30004000ull, 4096, 1) == 0 && kf_poke(0x30004000ull, &x, 1) != 0,
           "MAP_FIXED over a mapped page and a free one: both described, and mprotect knows the second");

    /* A hint is honoured when it is free, and is only a hint when it is not. */
    h = MMAP(0x50000000ull, 4096, 3, MAP_PRIV_ANON, -1, 0);
    expect(h == 0x50000000l, "a free address hinted is the address given");
    h = MMAP(0x50000000ull, 8192, 3, MAP_PRIV_ANON, -1, 0);
    expect(h > 0 && h != 0x50000000l && (h + 8192 <= 0x50000000l || h >= 0x50001000l) &&
           mem_is(0x50000000ull, 0, 1), "one in use is placed elsewhere, and what was there is untouched");

    /* The arena steps over what a fixed mapping took. */
    a = MMAP(0, 4096, 3, MAP_PRIV_ANON, -1, 0);
    expect(MMAP((uint64_t)a + 4096u, 4096, 3, MAP_PRIV_ANON | 0x10u, -1, 0) == a + 4096 &&
           kf_poke((uint64_t)a + 4096u, &x, 1) == 0, "a fixed mapping exactly where the arena would go next");
    b = MMAP(0, 8192, 3, MAP_PRIV_ANON, -1, 0);
    expect(b > 0 && (b >= a + 8192 || b + 8192 <= a + 4096) && mem_is((uint64_t)a + 4096u, 'A', 1),
           "the next mapping goes around it - it used to be handed the same pages");

    /* munmap and mprotect on what mmap made. */
    expect(SYS2(11, 0x30000000ull, 4096) == 0 && kf_peek(0x30000000ull, &y, 1) != 0 &&
           mem_is(0x30002000ull, 'A', 1), "munmap takes a page out and leaves the rest");
    expect(SYS3(10, 0x30002000ull, 4096, 1) == 0 && kf_poke(0x30002000ull, &x, 1) != 0 &&
           mem_is(0x30002000ull, 'A', 1), "mprotect to read-only: readable, not writable");
    expect(kf_lock_imbalance() == 0, "mmap released every lock it took");
}

/* What byte i of the test file holds. Not i * 7 + 3 alone: that repeats every
 * 256 bytes, so byte 4096 was byte 0 and a mapping that ignored its offset read
 * the right value - which is how that sabotage went NOT RED. */
#define PAT(i) ((uint8_t)((i) * 7u + ((i) >> 8) * 13u + 3u))

static void t_mmap_files(void) {
    uint64_t buf, fds;
    uint8_t x = 'Z', y = 0;
    long fd, wfd, m, m2, pid;
    int parent, child;
    uint32_t i;

    parent = fresh(131);
    buf = kf_ualloc(5000);
    fds = kf_ualloc(8);
    for (i = 0; i < 5000u; i++) {
        ((uint8_t *)kf_uptr(buf))[i] = PAT(i);
    }
    fd = tmp_open("/tmp/m", 0x42 /* O_CREAT|O_RDWR */, 0644);
    expect(SYS3(1, (uint64_t)fd, buf, 5000) == 5000, "a file of 5000 bytes");

    m = MMAP(0, 8192, 1 /* PROT_READ */, 2 /* MAP_PRIVATE */, fd, 0);
    expect(m > 0, "a private mapping of a file");
    for (i = 0; i < 5000u && m > 0; i += 613u) {
        (void)kf_peek((uint64_t)m + i, &y, 1);
        if (y != PAT(i)) {
            break;
        }
    }
    expect(i >= 5000u && kf_peek((uint64_t)m + 4999u, &y, 1) == 0 && y == PAT(4999u),
           "holds the file's bytes, across a page boundary");
    expect(mem_is((uint64_t)m + 5000u, 0, 8192 - 5000), "and zeros after the file's end");
    expect(kf_poke((uint64_t)m, &x, 1) != 0, "mapped read-only, it cannot be written");
    m2 = MMAP(0, 4096, 1, 2, fd, 4096);
    expect(m2 > 0 && kf_peek((uint64_t)m2, &y, 1) == 0 && y == PAT(4096u),
           "an offset starts the mapping that far into the file");

    /* Private means private: a store stays in the process. */
    m2 = MMAP(0, 4096, 3, 2, fd, 0);
    expect(m2 > 0 && kf_poke((uint64_t)m2, &x, 1) == 0 && mem_is((uint64_t)m2, 'Z', 1) &&
           SYS4(17, (uint64_t)fd, buf, 1, 0) == 1 && ((uint8_t *)kf_uptr(buf))[0] == PAT(0u),
           "a store into a private mapping does not reach the file");
    expect(SYS1(3, (uint64_t)fd) == 0 && kf_peek((uint64_t)m + 100u, &y, 1) == 0 &&
           y == PAT(100u), "the mapping outlives the descriptor it was made from");

    /* What cannot be mapped, and how it is said. */
    fd = tmp_open("/tmp/m", 0, 0);
    wfd = tmp_open("/tmp/m", 1, 0);
    (void)SYS2(293, fds, 0);
    expect(MMAP(0, 4096, 1, 2, fd, 100) == -VIBEOS_EINVAL, "an offset that is not a page multiple is EINVAL");
    expect(MMAP(0, 4096, 1, 2, 99, 0) == -VIBEOS_EBADF, "no descriptor is EBADF");
    expect(MMAP(0, 0, 2, 1, -1, 0) == -VIBEOS_EBADF && MMAP(0, 0, 1, 2, fd, 0) == -VIBEOS_EINVAL &&
           MMAP(0, 0, 3, MAP_PRIV_ANON, -1, 0) == -VIBEOS_EINVAL,
           "a length of zero is EINVAL, but a descriptor that is not open is EBADF first");
    expect(MMAP(0, 4096, 1, 2, wfd, 0) == -VIBEOS_EACCES, "a descriptor that cannot be read is EACCES");
    expect(MMAP(0, 4096, 1, 2, ((int32_t *)kf_uptr(fds))[0], 0) == -VIBEOS_ENODEV &&
           MMAP(0, 4096, 1, 2, tmp_open("/tmp", 0x10000, 0), 0) == -VIBEOS_ENODEV &&
           MMAP(0, 4096, 1, 2, 0, 0) == -VIBEOS_ENODEV, "a pipe, a directory and the console are ENODEV");
    /* The fake's own root filesystem: another implementation under the same call. */
    kf_fs_add("/f", "root file", 9, 0);
    m2 = MMAP(0, 4096, 1, 2, SYS2(2, ustr("/f"), 0), 0);
    expect(m2 > 0 && kf_peek((uint64_t)m2 + 5u, &y, 1) == 0 && y == 'f' && mem_is((uint64_t)m2 + 9u, 0, 16),
           "a file on another filesystem maps the same way");

    /* Fork: the child has the mapping, and each side's writes are its own. */
    m2 = MMAP(0, 4096, 3, MAP_PRIV_ANON, -1, 0);
    (void)kf_poke((uint64_t)m2, &x, 1);
    pid = SYS0(57);
    child = slot_of_pid(pid);
    expect(pid > 0 && child >= 0, "fork");
    kf_set_current(child);
    expect(mem_is((uint64_t)m2, 'Z', 1) && kf_peek((uint64_t)m + 100u, &y, 1) == 0 &&
           y == PAT(100u), "the child sees what the parent mapped");
    y = 'c';
    expect(kf_poke((uint64_t)m2, &y, 1) == 0 && mem_is((uint64_t)m2, 'c', 1), "the child writes its copy");
    kf_set_current(parent);
    expect(mem_is((uint64_t)m2, 'Z', 1), "and the parent's is as it was");

    /* brk, which could not run here before either. */
    expect(SYS1(12, 0) == 0x10000000l && SYS1(12, 0x10000000ull + 5000u) == 0x10002000l &&
           kf_poke(0x10001fffull, &x, 1) == 0 && mem_is(0x10000000ull, 0, 16),
           "brk grows the heap by whole zeroed pages");
    expect(SYS1(12, 0x10001000ull) == 0x10001000l && kf_peek(0x10001000ull, &y, 1) != 0,
           "and gives them back");
    expect(kf_lock_imbalance() == 0, "file mappings released every lock they took");
}

/* ---- /proc (L3 step 3) --------------------------------------------------------------- */

/* The kibibytes on the line of /proc/meminfo that starts with `name`, or -1. */
static long meminfo_kb(const char *text, const char *name) {
    const char *p = strstr(text, name);
    long v = 0;

    if (!p || (p != text && p[-1] != '\n')) {
        return -1;
    }
    p += strlen(name);
    if (*p++ != ':') {
        return -1;
    }
    while (*p == ' ') {
        p++;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p++ - '0');
    }
    return strncmp(p, " kB\n", 4) == 0 ? v : -1;
}

static void t_procfs(void) {
    uint64_t buf, st = 0;
    char text[600];
    linux_stat_t sb;
    long fd, n, total, avail;

    fresh(161);
    buf = kf_ualloc(600);
    fd = SYS2(2, ustr("/proc/meminfo"), 0);
    n = fd >= 0 ? SYS3(0, (uint64_t)fd, buf, 599) : -1;
    expect(fd >= 0 && n > 0 && n < 599, "/proc/meminfo opens and reads");
    memset(text, 0, sizeof(text));
    if (n > 0) {
        memcpy(text, kf_uptr(buf), (size_t)n);
    }
    total = meminfo_kb(text, "MemTotal");
    avail = meminfo_kb(text, "MemAvailable");
    expect(total == (long)(vibeos_frame_total() * 4ull) && total > 0,
           "MemTotal is the memory there is, in kB, on a line laid out as Linux lays it");
    expect(avail > 0 && avail <= total && meminfo_kb(text, "MemFree") == avail &&
           meminfo_kb(text, "Cached") == 0 && meminfo_kb(text, "SwapTotal") == 0 &&
           meminfo_kb(text, "SwapFree") == 0, "and the other lines a harness reads are there");
    expect(SYS3(0, (uint64_t)fd, buf, 599) == 0, "the file ends");
    expect(sys(262, (uint64_t)(uint32_t)-100, ustr("/proc/meminfo"), (st = kf_ualloc(144)), 0, 0, 0, 0) == 0 &&
           (memcpy(&sb, kf_uptr(st), sizeof(sb)), sb.st_size == n) &&
           sb.st_mode == (VIBEOS_S_IFREG | 0444u), "its size is what a read returns, and it is read-only");
    expect(SYS3(2, ustr("/proc/meminfo"), 1, 0) == -VIBEOS_EROFS &&
           SYS3(2, ustr("/proc/new"), 0x41, 0644) == -VIBEOS_EROFS,
           "nothing under /proc is written or made");
    expect(SYS2(2, ustr("/proc/nothing"), 0) == -VIBEOS_ENOENT, "a name it does not have is ENOENT");
    fd = SYS2(2, ustr("/proc/sys/kernel/pid_max"), 0);
    n = fd >= 0 ? SYS3(0, (uint64_t)fd, buf, 599) : -1;
    expect(n == 8 && memcmp(kf_uptr(buf), "4194304\n", 8) == 0, "/proc/sys/kernel/pid_max is a number and a newline");
    fd = SYS2(2, ustr("/proc/sys/kernel/tainted"), 0);
    n = fd >= 0 ? SYS3(0, (uint64_t)fd, buf, 599) : -1;
    expect(n == 2 && memcmp(kf_uptr(buf), "0\n", 2) == 0, "/proc/sys/kernel/tainted: nothing taints this kernel");
    expect(sys(262, (uint64_t)(uint32_t)-100, ustr("/proc/sys/kernel"), st, 0, 0, 0, 0) == 0 &&
           (memcpy(&sb, kf_uptr(st), sizeof(sb)), (sb.st_mode & VIBEOS_S_IFMT) == VIBEOS_S_IFDIR) &&
           sys(262, (uint64_t)(uint32_t)-100, ustr("/proc/sys/ker"), st, 0, 0, 0, 0) == -VIBEOS_ENOENT,
           "the directories on the way to a file exist, and half a name does not");
    {
        /* /proc lists meminfo and sys, once each; /proc/sys lists kernel. */
        long d = SYS2(2, ustr("/proc"), 0x10000 /* O_DIRECTORY */);
        long got = d >= 0 ? SYS3(217, (uint64_t)d, buf, 599) : -1;
        const uint8_t *b = (const uint8_t *)kf_uptr(buf);
        int meminfo = 0, sysdir = 0, others = 0;
        long at = 0;

        while (got > 0 && at < got) {
            const char *nm = (const char *)b + at + 19;
            uint16_t rl = 0;
            memcpy(&rl, b + at + 16, 2);
            if (strcmp(nm, "meminfo") == 0) {
                meminfo += b[at + 18] == 8 /* DT_REG */;
            } else if (strcmp(nm, "sys") == 0) {
                sysdir += b[at + 18] == 4 /* DT_DIR */;
            } else if (strcmp(nm, ".") != 0 && strcmp(nm, "..") != 0) {
                others++;
            }
            at += rl;
        }
        expect(meminfo == 1 && sysdir == 1 && others > 0, "/proc lists a file and a directory, each once");
    }
}

/* ---- /proc and /dev (L2 step 6) ---------------------------------------------------------- */

/* A whole file, through open and read; the bytes read, or the open's error. */
static long read_whole(const char *path, char *out, long cap) {
    uint64_t b = kf_ualloc(4096);
    long fd = SYS2(2, ustr(path), 0), n, total = 0;

    if (fd < 0) {
        return fd;
    }
    while (total < cap - 1 &&
           (n = SYS3(0, (uint64_t)fd, b, (uint64_t)(cap - 1 - total > 4096 ? 4096 : cap - 1 - total))) > 0) {
        memcpy(out + total, kf_uptr(b), (size_t)n);
        total += n;
    }
    out[total] = 0;
    (void)SYS1(3, (uint64_t)fd);
    return total;
}

static long link_of(const char *path, char *out) {
    uint64_t b = kf_ualloc(256);
    long n = SYS3(89, ustr(path), b, 255);

    out[0] = 0;
    if (n >= 0) {
        memcpy(out, kf_uptr(b), (size_t)n);
        out[n] = 0;
    }
    return n;
}

/* Does the directory list `name`, and with which getdents64 type; -1 if not. */
static int dir_type(const char *path, const char *name) {
    uint64_t b = kf_ualloc(4096);
    long d = SYS2(2, ustr(path), 0x10000 /* O_DIRECTORY */), got;
    int type = -1;

    while (d >= 0 && (got = SYS3(217, (uint64_t)d, b, 4096)) > 0) {
        const uint8_t *p = (const uint8_t *)kf_uptr(b);
        long at = 0;

        while (at < got) {
            uint16_t rl = 0;
            memcpy(&rl, p + at + 16, 2);
            if (strcmp((const char *)p + at + 19, name) == 0) {
                type = p[at + 18];
            }
            at += rl;
        }
    }
    if (d >= 0) {
        (void)SYS1(3, (uint64_t)d);
    }
    return type;
}

static void t_procdev(void) {
    static const uint8_t seed[32] = {9, 8, 7};
    char text[4096], want[64];
    uint64_t b, b2, st;
    linux_stat_t sb, sb2;
    vibeos_image_t *img;
    long fd, n, fields = 0;
    int slot, other;

    slot = fresh(170);
    other = kf_spawn(171, 171);
    kf_set_current(slot);
    img = ks_image(slot);
    memcpy(img->exe_path, "/bin/prog", 10);
    memcpy(img->cmdline, "prog\0-x\0", 8);
    img->cmdline_len = 8;
    (void)SYS2(157, 15 /* PR_SET_NAME */, ustr("prog"));
    b = kf_ualloc(64);
    b2 = kf_ualloc(64);
    st = kf_ualloc(144);

    /* The asking process, by the link that names it. */
    expect(link_of("/proc/self", text) == 3 && strcmp(text, "170") == 0,
           "/proc/self is a link to the asking process's directory");
    expect(link_of("/proc/self/exe", text) == 9 && strcmp(text, "/bin/prog") == 0,
           "/proc/self/exe is the program, read through the links like any other path");
    n = read_whole("/proc/self/cmdline", text, sizeof(text));
    expect(n == 8 && memcmp(text, "prog\0-x\0", 8) == 0, "cmdline is the arguments, each ended by a NUL");
    n = read_whole("/proc/170/stat", text, sizeof(text));
    for (fields = 0; n > 0 && text[0]; ) {
        const char *s;
        fields = 1;
        for (s = strchr(text, ')'); s && *s; s++) {
            fields += *s == ' ';
        }
        break;
    }
    expect(n > 0 && strncmp(text, "170 (prog) R ", 13) == 0 && fields == 51 && text[n - 1] == '\n',
           "stat is Linux's line: pid, (comm), state, and fifty more fields");
    n = read_whole("/proc/self/status", text, sizeof(text));
    expect(n > 0 && strstr(text, "Name:\tprog\n") && strstr(text, "Pid:\t170\n") &&
           strstr(text, "Uid:\t0\t0\t0\t0\n") && strstr(text, "State:\tR (running)\n") &&
           strstr(text, "SigBlk:\t0000000000000000\n"),
           "status names the process, its ids and its state as Linux lays them out");
    expect(read_whole("/proc/171/stat", text, sizeof(text)) > 0 && strncmp(text, "171 (", 5) == 0,
           "another process has a directory of its own");
    expect(read_whole("/proc/172/stat", text, sizeof(text)) == -VIBEOS_ENOENT &&
           read_whole("/proc/170/nothing", text, sizeof(text)) == -VIBEOS_ENOENT,
           "a pid that is nobody, and a name a process does not have, are ENOENT");
    expect(dir_type("/proc", "170") == 4 && dir_type("/proc", "171") == 4 && dir_type("/proc", "self") == 10 &&
           dir_type("/proc", "cpuinfo") == 8,
           "/proc lists every process as a directory, self as a link, and its files");
    expect(sys(262, (uint64_t)(uint32_t)-100, ustr("/proc/170"), st, 0, 0, 0, 0) == 0 &&
           (memcpy(&sb, kf_uptr(st), sizeof(sb)), (sb.st_mode & VIBEOS_S_IFMT) == VIBEOS_S_IFDIR),
           "a process's directory is a directory");

    /* Threads (L2 step 7). The main thread waits; another reads, as LTP's
     * futex_wait03 does to see its main thread asleep - and the process is
     * the leader's state, not "running because somebody is". */
    {
        int th = kf_spawn(175, 170);

        ks_id(th)->tgid = 170;
        ks_id(th)->is_thread = 1;
        ks_id(th)->ppid = ks_id(slot)->ppid;
        ks_id(slot)->sleeping = 1;
        kf_set_current(th);
        expect(read_whole("/proc/170/stat", text, sizeof(text)) > 0 && strncmp(text, "170 (prog) S ", 13) == 0,
               "a process whose leader waits is S, while another of its threads runs");
        expect(read_whole("/proc/170/task/175/stat", text, sizeof(text)) > 0 && strncmp(text, "175 (", 5) == 0 &&
               strstr(text, ") R ") && read_whole("/proc/170/task/170/stat", text, sizeof(text)) > 0 &&
               strncmp(text, "170 (prog) S ", 13) == 0,
               "task/<tid>/stat is each thread's own: its id and its state");
        expect(read_whole("/proc/self/task/175/status", text, sizeof(text)) > 0 && strstr(text, "Pid:\t175\n") &&
               strstr(text, "Tgid:\t170\n"), "a thread's status names it and its process");
        expect(dir_type("/proc/170/task", "170") == 4 && dir_type("/proc/170/task", "175") == 4 &&
               dir_type("/proc/170", "task") == 4,
               "task lists the process's threads, each a directory");
        expect(read_whole("/proc/175/stat", text, sizeof(text)) > 0 && strncmp(text, "175 (", 5) == 0 &&
               dir_type("/proc", "175") == -1,
               "/proc/<tid> exists for a thread, and is not listed");
        expect(read_whole("/proc/171/task/175/stat", text, sizeof(text)) == -VIBEOS_ENOENT &&
               read_whole("/proc/170/task/171/stat", text, sizeof(text)) == -VIBEOS_ENOENT,
               "a thread is under its own process's task/ and nobody else's");
        expect(link_of("/proc/170/task/175/exe", text) == 9 && strcmp(text, "/bin/prog") == 0,
               "and its program is its process's");
        ks_id(slot)->sleeping = 0;
        kf_set_current(slot);
    }

    /* A process's directory is a pidfd to pidfd_send_signal, as on Linux. */
    fd = SYS2(2, ustr("/proc/171"), 0x10000 /* O_DIRECTORY */);
    expect(fd >= 0 && SYS4(424, (uint64_t)fd, 10, 0, 0) == 0 &&
           (ks_id(other)->sig_pending & (1ull << 10)) != 0,
           "pidfd_send_signal takes an open /proc/<pid> for the process");
    (void)SYS1(3, (uint64_t)fd);
    fd = SYS2(2, ustr("/proc/171/stat"), 0);
    expect(fd >= 0 && SYS4(424, (uint64_t)fd, 10, 0, 0) == -VIBEOS_EBADF, "and a file under it for nothing");
    (void)SYS1(3, (uint64_t)fd);

    /* Its descriptors, as links to what each was opened as. */
    fd = SYS3(2, ustr("/tmp/pf"), 0x42 /* O_CREAT|O_RDWR */, 0644);
    snprintf(want, sizeof(want), "/proc/self/fd/%ld", fd);
    expect(fd >= 0 && link_of(want, text) == 7 && strcmp(text, "/tmp/pf") == 0,
           "/proc/self/fd/N is a link to the file descriptor N was opened on");
    snprintf(want, sizeof(want), "%ld", fd);
    expect(dir_type("/proc/self/fd", want) == 10, "and fd lists it");
    (void)SYS1(3, (uint64_t)fd);
    snprintf(want, sizeof(want), "/proc/self/fd/%ld", fd);
    expect(link_of(want, text) == -VIBEOS_ENOENT, "a descriptor closed is gone from fd");

    /* The machine's files. */
    n = read_whole("/proc/mounts", text, sizeof(text));
    expect(n > 0 && strstr(text, "tmpfs /tmp tmpfs rw 0 0\n") && strstr(text, "proc /proc proc ro 0 0\n") &&
           strstr(text, "devtmpfs /dev devtmpfs ro 0 0\n"),
           "/proc/mounts (a link to self/mounts) lists every mount as Linux does");
    n = read_whole("/proc/cpuinfo", text, sizeof(text));
    expect(n > 0 && strstr(text, "processor\t: 0\n") && strstr(text, "vendor_id\t: FakeIntel\n") &&
           strstr(text, "flags\t\t: fpu tsc lm\n") && strstr(text, "model name\t: Fake CPU\n"),
           "/proc/cpuinfo describes the processor in Linux's layout");
    n = read_whole("/proc/version", text, sizeof(text));
    expect(n > 0 && strncmp(text, "Linux version 6.1.0-vibeos ", 27) == 0, "/proc/version agrees with uname");

    /* /dev. */
    fd = SYS2(2, ustr("/dev/null"), 2 /* O_RDWR */);
    expect(fd >= 0 && SYS3(0, (uint64_t)fd, b, 64) == 0 && SYS3(1, (uint64_t)fd, b, 10) == 10,
           "/dev/null reads nothing and takes everything");
    expect(SYS2(5, (uint64_t)fd, st) == 0 && (memcpy(&sb, kf_uptr(st), sizeof(sb)), 1) &&
           sb.st_mode == (VIBEOS_S_IFCHR | 0666u) && sb.st_rdev == 0x103u &&
           sys(262, (uint64_t)(uint32_t)-100, ustr("/dev/null"), st, 0, 0, 0, 0) == 0 &&
           (memcpy(&sb2, kf_uptr(st), sizeof(sb2)), sb2.st_ino == sb.st_ino && sb2.st_dev == sb.st_dev),
           "it is character device 1:3, and fstat of it is stat of its name");
    expect(SYS3(16, (uint64_t)fd, 0x80045200u /* RNDGETENTCNT */, b) == -VIBEOS_ENOTTY,
           "it has no entropy to report");
    (void)SYS1(3, (uint64_t)fd);
    /* The pool's entropy, by the random devices' ioctl and by /proc, the same
     * number - LTP's ioctl07 compares the two (L2 step 7). */
    fd = SYS2(2, ustr("/dev/urandom"), 0);
    {
        int32_t cnt = -1;

        expect(fd >= 0 && SYS3(16, (uint64_t)fd, 0x80045200u, b) == 0 &&
               (memcpy(&cnt, kf_uptr(b), sizeof(cnt)), cnt >= 0 && cnt <= 256) &&
               read_whole("/proc/sys/kernel/random/entropy_avail", text, sizeof(text)) > 0 &&
               strtol(text, 0, 10) == cnt,
               "RNDGETENTCNT on /dev/urandom is what /proc/sys/kernel/random/entropy_avail says");
    }
    (void)SYS1(3, (uint64_t)fd);
    fd = SYS2(2, ustr("/dev/zero"), 0);
    memset(kf_uptr(b), 0xAA, 16);
    expect(fd >= 0 && SYS3(0, (uint64_t)fd, b, 16) == 16 && memcmp(kf_uptr(b), "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 16) == 0,
           "/dev/zero reads zeros");
    (void)SYS1(3, (uint64_t)fd);
    fd = SYS2(2, ustr("/dev/full"), 1 /* O_WRONLY */);
    expect(fd >= 0 && SYS3(1, (uint64_t)fd, b, 4) == -VIBEOS_ENOSPC, "/dev/full is always full");
    (void)SYS1(3, (uint64_t)fd);
    fd = SYS2(2, ustr("/dev/urandom"), 0);
    expect(fd >= 0 && SYS3(0, (uint64_t)fd, b, 32) == 32 && SYS3(0, (uint64_t)fd, b2, 32) == 32 &&
           memcmp(kf_uptr(b), kf_uptr(b2), 32) != 0, "/dev/urandom gives different bytes each read");
    (void)SYS1(3, (uint64_t)fd);
    expect(SYS2(2, ustr("/dev/nothing"), 0) == -VIBEOS_ENOENT, "a device /dev does not have is ENOENT");
    expect(dir_type("/dev", "null") == 2 && dir_type("/dev", "fd") == 10, "/dev lists its devices and links");
    expect(link_of("/dev/fd", text) == 13 && strcmp(text, "/proc/self/fd") == 0, "/dev/fd is /proc/self/fd");
    fd = SYS2(2, ustr("/dev/tty"), 2);
    expect(fd >= 0 && SYS2(5, (uint64_t)fd, st) == 0 && (memcpy(&sb, kf_uptr(st), sizeof(sb)), 1) &&
           sb.st_rdev == 0x500u && SYS3(16, (uint64_t)fd, 0x5401 /* TCGETS */, kf_ualloc(64)) == 0,
           "/dev/tty is the terminal: the console, which answers as one");
    (void)SYS1(3, (uint64_t)fd);

    /* getrandom and the pool behind it. */
    expect(SYS3(318, b, 32, 0) == 32 && SYS3(318, b2, 32, 0) == 32 && memcmp(kf_uptr(b), kf_uptr(b2), 32) != 0,
           "getrandom fills the buffer, differently each time");
    expect(SYS3(318, b, 32, 8) == -VIBEOS_EINVAL && SYS3(318, b, 32, 6) == -VIBEOS_EINVAL,
           "an unknown flag, and GRND_INSECURE with GRND_RANDOM, are EINVAL");
    vibeos_random_reset();
    expect(SYS3(318, b, 16, 1 /* GRND_NONBLOCK */) == -VIBEOS_EAGAIN && SYS3(318, b, 16, 4 /* GRND_INSECURE */) == 16,
           "before the pool is ready a caller is told EAGAIN, or given what there is if it asked for that");
    fd = SYS2(2, ustr("/dev/random"), 04000 /* O_NONBLOCK */);
    expect(fd >= 0 && SYS3(0, (uint64_t)fd, b, 8) == -VIBEOS_EAGAIN, "and /dev/random waits for it, /dev/urandom does not");
    (void)SYS1(3, (uint64_t)fd);
    {
        kf_outcome_t out = KF_RETURNED;
        (void)sys(318, b, 16, 0, 0, 0, 0, &out);
        expect(out == KF_BLOCKED, "a getrandom that may wait, waits");
    }
    vibeos_random_add(seed, (uint32_t)sizeof(seed), 256u);
    expect(SYS3(318, b, 16, 1) == 16, "and is answered once the pool is ready");

    /* Read seven bytes at a time, a file is the same file. */
    {
        char whole[2048], pieces[2048];
        long total = 0, got;

        n = read_whole("/proc/self/status", whole, sizeof(whole));
        fd = SYS2(2, ustr("/proc/self/status"), 0);
        while (fd >= 0 && total < (long)sizeof(pieces) - 8 && (got = SYS3(0, (uint64_t)fd, b, 7)) > 0) {
            memcpy(pieces + total, kf_uptr(b), (size_t)got);
            total += got;
        }
        (void)SYS1(3, (uint64_t)fd);
        expect(n > 7 && total == n && memcmp(whole, pieces, (size_t)n) == 0,
               "a /proc file read a few bytes at a time is the file read whole");
    }
}

/* ---- L4 step 1: poll, ppoll, select and pselect6 on one engine --------------------------- */
static void t_event_loops(void) {
    uint64_t up = kf_ualloc(4 * sizeof(linux_pollfd_t)), fdsu = kf_ualloc(8), sets = kf_ualloc(3 * 128);
    uint64_t tv = kf_ualloc(16), mask = kf_ualloc(8), pack = kf_ualloc(16), sa = kf_ualloc(16);
    linux_pollfd_t *p = (linux_pollfd_t *)kf_uptr(up);
    int32_t *f = (int32_t *)kf_uptr(fdsu);
    uint64_t *rs = (uint64_t *)kf_uptr(sets), *ws = rs + 16, *es = rs + 32;
    int64_t *t = (int64_t *)kf_uptr(tv);
    uint64_t *m = (uint64_t *)kf_uptr(mask), *pk = (uint64_t *)kf_uptr(pack);
    kf_outcome_t how = KF_RETURNED;
    long udp, tcp, lis;
    int me;

    me = fresh(81);
    kf_net_up(0x0A00020Fu, 0x0A000202u);
    expect(SYS2(293, fdsu, 0) == 0, "a pipe to wait on");

    /* select: the sets say which, and only of what was asked. */
    memset(rs, 0, 3 * 128);
    rs[0] = 1ull << f[0];
    ws[0] = 1ull << f[1];
    t[0] = 0; t[1] = 0;
    expect(SYS5(23, (uint64_t)f[1] + 1u, sets, sets + 128, sets + 256, tv) == 1 && rs[0] == 0 &&
           ws[0] == (1ull << f[1]) && es[0] == 0,
           "select: an empty pipe can be written and not read, and the sets say so");
    (void)SYS3(1, (uint64_t)f[1], ustr("x"), 1);
    rs[0] = 1ull << f[0];
    ws[0] = 0;
    t[0] = 1; t[1] = 0;
    expect(SYS5(23, (uint64_t)f[1] + 1u, sets, sets + 128, 0, tv) == 1 && rs[0] == (1ull << f[0]) &&
           t[0] * 1000000 + t[1] == 1000000,
           "a byte in it is readable at once, and what was left of the time - all of it - is written back");
    rs[0] = 1ull << 20;
    expect(SYS5(23, 21, sets, 0, 0, tv) == -VIBEOS_EBADF, "a descriptor that is not open is EBADF");
    t[0] = 0; t[1] = 1000000;
    expect(SYS5(23, 1, 0, 0, 0, tv) == -VIBEOS_EINVAL && SYS5(23, (uint64_t)-1, 0, 0, 0, 0) == -VIBEOS_EINVAL,
           "a microsecond count of a second or more, and a negative nfds, are EINVAL");
    (void)SYS3(0, (uint64_t)f[0], kf_ualloc(8), 8);
    rs[0] = 1ull << f[0];
    t[0] = 0; t[1] = 30000;
    {
        uint64_t t0 = ks_ticks();

        expect(sys(23, (uint64_t)f[0] + 1u, sets, 0, 0, tv, 0, &how) == 0 && how == KF_RETURNED && rs[0] == 0 &&
               t[0] == 0 && t[1] == 0,
               "a time that runs out returns 0, the set cleared and the time used up");
        expect(ks_ticks() - t0 >= 3u * ks_hz() / 100u + 1u,
               "and it is never shorter than asked: the tick it began in is not counted, as a sleep's is not");
    }
    {
        int64_t none = MMAP(0, 4096, 0 /* PROT_NONE */, MAP_PRIV_ANON, -1, 0);

        /* A set's size follows from nfds, so the handler judges it - and the
         * machine's copy would read this page, as the fake's does (M-082). */
        t[0] = 0; t[1] = 0;
        expect(none > 0 && SYS5(23, 1, (uint64_t)none, 0, 0, tv) == -VIBEOS_EFAULT,
               "a set in memory the program may not read is EFAULT");
        pk[0] = (uint64_t)none; pk[1] = 8;
        expect(sys(270, 1, 0, 0, 0, tv, pack, 0) == -VIBEOS_EFAULT, "and so is pselect6's mask there");
    }
    rs[0] = 1ull << f[0];
    (void)sys(23, (uint64_t)f[0] + 1u, sets, 0, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "and with no time it waits");

    /* Sockets say what they can do (socket.c, from the stack): a socket used to
     * be always ready, and nc - which waits in poll on its socket - spun. */
    udp = SYS3(41, 2, 2, 0);
    tcp = SYS3(41, 2, 1, 0);
    lis = SYS3(41, 2, 1, 0);
    ((uint8_t *)kf_uptr(sa))[0] = 2;
    ((uint8_t *)kf_uptr(sa))[2] = 0x1F; ((uint8_t *)kf_uptr(sa))[3] = 0x90;   /* 8080 */
    expect(udp >= 0 && tcp >= 0 && lis >= 0 && SYS3(49, (uint64_t)lis, sa, 16) == 0 && SYS2(50, (uint64_t)lis, 4) == 0,
           "three sockets, one of them listening");
    p[0].fd = (int32_t)udp; p[0].events = 1 | 4; p[0].revents = 0;
    p[1].fd = (int32_t)tcp; p[1].events = 1 | 4; p[1].revents = 0;
    p[2].fd = (int32_t)lis; p[2].events = 1; p[2].revents = 0;
    p[3].fd = f[1]; p[3].events = 4; p[3].revents = 0;
    expect(SYS3(7, up, 3, 0) == 2 && p[0].revents == 4 && p[1].revents == (4 | 0x10) && p[2].revents == 0,
           "poll: a datagram socket can send, a stream socket nobody connected is writable and hung up, "
           "and a listener with nobody waiting is nothing");
    (void)SYS1(3, (uint64_t)f[0]);
    expect(SYS3(7, up + 3u * sizeof(linux_pollfd_t), 1, 0) == 1 && p[3].revents == (4 | 0x8),
           "a pipe nobody reads any more is writable - the write fails at once - and an error, not a hangup");

    /* ppoll and pselect6: a mask for the wait, as rt_sigsuspend has. */
    expect(SYS2(293, fdsu, 0) == 0, "another pipe");
    p[0].fd = f[0]; p[0].events = 1; p[0].revents = 0;
    t[0] = 0; t[1] = 0;
    expect(SYS5(271, up, 1, tv, 0, 8) == 0 && t[0] == 0 && t[1] == 0, "ppoll with a zero timespec looks once");
    ks_id(me)->sig_blocked = 1ull << 10;
    (void)ks_signal_send(me, 10, 0);
    *m = 0;   /* nothing blocked during the wait */
    expect(SYS5(271, up, 1, 0, mask, 8) == -VIBEOS_EINTR && ks_id(me)->sig_saved_valid &&
           ks_id(me)->sig_saved == (1ull << 10) && ks_id(me)->sig_blocked == 0,
           "a signal the program blocks and ppoll's mask lets through ends the wait, the program's mask kept "
           "aside for the handler's frame");
    ks_id(me)->sig_blocked = 1ull << 10;
    ks_id(me)->sig_saved_valid = 0;
    *m = 1ull << 9;   /* Linux's numbering: SIGUSR1 is bit 9 */
    t[0] = 0; t[1] = 0;
    expect(SYS5(271, up, 1, tv, mask, 8) == 0 && ks_id(me)->sig_blocked == (1ull << 10) && !ks_id(me)->sig_saved_valid,
           "one ppoll's mask keeps out does not, and the program's mask is back before the call returns");
    expect(SYS5(271, up, 1, tv, mask, 4) == -VIBEOS_EINVAL, "a mask of any size but eight bytes is EINVAL");
    rs[0] = 1ull << f[0];
    *m = 0;
    pk[0] = mask; pk[1] = 8;
    expect(sys(270, (uint64_t)f[0] + 1u, sets, 0, 0, 0, pack, &how) == -VIBEOS_EINTR && how == KF_RETURNED &&
           ks_id(me)->sig_saved_valid,
           "pselect6 takes its mask in a pair, and waits under it");
    ks_id(me)->sig_saved_valid = 0;
    ks_id(me)->sig_pending = 0;
    pk[1] = 4;
    expect(sys(270, (uint64_t)f[0] + 1u, sets, 0, 0, tv, pack, 0) == -VIBEOS_EINVAL, "and refuses a mask of another size");
    pk[0] = 0;
    t[0] = 0; t[1] = 0;
    rs[0] = 1ull << f[0];
    expect(sys(270, (uint64_t)f[0] + 1u, sets, 0, 0, tv, pack, 0) == 0 && rs[0] == 0, "with no mask it is select on a timespec");
    expect(kf_lock_imbalance() == 0, "the event loops released every lock they took");
}

/* ---- L4 steps 2 to 4: eventfd, timerfd, signalfd ------------------------------------ */
static void t_event_fds(void) {
    uint64_t v8 = kf_ualloc(8), its = kf_ualloc(32), old = kf_ualloc(32), mask = kf_ualloc(8);
    uint64_t pf = kf_ualloc(sizeof(linux_pollfd_t)), rec = kf_ualloc(256), ts = kf_ualloc(16);
    uint64_t *v = (uint64_t *)kf_uptr(v8), *m = (uint64_t *)kf_uptr(mask);
    int64_t *it = (int64_t *)kf_uptr(its), *ot = (int64_t *)kf_uptr(old), *t = (int64_t *)kf_uptr(ts);
    linux_pollfd_t *p = (linux_pollfd_t *)kf_uptr(pf);
    linux_signalfd_siginfo_t *r = (linux_signalfd_siginfo_t *)kf_uptr(rec);
    kf_outcome_t how = KF_RETURNED;
    long efd, sem, dup, tfd, tb, sfd;
    uint64_t t0;
    int me = fresh(83);

    /* eventfd: a counter, read whole or one at a time. */
    efd = SYS2(290, 3, 0);
    expect(efd >= 0 && SYS3(0, (uint64_t)efd, v8, 8) == 8 && *v == 3, "eventfd: a read takes the whole count");
    (void)sys(0, (uint64_t)efd, v8, 8, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "and waits at zero");
    sem = SYS2(290, 0, 0x800 | 1 /* EFD_NONBLOCK|EFD_SEMAPHORE */);
    *v = 2;
    expect(sem >= 0 && SYS3(0, (uint64_t)sem, v8, 8) == -VIBEOS_EAGAIN && SYS3(1, (uint64_t)sem, v8, 8) == 8,
           "non-blocking at zero is EAGAIN; a write adds");
    expect(SYS3(0, (uint64_t)sem, v8, 8) == 8 && *v == 1 && SYS3(0, (uint64_t)sem, v8, 8) == 8 && *v == 1 &&
           SYS3(0, (uint64_t)sem, v8, 8) == -VIBEOS_EAGAIN, "a semaphore gives one at a time");
    *v = ~0ull;
    expect(SYS3(1, (uint64_t)sem, v8, 8) == -VIBEOS_EINVAL && SYS3(0, (uint64_t)sem, v8, 4) == -VIBEOS_EINVAL &&
           SYS2(290, 0, 4) == -VIBEOS_EINVAL, "the value past the largest count, a short buffer, an unknown flag: EINVAL");
    *v = 0xfffffffffffffffeull;
    expect(SYS3(1, (uint64_t)sem, v8, 8) == 8, "the largest count fits");
    *v = 1;
    p->fd = (int32_t)sem; p->events = 1 | 4; p->revents = 0;
    expect(SYS3(1, (uint64_t)sem, v8, 8) == -VIBEOS_EAGAIN && SYS3(7, pf, 1, 0) == 1 && p->revents == 1,
           "one more does not: no room, readable and not writable");
    dup = SYS1(32, (uint64_t)efd);
    *v = 5;
    expect(SYS3(1, (uint64_t)efd, v8, 8) == 8 && SYS3(0, (uint64_t)dup, v8, 8) == 8 && *v == 5,
           "a duplicate shares the count: it is the description's");
    expect(SYS2(290, 0, 0x80000 /* EFD_CLOEXEC */) >= 0, "EFD_CLOEXEC");

    /* timerfd: expiries counted by whoever looks. */
    tfd = SYS2(283, 1 /* MONOTONIC */, 0x800 /* TFD_NONBLOCK */);
    expect(tfd >= 0 && SYS3(0, (uint64_t)tfd, v8, 8) == -VIBEOS_EAGAIN, "timerfd: disarmed, nothing to read");
    memset(kf_uptr(its), 0, 32);
    it[2] = 0; it[3] = 30000000;   /* it_value: 30 ms */
    it[0] = 0; it[1] = 10000000;   /* it_interval: 10 ms */
    p->fd = (int32_t)tfd; p->events = 1; p->revents = 0;
    expect(SYS4(286, (uint64_t)tfd, 0, its, 0) == 0 && SYS3(7, pf, 1, 0) == 0,
           "armed for 30 ms: not yet");
    expect(SYS2(287, (uint64_t)tfd, old) == 0 && ot[0] == 0 && ot[1] == 10000000 && ot[2] == 0 &&
           ot[3] > 0 && ot[3] <= 40000000, "gettime: the period, and what is left");
    t[0] = 0; t[1] = 100000000;
    (void)SYS2(35, ts, 0);   /* 100 ms go by */
    expect(SYS3(7, pf, 1, 0) == 1 && SYS3(0, (uint64_t)tfd, v8, 8) == 8 && *v >= 5 && *v <= 9,
           "after 100 ms it has expired once and every 10 ms since, and a read takes the count");
    expect(SYS3(0, (uint64_t)tfd, v8, 8) == -VIBEOS_EAGAIN, "which it then has none of");
    memset(kf_uptr(its), 0, 32);
    expect(SYS4(286, (uint64_t)tfd, 0, its, old) == 0 && ot[1] == 10000000 &&
           SYS2(287, (uint64_t)tfd, old) == 0 && ot[0] == 0 && ot[1] == 0 && ot[2] == 0 && ot[3] == 0,
           "a zero value disarms, and the old setting is handed back");
    it[2] = 0;
    it[3] = 1;   /* a nanosecond after the clock started: long past, and not zero, which disarms */
    expect(SYS4(286, (uint64_t)tfd, 1 /* TFD_TIMER_ABSTIME */, its, 0) == 0 && SYS3(0, (uint64_t)tfd, v8, 8) == 8 && *v == 1,
           "an absolute time already past has expired once");
    tb = SYS2(283, 1, 0);
    memset(kf_uptr(its), 0, 32);
    it[3] = 20000000;
    t0 = ks_ticks();
    expect(tb >= 0 && SYS4(286, (uint64_t)tb, 0, its, 0) == 0 && SYS3(0, (uint64_t)tb, v8, 8) == 8 && *v == 1 &&
           ks_ticks() - t0 >= 2u * ks_hz() / 100u + 1u,
           "a blocking read waits for the expiry, and never less than asked");
    it[3] = 1000000000;
    expect(SYS2(283, 99, 0) == -VIBEOS_EINVAL && SYS2(283, 2 /* PROCESS_CPUTIME */, 0) == -VIBEOS_EINVAL &&
           SYS2(283, 1, 4) == -VIBEOS_EINVAL && SYS4(286, (uint64_t)tfd, 0, its, 0) == -VIBEOS_EINVAL &&
           SYS4(286, (uint64_t)efd, 0, its, 0) == -VIBEOS_EINVAL && SYS2(287, 99, old) == -VIBEOS_EBADF,
           "an unknown or CPU-time clock, an unknown flag, a bad time, a descriptor that is not a timer, none at all");
    expect(SYS2(287, 99, 0x10) == -VIBEOS_EBADF && SYS2(287, (uint64_t)tfd, 0x10) == -VIBEOS_EFAULT,
           "gettime looks at the descriptor before the pointer (LTP's timerfd_gettime01)");

    /* signalfd: pending signals read as records. */
    ks_id(me)->sig_blocked = (1ull << 10) | (1ull << 12);
    *m = (1ull << 9) | (1ull << 11);   /* Linux's numbering: SIGUSR1, SIGUSR2 */
    sfd = sys(289, (uint64_t)-1, mask, 8, 0x800 /* SFD_NONBLOCK */, 0, 0, 0);
    p->fd = (int32_t)sfd; p->events = 1; p->revents = 0;
    expect(sfd >= 0 && SYS3(0, (uint64_t)sfd, rec, 128) == -VIBEOS_EAGAIN && SYS3(7, pf, 1, 0) == 0,
           "signalfd: nothing pending, nothing to read");
    {
        vibeos_siginfo_t why;

        memset(&why, 0, sizeof(why));
        why.from = VIBEOS_SIG_FROM_PROCESS;
        why.pid = 77;
        why.uid = 5;
        (void)ks_signal_send(me, 12, &why);
        (void)ks_signal_send(me, 10, &why);
    }
    expect(SYS3(7, pf, 1, 0) == 1 && SYS3(0, (uint64_t)sfd, rec, 256) == 256 && r[0].ssi_signo == 10 &&
           r[0].ssi_pid == 77 && r[0].ssi_uid == 5 && r[0].ssi_code == 0 && r[1].ssi_signo == 12 &&
           (ks_id(me)->sig_pending & ((1ull << 10) | (1ull << 12))) == 0,
           "two pending are two records, lowest first, with who sent them - taken, not delivered");
    expect(SYS3(0, (uint64_t)sfd, rec, 64) == -VIBEOS_EINVAL && sys(289, (uint64_t)-1, mask, 4, 0, 0, 0, 0) == -VIBEOS_EINVAL &&
           sys(289, (uint64_t)efd, mask, 8, 0, 0, 0, 0) == -VIBEOS_EINVAL,
           "a buffer shorter than a record, a mask of another size, a descriptor that is not a signalfd: EINVAL");
    *m = 1ull << 9;
    expect(sys(289, (uint64_t)sfd, mask, 8, 0, 0, 0, 0) == sfd, "a second call on it changes its mask");
    (void)ks_signal_send(me, 12, 0);
    expect(SYS3(0, (uint64_t)sfd, rec, 128) == -VIBEOS_EAGAIN, "and SIGUSR2 is not in it any more");
    ks_id(me)->sig_pending = 0;
    expect(kf_lock_imbalance() == 0, "the event descriptors released every lock they took");
}

/* clone3's argument rules (L2 step 6, from step 5's LTP run: clone302). */
static void t_clone3_checks(void) {
    uint64_t a, big;
    linux_clone_args_t *ca;

    (void)fresh(190);
    a = kf_ualloc(sizeof(linux_clone_args_t));
    ca = (linux_clone_args_t *)kf_uptr(a);
    memset(ca, 0, sizeof(*ca));
    ca->exit_signal = 17;
    ca->flags = LINUX_CLONE_SIGHAND;
    expect(SYS2(435, a, sizeof(*ca)) == -VIBEOS_EINVAL, "handlers shared without the memory they point into are EINVAL");
    ca->flags = LINUX_CLONE_FS | LINUX_CLONE_NEWNS;
    expect(SYS2(435, a, sizeof(*ca)) == -VIBEOS_EINVAL, "a filesystem view both shared and new is EINVAL");
    ca->flags = LINUX_CLONE_PIDFD | LINUX_CLONE_PARENT_SETTID;
    ca->pidfd = ca->parent_tid = kf_ualloc(8);
    expect(SYS2(435, a, sizeof(*ca)) == -VIBEOS_EINVAL, "a pidfd and a tid written to one place are EINVAL");
    ca->flags = LINUX_CLONE_PIDFD;
    /* Unmapped, inside the fake's address-space window: outside it the fake
     * takes an address for a host pointer, so without the check the late
     * write crashed the suite instead of failing this line, where the
     * machine's copy refuses it. */
    ca->pidfd = 0x20000000ull;
    ca->parent_tid = 0;
    expect(SYS2(435, a, sizeof(*ca)) == -VIBEOS_EFAULT,
           "a pidfd to be written outside user memory is EFAULT, before there is a child (L2 step 7)");
    ca->flags = 0;
    ca->pidfd = ca->parent_tid = 0;
    expect(SYS2(435, a, 4097) == -VIBEOS_E2BIG, "a structure larger than a page is E2BIG");
    big = kf_ualloc(sizeof(*ca) + 8u);
    memset(kf_uptr(big), 0, sizeof(*ca) + 8u);
    memcpy(kf_uptr(big), ca, sizeof(*ca));
    ((uint8_t *)kf_uptr(big))[sizeof(*ca) + 3u] = 1;
    expect(SYS2(435, big, sizeof(*ca) + 8u) == -VIBEOS_E2BIG,
           "a newer structure is E2BIG if what this kernel does not know is not zero");
    expect(SYS1(56, 65 /* CSIGNAL past the last signal */) == -VIBEOS_EINVAL,
           "clone's exit signal must be a signal");
}

/* A futex on a page mapped MAP_SHARED is the same word in every process that
 * maps it, wherever each maps it (L2 step 6): LTP's checkpoints. */
static void t_futex_shared(void) {
    uint64_t wa, wb, ts;
    kf_outcome_t out = KF_RETURNED;
    int a, b;

    a = fresh(180);
    b = kf_spawn(181, 181);
    wa = (kf_ualloc(8192) + 4095u) & ~4095ull;
    wb = (kf_ualloc(8192) + 4095u) & ~4095ull;
    kf_share_page(a, wa, 77u);
    kf_share_page(b, wb, 77u);
    *(uint32_t *)kf_uptr(wa + 8u) = 5u;
    *(uint32_t *)kf_uptr(wb + 8u) = 5u;
    kf_set_current(a);
    (void)sys(202, wa + 8u, 0 /* FUTEX_WAIT */, 5, 0, 0, 0, &out);
    expect(out == KF_BLOCKED, "a waits on the shared word");
    kf_set_current(b);
    expect(SYS3(202, wb + 8u, 1 /* FUTEX_WAKE */, 1) == 1,
           "and b, waking the same word at its own address, wakes it");
    kf_set_current(a);
    out = KF_RETURNED;
    (void)sys(202, wa + 8u, 128 /* FUTEX_WAIT|PRIVATE */, 5, 0, 0, 0, &out);
    kf_set_current(b);
    expect(SYS3(202, wb + 8u, 1 | 128, 1) == 0, "a private wait is its own process's, whatever the page");
    /* A timed wait ends. */
    kf_set_current(a);
    ts = kf_ualloc(16);
    ((int64_t *)kf_uptr(ts))[0] = 0;
    ((int64_t *)kf_uptr(ts))[1] = 50000000;   /* 50 ms */
    expect(sys(202, wa + 8u, 128, 5, ts, 0, 0, 0) == -VIBEOS_ETIMEDOUT, "a wait with a timeout nobody ends is ETIMEDOUT");
    ((int64_t *)kf_uptr(ts))[1] = 1000000000;
    expect(sys(202, wa + 8u, 128, 5, ts, 0, 0, 0) == -VIBEOS_EINVAL, "and a timeout that is not one is EINVAL");
    kf_share_page(-1, 0, 0);
}

/* ---- what LTP's L1 tests found (L3 step 3) --------------------------------------------- */

static void t_ltp_l1(void) {
    uint64_t buf, iov, stx, fds;
    uint64_t *v;
    linux_flock_t fl;
    long fd, dfd, none, i, last = 0;
    int parent, child;
    long pid;

    /* After fresh(): it gives the user arena back, and an address taken before
     * it is the next allocation's. */
    parent = fresh(191);
    buf = kf_ualloc(64);
    iov = kf_ualloc(32);
    stx = kf_ualloc(256);
    fds = kf_ualloc(8);
    v = (uint64_t *)kf_uptr(iov);

    /* M-082 (statx03): a path that is not the program's to read is EFAULT,
     * whatever its first byte is. A PROT_NONE page holds zeros. */
    none = MMAP(0, 4096, 0 /* PROT_NONE */, MAP_PRIV_ANON, -1, 0);
    expect(none > 0 && sys(332, (uint64_t)(uint32_t)-100, (uint64_t)none, 0, 0, stx, 0, 0) == -VIBEOS_EFAULT,
           "statx of a path in memory the program may not read is EFAULT, not ENOENT");
    expect(sys(262, (uint64_t)(uint32_t)-100, (uint64_t)none, stx, 0, 0, 0, 0) == -VIBEOS_EFAULT &&
           sys(262, (uint64_t)(uint32_t)-100, (uint64_t)none, stx, 0x1000 /* AT_EMPTY_PATH */, 0, 0, 0) == -VIBEOS_EFAULT,
           "nor is newfstatat's, with AT_EMPTY_PATH or without");
    expect(sys(268, (uint64_t)(uint32_t)-100, (uint64_t)none, 0600, 0, 0, 0, 0) == -VIBEOS_EFAULT,
           "nor fchmodat's");
    expect(sys(260, (uint64_t)(uint32_t)-100, (uint64_t)none, (uint64_t)(uint32_t)-1, (uint64_t)(uint32_t)-1,
               0x1000 /* AT_EMPTY_PATH */, 0, 0) == -VIBEOS_EFAULT,
           "nor fchownat's under AT_EMPTY_PATH, where an unreadable zero would have named the directory itself");

    /* fchmodat02: an empty path is ENOENT before the directory is looked at. */
    fd = tmp_open("/tmp/l1", 0x42, 0644);
    expect(sys(268, (uint64_t)fd, ustr(""), 0600, 0, 0, 0, 0) == -VIBEOS_ENOENT &&
           sys(268, 99, ustr(""), 0600, 0, 0, 0, 0) == -VIBEOS_ENOENT &&
           sys(268, (uint64_t)fd, ustr("x"), 0600, 0, 0, 0, 0) == -VIBEOS_ENOTDIR,
           "an empty path is ENOENT whatever the directory descriptor is");

    /* preadv02: a length that is not one, and a directory. */
    v[0] = buf;
    v[1] = (uint64_t)-1;
    expect(SYS4(295, (uint64_t)fd, iov, 1, 0) == -VIBEOS_EINVAL && SYS4(296, (uint64_t)fd, iov, 1, 0) == -VIBEOS_EINVAL,
           "preadv and pwritev with a negative length are EINVAL, not EFAULT");
    v[1] = 8;
    dfd = SYS2(2, ustr("/tmp"), 0x10000);
    expect(SYS4(295, (uint64_t)dfd, iov, 1, 0) == -VIBEOS_EISDIR && SYS4(17, (uint64_t)dfd, buf, 8, 0) == -VIBEOS_EISDIR,
           "reading a directory at an offset is EISDIR");
    (void)SYS2(293, fds, 0);
    expect(SYS4(17, (uint64_t)((int32_t *)kf_uptr(fds))[0], buf, 8, 0) == -VIBEOS_ESPIPE, "and a pipe is still ESPIPE");

    /* fallocate02: past the largest offset. */
    expect(SYS4(285, (uint64_t)fd, 1 /* KEEP_SIZE */, 0x7ffffffffffffc00ull, 1024) == -VIBEOS_EFBIG &&
           SYS4(285, (uint64_t)fd, 1, 1024, 0x7ffffffffffffc00ull) == -VIBEOS_EFBIG &&
           SYS4(285, (uint64_t)fd, 1, 0, 4096) == 0,
           "fallocate past the largest offset is EFBIG even when it keeps the size");

    /* A socket of a family there is none of. */
    expect(SYS3(41, 1 /* AF_UNIX */, 1, 0) == -VIBEOS_EAFNOSUPPORT, "a local socket is EAFNOSUPPORT");

    /* fcntl11: F_GETLK names the first lock in the way, by position. */
    expect(lock_op(fd, 6 /* F_SETLK */, 1 /* F_WRLCK */, 10, 5, 0) == 0 &&
           lock_op(fd, 6, 0 /* F_RDLCK */, 1, 5, 0) == 0, "a write lock at 10, then a read lock at 1");
    pid = SYS0(57);
    child = slot_of_pid(pid);
    kf_set_current(child);
    expect(lock_op(fd, 5 /* F_GETLK */, 1, 0, 0, &fl) == 0 && fl.l_type == 0 /* F_RDLCK */ &&
           fl.l_start == 1 && fl.l_len == 5 && fl.l_pid == 191,
           "another process asking about the whole file is told of the one that starts first");
    kf_set_current(parent);

    /* creat05, fcntl12: a process runs into its own limit, not the machine's. */
    for (i = 0; i < 1100; i++) {
        last = SYS2(2, ustr("/tmp/l1"), 0);
        if (last < 0) {
            break;
        }
    }
    expect(last == -VIBEOS_EMFILE && i > 1000, "opening until refused ends at EMFILE, a thousand files on");
    expect(SYS3(72, 1, 0 /* F_DUPFD */, 1) == -VIBEOS_EMFILE, "and a duplicate is refused the same way");
}

/* ---- sleeping (L3 step 3) ------------------------------------------------------------ */

static void t_sleep(void) {
    uint64_t req = kf_ualloc(16), rem = kf_ualloc(16);
    int64_t *q = (int64_t *)kf_uptr(req), *m = (int64_t *)kf_uptr(rem);
    uint64_t t0;
    int me = fresh(181);
    kf_outcome_t how;

    /* The fake's clock is 100 ticks a second and moves one tick a wait. */
    q[0] = 0;
    q[1] = 250000000;                 /* a quarter of a second: 25 ticks */
    t0 = ks_ticks();
    expect(SYS2(35, req, 0) == 0 && ks_ticks() - t0 >= 26u && ks_ticks() - t0 <= 27u,
           "nanosleep waits the time asked for, and the rest of the tick it began in");
    q[1] = 1;                         /* a nanosecond is still a wait */
    t0 = ks_ticks();
    expect(SYS2(35, req, 0) == 0 && ks_ticks() - t0 >= 2u, "a time shorter than a tick is rounded up, not away");
    q[1] = 0;
    t0 = ks_ticks();
    expect(SYS2(35, req, 0) == 0 && ks_ticks() == t0, "a sleep of nothing returns at once");
    q[1] = 1000000000;
    expect(SYS2(35, req, 0) == -VIBEOS_EINVAL, "a billion nanoseconds is not a timespec");
    q[0] = -1;
    q[1] = 0;
    expect(SYS2(35, req, 0) == -VIBEOS_EINVAL && SYS2(35, 0, 0) == -VIBEOS_EFAULT,
           "nor is a negative second; no request at all is EFAULT");

    /* A signal ends it, and says how much was left. */
    q[0] = 5;
    q[1] = 0;
    m[0] = m[1] = -1;
    (void)ks_signal_raise(me, 10);
    expect(SYS2(35, req, rem) == -VIBEOS_EINTR && ks_id(me)->sys_restart == 0u,
           "a signal ends a sleep with EINTR, and a sleep is not run again");
    expect(m[0] == 5 && m[1] >= 0 && m[1] < 1000000000ll, "and what was left of it is reported");
    ks_id(me)->sig_pending = 0;

    /* clock_nanosleep: until a time, not for one. */
    q[0] = (int64_t)(ks_ticks() / 100u) + 1;
    q[1] = 0;
    expect(sys(230, 1 /* MONOTONIC */, 1 /* TIMER_ABSTIME */, req, 0, 0, 0, 0) == 0 &&
           ks_ticks() >= (uint64_t)q[0] * 100u && ks_ticks() < (uint64_t)q[0] * 100u + 2u,
           "clock_nanosleep with TIMER_ABSTIME sleeps until the clock reads the time");
    t0 = ks_ticks();
    expect(sys(230, 1, 1, req, 0, 0, 0, 0) == 0 && ks_ticks() == t0, "a time already past is no wait");
    q[0] = 0;
    q[1] = 20000000;
    t0 = ks_ticks();
    expect(sys(230, 0 /* REALTIME */, 0, req, 0, 0, 0, 0) == 0 && ks_ticks() - t0 >= 3u,
           "without the flag it is nanosleep on the clock named");
    t0 = ks_ticks();
    expect(sys(230, 11 /* TAI */, 0, req, 0, 0, 0, 0) == 0 && ks_ticks() - t0 >= 3u,
           "and TAI is slept on like the others (L2 step 7)");
    expect(sys(230, 3 /* THREAD_CPUTIME_ID */, 0, req, 0, 0, 0, 0) == -VIBEOS_EOPNOTSUPP &&
           sys(230, 6 /* MONOTONIC_COARSE */, 0, req, 0, 0, 0, 0) == -VIBEOS_EOPNOTSUPP &&
           sys(230, 4 /* MONOTONIC_RAW */, 0, req, 0, 0, 0, 0) == -VIBEOS_EOPNOTSUPP,
           "a thread's CPU time and the raw and coarse clocks have no sleep: EOPNOTSUPP, as Linux");
    expect(sys(230, 1, 2, req, 0, 0, 0, 0) == -VIBEOS_EINVAL && sys(230, 99, 0, req, 0, 0, 0, 0) == -VIBEOS_EINVAL,
           "an unknown flag and an unknown clock are EINVAL");
    q[0] = 0;
    q[1] = 0;
    expect(sys(230, 2 /* PROCESS_CPUTIME_ID */, 1, req, 0, 0, 0, 0) == 0,
           "a CPU time the process has already had is no wait");
    q[1] = 20000000;
    {
        kf_outcome_t cpu_how = KF_RETURNED;

        (void)sys(230, 2, 0, req, 0, 0, 0, &cpu_how);
        expect(cpu_how == KF_BLOCKED,
               "a sleep on the process's own CPU time waits for CPU time, which waiting does not give it");
    }

    /* A sleep longer than anything will wait is a wait, not a wrap to zero. */
    q[0] = 0x7fffffffffffffffll;
    (void)sys(35, req, 0, 0, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "the longest sleep there is does not return");
}

/* ---- SA_RESTART (L3 step 3) ----------------------------------------------------------- */

/* The register a handler will return to, read out of Linux's frame as a
 * program would: the ucontext is eight bytes above where the handler's stack
 * starts (L2). 0 is the saved rip, 2 the saved rax. */
static uint64_t saved_reg(const struct ks_regs *fr, uint32_t which) {
    const linux_ucontext_t *uc = (const linux_ucontext_t *)kf_uptr(fr->sp + 8u);
    return which == 0u ? uc->uc_mcontext.rip : uc->uc_mcontext.rax;
}

static void t_sa_restart(void) {
    uint64_t act = kf_ualloc(32), stack = kf_ualloc(8192), handler = kf_ualloc(16), restorer = kf_ualloc(16);
    uint64_t *a = (uint64_t *)kf_uptr(act);
    struct ks_regs fr;
    int me = fresh(171);
    long pid;

    pid = SYS0(57);
    kf_set_current(me);
    a[0] = handler;
    a[1] = 0x04000000u | 0x10000000u;    /* SA_RESTORER | SA_RESTART */
    a[2] = restorer;
    a[3] = 0;
    expect(pid > 0 && SYS4(13, 10 /* SIGUSR1 */, act, 0, 8) == 0, "a handler installed with SA_RESTART");

    /* The wait is cut short: the program's answer is EINTR until delivery says
     * otherwise, and the call is remembered. */
    (void)ks_signal_raise(me, 10);
    expect(SYS3(61, (uint64_t)-1, 0, 0) == -VIBEOS_EINTR && ks_id(me)->sys_restart == 62u,
           "waitpid cut short by a signal is EINTR, and remembered as restartable");
    memset(&fr, 0, sizeof(fr));
    fr.ip = 0x1002;
    fr.sp = stack + 8000u;
    fr.ret = (uint64_t)-VIBEOS_EINTR;
    expect(linux_signal_deliver(&fr) == 1 && fr.ip == handler && ks_id(me)->sys_restart == 0u,
           "the handler is entered");
    expect(saved_reg(&fr, 0) == 0x1000 && saved_reg(&fr, 2) == 61u,
           "and returns to the call itself: SA_RESTART runs waitpid again");

    /* Without the flag the program gets its EINTR. */
    ks_id(me)->sig_blocked = 0;
    a[1] = 0x04000000u;
    (void)SYS4(13, 10, act, 0, 8);
    (void)ks_signal_raise(me, 10);
    expect(SYS3(61, (uint64_t)-1, 0, 0) == -VIBEOS_EINTR, "interrupted again");
    memset(&fr, 0, sizeof(fr));
    fr.ip = 0x1002;
    fr.sp = stack + 8000u;
    fr.ret = (uint64_t)-VIBEOS_EINTR;
    expect(linux_signal_deliver(&fr) == 1 && saved_reg(&fr, 0) == 0x1002 &&
           saved_reg(&fr, 2) == (uint64_t)-VIBEOS_EINTR,
           "without SA_RESTART the handler returns after the call, to EINTR");

    /* A signal nobody handles interrupts nothing the program can see. */
    ks_id(me)->sig_blocked = 0;
    (void)ks_signal_raise(me, 17 /* SIGCHLD, default: discarded */);
    expect(SYS3(61, (uint64_t)-1, 0, 0) == -VIBEOS_EINTR, "interrupted by a signal that will be discarded");
    memset(&fr, 0, sizeof(fr));
    fr.ip = 0x1002;
    fr.sp = stack + 8000u;
    fr.ret = (uint64_t)-VIBEOS_EINTR;
    expect(linux_signal_deliver(&fr) == 0 && fr.ip == 0x1000 && fr.ret == 61u,
           "with no handler to run, the call is simply issued again");

    /* And a call nothing interrupted is left alone. */
    memset(&fr, 0, sizeof(fr));
    fr.ip = 0x1002;
    fr.ret = 7;
    expect(SYS0(39) > 0 && ks_id(me)->sys_restart == 0u && linux_signal_deliver(&fr) == 0 &&
           fr.ip == 0x1002 && fr.ret == 7u, "a call that was not interrupted is not restarted");
}

/* ---- signals completed (docs/abi/ L2 step 2) ------------------------------------------ */

#define T_SA_RESTORER 0x04000000u
#define T_SA_SIGINFO  0x00000004u
#define T_SA_ONSTACK  0x08000000u
#define T_SA_NODEFER  0x40000000u
#define T_SA_RESETHAND 0x80000000u

/* Install a handler for `sig` with `flags` (plus SA_RESTORER) through
 * rt_sigaction, as a C library does. */
static void t_handler(uint32_t sig, uint64_t handler, uint64_t flags, uint64_t mask) {
    uint64_t act = kf_ualloc(32);
    uint64_t *a = (uint64_t *)kf_uptr(act);

    a[0] = handler;
    a[1] = flags | T_SA_RESTORER;
    a[2] = 0x3000;          /* the trampoline; never run here */
    a[3] = mask;
    expect(SYS4(13, sig, act, 0, 8) == 0, "a handler is installed");
}

/* Deliver on a frame that was at `ip` with stack `sp`; the handler's frame. */
static int t_deliver(struct ks_regs *fr, uint64_t ip, uint64_t sp) {
    memset(fr, 0, sizeof(*fr));
    fr->ip = ip;
    fr->sp = sp;
    fr->ret = 0x77;
    fr->other[9] = 0xB0B0;   /* rbx: a register nobody but the frame keeps */
    return linux_signal_deliver(fr);
}

static void t_signals_l2(void) {
    uint64_t stack, handler, alt, ss, old, set, info, ts, qi;
    linux_stack_t *ssp, *oldp;
    uint64_t *setp;
    linux_siginfo_t *ip, *qp;
    int64_t *tsp;
    struct ks_regs fr;
    const linux_ucontext_t *uc;
    const linux_siginfo_t *si;
    kf_outcome_t how;
    int me, other;
    uint32_t i;

    /* ---- the frame: what a handler with SA_SIGINFO is handed -------------- */
    me = fresh(301);
    handler = kf_ualloc(16);
    stack = kf_ualloc(16384);
    other = kf_spawn(302, 301);
    t_handler(10, handler, T_SA_SIGINFO, 0);
    ks_id(me)->sig_blocked = 1ull << 12;            /* the program's own mask */
    for (i = 0; i < 512u; i++) {
        g_kf_fpu[i] = (unsigned char)(i * 7u);
    }
    g_kf_fpu[26] = g_kf_fpu[27] = 0;
    kf_set_current(other);
    expect(SYS2(62, 301, 10) == 0, "another process of the session sends SIGUSR1");
    kf_set_current(me);
    expect(t_deliver(&fr, 0x1234, stack + 12000u) == 1 && fr.ip == handler,
           "the handler is entered");
    uc = (const linux_ucontext_t *)kf_uptr(fr.sp + 8u);
    si = (const linux_siginfo_t *)kf_uptr(fr.sp + 8u + sizeof(linux_ucontext_t));
    expect(fr.arg0 == 10u && fr.arg1 == fr.sp + 8u + sizeof(linux_ucontext_t) && fr.arg2 == fr.sp + 8u,
           "its arguments are the signal, the siginfo and the ucontext");
    expect((fr.sp & 15u) == 8u && fr.sp + 512u < stack + 12000u - 128u,
           "entered as if called, below the red zone");
    expect(*(const uint64_t *)kf_uptr(fr.sp) == 0x3000, "returning to the C library's trampoline");
    expect(si->si_signo == 10 && si->si_code == LINUX_SI_USER && si->pid == 302,
           "the siginfo says kill sent it, and who");
    expect(uc->uc_mcontext.rip == 0x1234 && uc->uc_mcontext.rax == 0x77 &&
           uc->uc_mcontext.rbx == 0xB0B0 && uc->uc_mcontext.rsp == stack + 12000u,
           "the ucontext holds the interrupted registers by Linux's names");
    expect(uc->uc_sigmask == (1ull << 11) && uc->uc_stack.ss_flags == LINUX_SS_DISABLE,
           "and the mask to return to - Linux's numbering - and no alternate stack");
    expect(uc->uc_mcontext.fpstate != 0u && (uc->uc_mcontext.fpstate & 63u) == 0u &&
           memcmp(kf_uptr(uc->uc_mcontext.fpstate), g_kf_fpu, 512) == 0,
           "and the vector registers, 64-aligned, where fpstate says");
    expect(ks_id(me)->sig_blocked == ((1ull << 12) | (1ull << 10)),
           "the signal itself is blocked while its handler runs");

    /* ---- rt_sigreturn reads the frame as the program left it -------------- */
    {
        linux_ucontext_t *w = (linux_ucontext_t *)kf_uptr(fr.sp + 8u);
        uint64_t base = fr.sp + 8u;

        w->uc_mcontext.rip = 0x1300;       /* what a recovering handler does */
        w->uc_mcontext.rax = 0x55;
        w->uc_sigmask = 1ull << 1;         /* SIGINT, in Linux's numbering */
        ((unsigned char *)kf_uptr(w->uc_mcontext.fpstate))[100] = 0xEE;
        kf_next_sp(base);
        expect(sys(15, 0, 0, 0, 0, 0, 0, &how) == 0x55 && how == 0 &&
               kf_last_frame()->ip == 0x1300 && kf_last_frame()->other[9] == 0xB0B0,
               "rt_sigreturn resumes where the ucontext now says, with its registers");
        expect(ks_id(me)->sig_blocked == (1ull << 2) && g_kf_fpu[100] == 0xEE,
               "and puts back its mask and its vector registers");
        w->uc_mcontext.rip = 0xdead000000000000ull;
        kf_next_sp(base);
        (void)sys(15, 0, 0, 0, 0, 0, 0, &how);
        expect(how == KF_EXITED && kf_exit_code() == 128u + 11u,
               "a resume address outside user memory ends the task with SIGSEGV (H-018)");
    }
    {
        uint64_t u;
        linux_ucontext_t *w;

        me = fresh(303);
        u = kf_ualloc(sizeof(linux_ucontext_t) + 64u);
        w = (linux_ucontext_t *)kf_uptr(u);
        w->uc_mcontext.rip = 0x1300;
        w->uc_mcontext.fpstate = u + 4u;
        kf_next_sp(u);
        (void)sys(15, 0, 0, 0, 0, 0, 0, &how);
        expect(how == KF_EXITED && kf_exit_code() == 128u + 11u, "so does a misaligned vector area");
        w->uc_mcontext.fpstate = 0;
        kf_next_sp(u);
        (void)sys(15, 0, 0, 0, 0, 0, 0, &how);
        expect(how == 0 && kf_last_frame()->ip == 0x1300, "and none at all is allowed, as on Linux");
    }

    /* ---- sigaltstack ------------------------------------------------------ */
    me = fresh(311);
    handler = kf_ualloc(16);
    stack = kf_ualloc(16384);
    alt = kf_ualloc(8192);
    ss = kf_ualloc(24); old = kf_ualloc(24);
    ssp = (linux_stack_t *)kf_uptr(ss);
    oldp = (linux_stack_t *)kf_uptr(old);
    expect(SYS2(131, 0, old) == 0 && oldp->ss_flags == LINUX_SS_DISABLE && oldp->ss_size == 0u,
           "a thread starts with no alternate stack");
    ssp->ss_sp = alt;
    ssp->ss_size = 1024;
    ssp->ss_flags = 0;
    expect(SYS2(131, ss, 0) == -VIBEOS_ENOMEM, "one smaller than MINSIGSTKSZ is refused");
    ssp->ss_size = 8192;
    ssp->ss_flags = 5;
    expect(SYS2(131, ss, 0) == -VIBEOS_EINVAL, "and flags that are not a mode");
    ssp->ss_flags = 0;
    expect(SYS2(131, ss, old) == 0 && SYS2(131, 0, old) == 0 &&
           oldp->ss_sp == alt && oldp->ss_size == 8192u && oldp->ss_flags == 0,
           "one set is reported back");
    t_handler(12, handler, T_SA_SIGINFO | T_SA_ONSTACK, 0);
    t_handler(10, handler, T_SA_SIGINFO, 0);
    (void)ks_signal_raise(me, 12);
    expect(t_deliver(&fr, 0x1234, stack + 12000u) == 1 && fr.sp > alt && fr.sp < alt + 8192u,
           "SA_ONSTACK delivers on it");
    uc = (const linux_ucontext_t *)kf_uptr(fr.sp + 8u);
    expect(uc->uc_stack.ss_sp == alt && uc->uc_stack.ss_flags == 0 && uc->uc_mcontext.rsp == stack + 12000u,
           "and the frame records the stack the program was on");
    kf_next_sp(fr.sp + 256u);
    expect(SYS2(131, ss, 0) == -VIBEOS_EPERM, "it cannot be changed from on it");
    ks_id(me)->sig_blocked = 0;
    (void)ks_signal_raise(me, 10);
    expect(t_deliver(&fr, 0x1234, stack + 12000u) == 1 && fr.sp < stack + 12000u && fr.sp > stack,
           "a handler without SA_ONSTACK runs on the program's own stack");
    ks_id(me)->sig_blocked = 0;
    {
        uint64_t inside = fr.sp;
        (void)ks_signal_raise(me, 12);
        expect(t_deliver(&fr, 0x1234, alt + 4000u) == 1 && fr.sp < alt + 4000u - 128u && fr.sp > alt,
               "already on it, the next frame goes below the one in use, not at its top");
        (void)inside;
    }
    ssp->ss_flags = (int32_t)LINUX_SS_AUTODISARM;
    ks_id(me)->sig_blocked = 0;
    expect(SYS2(131, ss, 0) == 0, "SS_AUTODISARM is accepted");
    (void)ks_signal_raise(me, 12);
    expect(t_deliver(&fr, 0x1234, stack + 12000u) == 1 && ks_id(me)->sas_size == 0u,
           "and disarms the stack while a handler is on it");
    kf_next_sp(fr.sp + 8u);
    (void)sys(15, 0, 0, 0, 0, 0, 0, &how);
    expect(how == 0 && ks_id(me)->sas_size == 8192u && ks_id(me)->sas_sp == alt,
           "rt_sigreturn arms it again from the frame");

    /* ---- SA_NODEFER, SA_RESETHAND ----------------------------------------- */
    me = fresh(321);
    handler = kf_ualloc(16);
    stack = kf_ualloc(16384);
    t_handler(10, handler, T_SA_NODEFER | T_SA_RESETHAND | T_SA_SIGINFO, 0);
    (void)ks_signal_raise(me, 10);
    expect(t_deliver(&fr, 0x1234, stack + 12000u) == 1 &&
           (ks_id(me)->sig_blocked & (1ull << 10)) == 0u && ks_ps(me)->sig_handler[10] == SIG_DFL_ADDR,
           "SA_NODEFER leaves the signal unblocked, SA_RESETHAND delivers once");
    expect((ks_ps(me)->sig_flags[10] & T_SA_SIGINFO) != 0u,
           "and SA_RESETHAND leaves SA_SIGINFO as it was (LTP's sigaction01)");

    /* ---- rt_sigpending, rt_sigsuspend, pause ------------------------------ */
    me = fresh(331);
    handler = kf_ualloc(16);
    stack = kf_ualloc(16384);
    set = kf_ualloc(8);
    setp = (uint64_t *)kf_uptr(set);
    t_handler(10, handler, 0, 0);
    ks_id(me)->sig_blocked = 1ull << 10;
    (void)ks_signal_raise(me, 10);
    (void)ks_signal_raise(me, 12);   /* pending and not blocked: not reported */
    expect(SYS2(127, set, 8) == 0 && *setp == (1ull << 9),
           "rt_sigpending reports a blocked one, Linux-numbered, and only the blocked ones");
    expect(SYS2(127, set, 4) == -VIBEOS_EINVAL, "for a sigset of eight bytes only");
    ks_id(me)->sig_pending &= ~(1ull << 12);
    *setp = 0;
    expect(SYS2(130, set, 8) == -VIBEOS_EINTR && ks_id(me)->sig_saved_valid &&
           ks_id(me)->sig_saved == (1ull << 10) && ks_id(me)->sig_blocked == 0u,
           "rt_sigsuspend waits under its mask, which lets the pending one in");
    expect(t_deliver(&fr, 0x1234, stack + 12000u) == 1 &&
           ((const linux_ucontext_t *)kf_uptr(fr.sp + 8u))->uc_sigmask == (1ull << 9) &&
           !ks_id(me)->sig_saved_valid,
           "and the handler returns to the program's own mask, not the temporary one");
    ks_id(me)->sig_blocked = 0;
    (void)sys(34, 0, 0, 0, 0, 0, 0, &how);
    expect(how == KF_BLOCKED, "pause waits while nothing is pending");
    (void)ks_signal_raise(me, 10);
    expect(SYS0(34) == -VIBEOS_EINTR, "and ends with EINTR when a signal needs handling");
    ks_id(me)->sig_pending = 0;

    /* ---- rt_sigtimedwait --------------------------------------------------- */
    me = fresh(341);
    handler = kf_ualloc(16);
    stack = kf_ualloc(16384);
    other = kf_spawn(342, 341);
    set = kf_ualloc(8); info = kf_ualloc(128); ts = kf_ualloc(16);
    setp = (uint64_t *)kf_uptr(set);
    ip = (linux_siginfo_t *)kf_uptr(info);
    tsp = (int64_t *)kf_uptr(ts);
    ks_id(me)->sig_blocked = (1ull << 10) | (1ull << 12);
    t_handler(12, SIG_IGN_ADDR, 0, 0);
    kf_set_current(other);
    expect(SYS2(62, 341, 12) == 0 && SYS2(62, 341, 10) == 0, "two blocked signals are sent");
    kf_set_current(me);
    *setp = (1ull << 9) | (1ull << 11);
    expect(sys(128, set, info, 0, 8, 0, 0, 0) == 10 && ip->si_signo == 10 && ip->pid == 342 &&
           (ks_id(me)->sig_pending & (1ull << 10)) == 0u,
           "rt_sigtimedwait takes the lowest of the set, with why it came");
    expect(sys(128, set, info, 0, 8, 0, 0, 0) == 12,
           "an ignored signal that is blocked was kept, and is taken");
    tsp[0] = 0;
    tsp[1] = 0;
    expect(sys(128, set, info, ts, 8, 0, 0, 0) == -VIBEOS_EAGAIN, "with nothing pending, a zero wait is EAGAIN");
    tsp[1] = 1000000000;
    expect(sys(128, set, info, ts, 8, 0, 0, 0) == -VIBEOS_EINVAL, "a timespec that is not one is EINVAL");
    (void)sys(128, set, info, 0, 8, 0, 0, &how);
    expect(how == KF_BLOCKED, "without a time it waits");

    /* ---- rt_sigqueueinfo, rt_tgsigqueueinfo -------------------------------- */
    me = fresh(351);
    handler = kf_ualloc(16);
    stack = kf_ualloc(16384);
    other = kf_spawn(352, 351);
    qi = kf_ualloc(128);
    qp = (linux_siginfo_t *)kf_uptr(qi);
    qp->si_code = LINUX_SI_QUEUE;
    qp->pid = 352;
    qp->value = 42;
    kf_set_current(other);
    expect(SYS3(129, 351, 10, qi) == 0 && kf_siginfo(me, 10)->from == VIBEOS_SIG_FROM_QUEUE &&
           kf_siginfo(me, 10)->addr == 42u,
           "rt_sigqueueinfo queues a signal with the sender's value");
    qp->si_code = LINUX_SI_USER;
    expect(SYS3(129, 351, 12, qi) == -VIBEOS_EPERM, "but may not claim kill sent it, to another process");
    expect(SYS3(129, 352, 12, qi) == 0, "to itself it may");
    qp->si_code = LINUX_SI_QUEUE;
    expect(SYS4(297, 999, 351, 12, qi) == -VIBEOS_ESRCH, "rt_tgsigqueueinfo checks the thread is in the group");
    expect(SYS4(297, 351, 351, 12, qi) == 0, "and queues to the thread named");
    kf_set_current(me);
    ks_id(me)->sig_pending = 0;

    /* ---- a fault, given to the program ------------------------------------- */
    me = fresh(361);
    handler = kf_ualloc(16);
    stack = kf_ualloc(16384);
    {
        vibeos_siginfo_t why;

        memset(&why, 0, sizeof(why));
        why.from = VIBEOS_SIG_FROM_FAULT;
        why.trapno = 14;
        why.fault_err = 0x6;          /* a write, from user, to a page not present */
        why.addr = 0x40;
        memset(&fr, 0, sizeof(fr));
        fr.ip = 0x1234;
        fr.sp = stack + 12000u;
        expect(linux_signal_fault(&fr, 11, &why) == 0, "a fault with no handler is the architecture's to kill");
        t_handler(11, handler, T_SA_SIGINFO, 0);
        ks_id(me)->sig_blocked = 1ull << 11;
        expect(linux_signal_fault(&fr, 11, &why) == 0 && (ks_id(me)->sig_pending & (1ull << 11)) == 0u,
               "and one the program blocks, as Linux forces it - nothing left pending behind it");
        ks_id(me)->sig_blocked = 0;
        expect(linux_signal_fault(&fr, 11, &why) == 1 && fr.ip == handler, "with a handler, the program takes it");
        si = (const linux_siginfo_t *)kf_uptr(fr.arg1);
        uc = (const linux_ucontext_t *)kf_uptr(fr.arg2);
        expect(si->si_signo == 11 && si->si_code == LINUX_SEGV_MAPERR && linux_si_addr(si) == 0x40,
               "told the address, and that nothing was mapped there");
        expect(uc->uc_mcontext.rip == 0x1234 && uc->uc_mcontext.trapno == 14u &&
               uc->uc_mcontext.err == 0x6u && uc->uc_mcontext.cr2 == 0x40u,
               "with the faulting rip, trap number, error code and cr2 in the ucontext");
        ks_id(me)->sig_blocked = 0;
        why.fault_err = 0x7;
        memset(&fr, 0, sizeof(fr));
        fr.ip = 0x1234;
        fr.sp = stack + 12000u;
        expect(linux_signal_fault(&fr, 11, &why) == 1 &&
               ((const linux_siginfo_t *)kf_uptr(fr.arg1))->si_code == LINUX_SEGV_ACCERR,
               "a present page refused is SEGV_ACCERR");
        ks_id(me)->sig_blocked = 0;
        why.trapno = 13;
        why.addr = 0x1234;
        memset(&fr, 0, sizeof(fr));
        fr.ip = 0x1234;
        fr.sp = stack + 12000u;
        expect(linux_signal_fault(&fr, 11, &why) == 1 &&
               ((const linux_siginfo_t *)kf_uptr(fr.arg1))->si_code == LINUX_SI_KERNEL &&
               linux_si_addr((const linux_siginfo_t *)kf_uptr(fr.arg1)) == 0u,
               "a general protection fault names no address");
    }

    /* Found by LTP once its timeout worked (L2 step 3). */
    me = fresh(371);
    other = kf_spawn(372, 371);
    ks_id(other)->tgid = 371;            /* a second thread of 371 */
    kf_set_current(me);
    {
        uint64_t q = kf_ualloc(128);
        linux_siginfo_t *qq = (linux_siginfo_t *)kf_uptr(q);

        qq->si_code = LINUX_SI_QUEUE;
        expect(SYS3(129, 372, 10, q) == 0 && SYS2(62, 372, 12) == 0,
               "rt_sigqueueinfo and kill find a process by one of its threads' ids");
    }
    ks_set_ps(other, 0);                 /* on its way out */
    expect(SYS2(62, 372, 15) == 0 && SYS2(200, 372, 15) == 0,
           "a signal to a process that is exiting is not the sender's error");
    expect(SYS3(234, (uint64_t)-1, 371, 10) == -VIBEOS_EINVAL && SYS3(234, 371, 0, 10) == -VIBEOS_EINVAL,
           "tgkill refuses an id that is not positive before looking it up");
    expect(SYS1(121, 0) == (long)ks_id(me)->pgid && SYS1(121, 372) == (long)ks_id(other)->pgid &&
           SYS1(121, 999) == -VIBEOS_ESRCH,
           "getpgid: the caller's group for 0, another's by pid, ESRCH for nobody");
    {
        uint64_t act = kf_ualloc(32);
        uint64_t *aa = (uint64_t *)kf_uptr(act);
        aa[0] = kf_ualloc(16);
        aa[1] = T_SA_RESTORER | T_SA_SIGINFO;
        aa[2] = 0x3000;
        expect(SYS4(13, 10, act, 0, 4) == -VIBEOS_EINVAL && SYS4(14, 0, 0, act, 16) == -VIBEOS_EINVAL,
               "rt_sigaction and rt_sigprocmask refuse a sigset that is not eight bytes");
    }
    expect(SYS3(61, 0x80000000u, 0, 0) == -VIBEOS_ESRCH, "waitpid(INT_MIN) is ESRCH, as Linux says");
}

/* ---- timers (docs/abi/ L2 step 3) --------------------------------------------------- */

static void t_timers(void) {
    uint64_t itv, old, ts, its, sev, idp, set, info, tv, tz, tms;
    linux_itimerval_t *ip, *op;
    linux_itimerspec_t *sp;
    linux_sigevent_t *ev;
    linux_timespec_t *tsp;
    linux_siginfo_t *si;
    int32_t *id;
    kf_outcome_t how;
    int me, other;
    long r;

    /* alarm: seconds, and what was left of the last one. */
    me = fresh(401);
    expect(SYS1(37, 2) == 0, "alarm with none before returns 0");
    expect(SYS1(37, 5) == 2, "a second alarm returns what was left of the first");
    kf_cpu(260, 1);
    expect(SYS1(37, 0) == 2 && (ks_id(me)->sig_pending & (1ull << 14)) == 0u,
           "2.4 seconds left reads as 2, and alarm(0) cancels it");
    expect(SYS1(37, 1) == 0, "nothing was left after a cancel");
    kf_cpu(99, 1);
    expect(SYS1(37, 1) == 1 && (ks_id(me)->sig_pending & (1ull << 14)) == 0u,
           "a hundredth of a second left is still 1, not 0 - 0 would mean none");
    r = sys(34, 0, 0, 0, 0, 0, 0, &how);
    expect(r == -VIBEOS_EINTR && how == 0, "pause is ended by the alarm");
    expect((ks_id(me)->sig_pending & (1ull << 14)) != 0u && kf_siginfo(me, 14)->from == VIBEOS_SIG_FROM_KERNEL,
           "SIGALRM is raised, from the kernel as Linux's alarm is");

    /* setitimer and getitimer. */
    me = fresh(402);
    itv = kf_ualloc(32);
    old = kf_ualloc(32);
    ip = (linux_itimerval_t *)kf_uptr(itv);
    op = (linux_itimerval_t *)kf_uptr(old);
    ip->it_value.tv_sec = 0;
    ip->it_value.tv_usec = 300000;
    ip->it_interval.tv_sec = 0;
    ip->it_interval.tv_usec = 100000;
    expect(SYS3(38, 0, itv, old) == 0 && op->it_value.tv_sec == 0 && op->it_value.tv_usec == 0,
           "setitimer arms ITIMER_REAL, and there was none");
    expect(SYS2(36, 0, old) == 0 && op->it_value.tv_usec == 300000 && op->it_interval.tv_usec == 100000,
           "getitimer reads it back");
    kf_cpu(31, 1);
    expect((ks_id(me)->sig_pending & (1ull << 14)) != 0u, "it fires after its 0.3 seconds");
    ks_id(me)->sig_pending = 0;
    kf_cpu(10, 1);
    expect((ks_id(me)->sig_pending & (1ull << 14)) != 0u, "and again after its period");
    ip->it_value.tv_usec = 1000000;
    expect(SYS3(38, 0, itv, 0) == -VIBEOS_EINVAL && SYS3(38, 3, itv, 0) == -VIBEOS_EINVAL,
           "a timeval that is not one, and a fourth timer, are EINVAL");
    expect(SYS3(38, 0, 0, old) == 0 && op->it_interval.tv_usec == 100000 && SYS2(36, 0, old) == 0 &&
           op->it_value.tv_usec == 0 && op->it_value.tv_sec == 0,
           "no new value disarms it, and says what it was");
    ip->it_value.tv_usec = 50000;
    ip->it_interval.tv_usec = 0;
    ks_id(me)->sig_pending = 0;
    expect(SYS3(38, 1, itv, 0) == 0, "ITIMER_VIRTUAL armed for 5 ticks of user time");
    kf_cpu(10, 0);
    expect((ks_id(me)->sig_pending & (1ull << 26)) == 0u, "time in the kernel does not count for it");
    kf_cpu(5, 1);
    expect((ks_id(me)->sig_pending & (1ull << 26)) != 0u, "user time does: SIGVTALRM");
    expect(SYS3(38, 2, itv, 0) == 0, "ITIMER_PROF armed for 5 ticks");
    kf_cpu(5, 0);
    expect((ks_id(me)->sig_pending & (1ull << 27)) != 0u, "kernel time counts for it: SIGPROF");

    /* POSIX timers. */
    me = fresh(403);
    other = kf_spawn(404, 403);
    its = kf_ualloc(32);
    sev = kf_ualloc(64);
    idp = kf_ualloc(4);
    set = kf_ualloc(8);
    info = kf_ualloc(128);
    sp = (linux_itimerspec_t *)kf_uptr(its);
    ev = (linux_sigevent_t *)kf_uptr(sev);
    id = (int32_t *)kf_uptr(idp);
    si = (linux_siginfo_t *)kf_uptr(info);
    expect(SYS3(222, LINUX_CLOCK_MONOTONIC, 0, idp) == 0 && *id == 0 &&
           SYS3(222, LINUX_CLOCK_MONOTONIC, 0, idp) == 0 && *id == 1,
           "timer_create numbers timers from zero");
    sp->it_value.tv_sec = 0;
    sp->it_value.tv_nsec = 20000000;
    expect(sys(223, 1, 0, its, 0, 0, 0, 0) == 0, "timer_settime arms the second one for 20ms");
    ks_id(me)->sig_blocked = 1ull << 14;
    kf_cpu(3, 1);
    *(uint64_t *)kf_uptr(set) = 1ull << 13;
    expect(sys(128, set, info, 0, 8, 0, 0, 0) == 14 && si->si_code == LINUX_SI_TIMER && si->pid == 1 &&
           si->value == 1u,
           "without a sigevent: SIGALRM, SI_TIMER, the timer's id as its value");
    ev->sigev_signo = 10;
    ev->sigev_notify = LINUX_SIGEV_SIGNAL;
    ev->sigev_value = 0x55;
    expect(SYS3(222, LINUX_CLOCK_REALTIME, sev, idp) == 0 && *id == 2, "one with a sigevent");
    sp->it_value.tv_nsec = 10000000;
    sp->it_interval.tv_nsec = 10000000;
    ks_id(me)->sig_blocked = 1ull << 10;
    expect(sys(223, 2, 0, its, 0, 0, 0, 0) == 0, "periodic, every tick, with its signal blocked");
    kf_cpu(12, 1);
    *(uint64_t *)kf_uptr(set) = 1ull << 9;
    r = sys(128, set, info, 0, 8, 0, 0, 0);
    expect(r == 10 && si->value == 0x55u && si->uid >= 8u && SYS1(225, 2) == (long)si->uid,
           "one signal for many expiries: the rest are its overrun, as timer_getoverrun says");
    expect(sys(224, 2, its, 0, 0, 0, 0, 0) == 0 && sp->it_interval.tv_nsec == 10000000 &&
           sp->it_value.tv_nsec > 0, "timer_gettime reads it");
    expect(SYS1(226, 2) == 0 && sys(224, 2, its, 0, 0, 0, 0, 0) == -VIBEOS_EINVAL &&
           SYS1(226, 2) == -VIBEOS_EINVAL && SYS1(225, 2) == -VIBEOS_EINVAL,
           "timer_delete, and a deleted timer is no timer");
    ev->sigev_notify = LINUX_SIGEV_NONE;
    expect(SYS3(222, LINUX_CLOCK_MONOTONIC, sev, idp) == 0, "SIGEV_NONE");
    sp->it_value.tv_nsec = 50000000;
    sp->it_interval.tv_nsec = 0;
    (void)sys(223, (uint64_t)*id, 0, its, 0, 0, 0, 0);
    kf_cpu(2, 1);
    expect(sys(224, (uint64_t)*id, its, 0, 0, 0, 0, 0) == 0 && sp->it_value.tv_nsec == 30000000,
           "a silent timer still counts down");
    ev->sigev_notify = LINUX_SIGEV_THREAD_ID;
    ev->notify_tid = 999;
    expect(SYS3(222, LINUX_CLOCK_MONOTONIC, sev, idp) == -VIBEOS_EINVAL, "SIGEV_THREAD_ID to no thread");
    ev->notify_tid = 404;
    expect(SYS3(222, LINUX_CLOCK_MONOTONIC, sev, idp) == -VIBEOS_EINVAL,
           "nor to a thread of another process");
    ev->notify_tid = 403;
    ev->sigev_signo = 12;
    expect(SYS3(222, LINUX_CLOCK_MONOTONIC, sev, idp) == 0, "to one of its own");
    ev->sigev_notify = 7;
    expect(SYS3(222, LINUX_CLOCK_MONOTONIC, sev, idp) == -VIBEOS_EINVAL, "an unknown notify is EINVAL");
    ev->sigev_notify = LINUX_SIGEV_SIGNAL;
    ev->sigev_signo = 65;
    expect(SYS3(222, LINUX_CLOCK_MONOTONIC, sev, idp) == -VIBEOS_EINVAL, "so is a signal that does not exist");
    expect(SYS3(222, 99, 0, idp) == -VIBEOS_EINVAL && SYS3(222, LINUX_CLOCK_MONOTONIC_RAW, 0, idp) == -VIBEOS_EINVAL,
           "a clock it does not know, and one Linux keeps no timers on, are EINVAL");
    sp->it_value.tv_nsec = 1000000000;
    expect(sys(223, 0, 0, its, 0, 0, 0, 0) == -VIBEOS_EINVAL, "timer_settime refuses a time that is not one");
    sp->it_value.tv_nsec = 0;   /* a valid time, so that only the flag or the id is wrong */
    expect(sys(223, 0, 2, its, 0, 0, 0, 0) == -VIBEOS_EINVAL && sys(223, 77, 0, its, 0, 0, 0, 0) == -VIBEOS_EINVAL,
           "and a bad flag, and a timer that is not there");
    /* Absolute, on the machine's clock. */
    ks_id(me)->sig_pending = 0;
    ks_id(me)->sig_blocked = 0;
    sp->it_value.tv_sec = (int64_t)(ks_ticks() / 100u);
    sp->it_value.tv_nsec = (int64_t)((ks_ticks() % 100u) + 5u) * 10000000;
    if (sp->it_value.tv_nsec >= 1000000000) {
        sp->it_value.tv_sec++;
        sp->it_value.tv_nsec -= 1000000000;
    }
    expect(sys(223, 0, 1, its, 0, 0, 0, 0) == 0, "TIMER_ABSTIME: five ticks from now, as a clock reading");
    kf_cpu(4, 1);
    expect((ks_id(me)->sig_pending & (1ull << 14)) == 0u, "not before the clock reads it");
    kf_cpu(1, 1);
    expect((ks_id(me)->sig_pending & (1ull << 14)) != 0u, "and when it does");
    /* On the process's CPU clock. */
    ks_id(me)->sig_pending = 0;
    expect(SYS3(222, LINUX_CLOCK_PROCESS_CPUTIME_ID, 0, idp) == 0, "a timer on the process's CPU clock");
    sp->it_value.tv_sec = 0;
    sp->it_value.tv_nsec = 30000000;
    (void)sys(223, (uint64_t)*id, 0, its, 0, 0, 0, 0);
    kf_set_current(other);
    kf_cpu(5, 1);
    kf_set_current(me);
    expect((ks_id(me)->sig_pending & (1ull << 14)) == 0u, "another process's time does not count for it");
    kf_cpu(3, 0);
    expect((ks_id(me)->sig_pending & (1ull << 14)) != 0u, "its own does");

    /* The clocks. */
    me = fresh(405);
    ts = kf_ualloc(16);
    tsp = (linux_timespec_t *)kf_uptr(ts);
    expect(SYS2(229, LINUX_CLOCK_REALTIME, ts) == 0 && tsp->tv_sec == 0 && tsp->tv_nsec == 10000000,
           "clock_getres says a tick");
    expect(SYS2(229, LINUX_CLOCK_THREAD_CPUTIME_ID, 0) == 0 && SYS2(229, 99, ts) == -VIBEOS_EINVAL,
           "with nowhere to put it too, and EINVAL for a clock it does not know");
    /* The wall clock moves while the process waits: the two clocks must
     * differ, or a CPU clock that read the wall clock would pass. */
    tsp->tv_sec = 0;
    tsp->tv_nsec = 300000000;
    (void)SYS2(35, ts, 0);
    kf_cpu(7, 1);
    expect(ks_ticks() > 30u && SYS2(228, LINUX_CLOCK_PROCESS_CPUTIME_ID, ts) == 0 && tsp->tv_sec == 0 && tsp->tv_nsec == 70000000,
           "clock_gettime on the process's CPU clock: what it ran");
    expect(SYS2(228, LINUX_CLOCK_THREAD_CPUTIME_ID, ts) == 0 && tsp->tv_nsec == 70000000 &&
           SYS2(228, 99, ts) == -VIBEOS_EINVAL,
           "and on the thread's; an unknown clock is EINVAL, not the wall clock");
    /* A child forked into a slot that has run before starts from nothing:
     * the accounting is the slot's, every tenant's. */
    {
        long cpid = SYS0(57);
        int cslot = -1;
        uint32_t k;

        for (k = 0; k < ks_slots(); k++) {
            if (ks_id((int)k)->pid == (uint32_t)cpid) {
                cslot = (int)k;
            }
        }
        if (cslot >= 0) {
            kf_set_current(cslot);
            kf_cpu(4, 1);
            expect(SYS2(228, LINUX_CLOCK_THREAD_CPUTIME_ID, ts) == 0 && tsp->tv_nsec == 40000000,
                   "a forked child's CPU clock counts its own time only");
            (void)vibeos_task_transition((uint32_t)cslot, VIBEOS_TASK_ZOMBIE, "test");
            (void)vibeos_task_transition((uint32_t)cslot, VIBEOS_TASK_FREE, "test");
            kf_set_current(me);
            cpid = SYS0(57);
            kf_set_current(cslot);
            expect(ks_id(cslot)->pid == (uint32_t)cpid && SYS2(228, LINUX_CLOCK_THREAD_CPUTIME_ID, ts) == 0 &&
                   tsp->tv_nsec == 0 && tsp->tv_sec == 0,
                   "and one forked into the same slot afterwards starts from zero, not from its predecessor's");
            kf_set_current(me);
        } else {
            expect(0, "fork made a child");
        }
    }
    tv = kf_ualloc(16);
    tz = kf_ualloc(8);
    memset(kf_uptr(tz), 0xFF, 8);
    expect(SYS2(96, tv, tz) == 0 && ((linux_timeval_t *)kf_uptr(tv))->tv_sec == (int64_t)(ks_ticks() / 100u) &&
           ((linux_timezone_t *)kf_uptr(tz))->tz_minuteswest == 0, "gettimeofday, and a zone of nothing");
    tms = kf_ualloc(32);
    r = SYS1(100, tms);
    expect(r == (long)ks_ticks() && ((linux_tms_t *)kf_uptr(tms))->tms_utime == 7,
           "times: the clock in USER_HZ, and the CPU time the process ran");
    expect(SYS2(227, LINUX_CLOCK_REALTIME, ts) == -VIBEOS_EPERM, "setting the clock is refused");

    /* A timer that outlives its process is counted, and goes to nobody else:
     * the exit path is supposed to have taken it. */
    {
        uint64_t before = vibeos_mbz_count(VIBEOS_MBZ_PTIMER_ORPHAN);

        me = fresh(406);
        other = kf_spawn(407, 406);
        (void)SYS1(37, 1);
        (void)vibeos_task_transition((uint32_t)me, VIBEOS_TASK_ZOMBIE, "test");
        (void)vibeos_task_transition((uint32_t)me, VIBEOS_TASK_FREE, "test");
        kf_set_current(other);
        kf_cpu(102, 1);
        expect(vibeos_mbz_count(VIBEOS_MBZ_PTIMER_ORPHAN) == before + 1u &&
               (ks_id(other)->sig_pending & (1ull << 14)) == 0u,
               "a timer whose process is gone is ptimer_orphan, and nobody else's alarm");
    }
}

/* ---- limits, usage, priorities (docs/abi/ L2 step 4) ---------------------------------- */

static void t_limits(void) {
    uint64_t rl, rl2, buf, path, ru;
    uint64_t *rp, *rp2;
    int me, other;
    long r, fd, child;
    uint32_t i;

    me = fresh(501);
    rl = kf_ualloc(16);
    rl2 = kf_ualloc(16);
    rp = (uint64_t *)kf_uptr(rl);
    rp2 = (uint64_t *)kf_uptr(rl2);

    /* What a process starts with. */
    expect(SYS2(97, LINUX_RLIMIT_NOFILE, rl) == 0 && rp[0] == 1024u && rp[1] == 1024u,
           "RLIMIT_NOFILE starts at what the descriptor table holds");
    expect(SYS2(97, LINUX_RLIMIT_CORE, rl) == 0 && rp[0] == 0u && rp[1] == LINUX_RLIM64_INFINITY,
           "RLIMIT_CORE starts at 0, as Linux starts it");
    expect(SYS2(97, LINUX_RLIMIT_STACK, rl) == 0 && rp[0] == ks_stack_bytes(),
           "RLIMIT_STACK is the stack this machine gives");
    expect(SYS2(97, 16, rl) == -VIBEOS_EINVAL && SYS2(97, (uint64_t)-1, rl) == -VIBEOS_EINVAL,
           "a resource that does not exist is EINVAL");
    expect(SYS4(302, 0, LINUX_RLIMIT_NOFILE, 0, rl2) == 0 && rp2[0] == 1024u,
           "prlimit64 reads what getrlimit reads");

    /* The rules for changing one. */
    rp[0] = 20;
    rp[1] = 10;
    expect(SYS2(160, LINUX_RLIMIT_NOFILE, rl) == -VIBEOS_EINVAL, "a soft limit above the hard one is EINVAL");
    rp[0] = 2048;
    rp[1] = 2048;
    expect(SYS2(160, LINUX_RLIMIT_NOFILE, rl) == -VIBEOS_EPERM,
           "nobody raises NOFILE past what the table can hold");

    /* NOFILE is the descriptor table's limit. */
    rp[0] = 5;
    rp[1] = 1024;
    expect(SYS2(160, LINUX_RLIMIT_NOFILE, rl) == 0 && ks_ps(me)->files.limit == 5u,
           "setrlimit(RLIMIT_NOFILE) is the table's limit");
    for (i = 0, fd = 0; i < 10u && fd >= 0; i++) {
        fd = SYS1(32, 0);   /* dup */
    }
    expect(fd == -VIBEOS_EMFILE, "and a descriptor past it is EMFILE");

    /* Children inherit limits, nice and personality. */
    me = fresh(502);
    rl = kf_ualloc(16);
    rp = (uint64_t *)kf_uptr(rl);
    rp[0] = 50;
    rp[1] = 60;
    (void)SYS2(160, LINUX_RLIMIT_NOFILE, rl);
    (void)ks_task_set_nice(me, 7);
    expect(SYS1(135, 0x0008) == 0 && SYS1(135, 0xFFFFFFFFu) == 0x0008, "personality is kept and read back");
    child = SYS0(57);
    {
        int cs = -1;
        uint32_t k;
        for (k = 0; k < ks_slots(); k++) {
            if (ks_id((int)k)->pid == (uint32_t)child) {
                cs = (int)k;
            }
        }
        expect(cs >= 0 && ks_ps(cs)->rlim_cur[LINUX_RLIMIT_NOFILE] == 50u &&
               ks_ps(cs)->rlim_max[LINUX_RLIMIT_NOFILE] == 60u && ks_ps(cs)->files.limit == 50u &&
               ks_task_nice(cs) == 7 && ks_ps(cs)->personality == 0x0008u,
               "a forked child has its parent's limits, nice and personality");
    }

    /* FSIZE: a write is cut at the limit, and past it is SIGXFSZ and EFBIG. */
    me = fresh(503);
    rl = kf_ualloc(16);
    rp = (uint64_t *)kf_uptr(rl);
    buf = kf_ualloc(300);
    path = ustr("/tmp/big");
    fd = SYS3(2, path, 0x42, 0644);   /* O_CREAT | O_RDWR */
    rp[0] = 100;
    rp[1] = LINUX_RLIM64_INFINITY;
    expect(fd >= 0 && SYS2(160, LINUX_RLIMIT_FSIZE, rl) == 0, "RLIMIT_FSIZE set to 100 bytes");
    expect(SYS3(1, (uint64_t)fd, buf, 200) == 100, "a write that would pass it is cut at it");
    expect(SYS3(1, (uint64_t)fd, buf, 10) == -VIBEOS_EFBIG && (ks_id(me)->sig_pending & (1ull << VIBEOS_SIGXFSZ)),
           "one at it is EFBIG, with SIGXFSZ");
    ks_id(me)->sig_pending = 0;

    /* DATA: brk may not take the heap past it. */
    {
        uint64_t base = (uint64_t)SYS1(12, 0);
        rp[0] = (base - ks_heap_base()) + 8192u;
        rp[1] = LINUX_RLIM64_INFINITY;
        expect(SYS2(160, LINUX_RLIMIT_DATA, rl) == 0 && (uint64_t)SYS1(12, base + 4096u) == base + 4096u,
               "brk within RLIMIT_DATA moves");
        expect((uint64_t)SYS1(12, base + 65536u) == base + 4096u,
               "and past it stays where it was, as Linux refuses");
    }

    /* CPU: SIGXCPU at the soft limit, SIGKILL at the hard one. */
    me = fresh(504);
    rl = kf_ualloc(16);
    rp = (uint64_t *)kf_uptr(rl);
    rp[0] = 1;
    rp[1] = 2;
    expect(SYS2(160, LINUX_RLIMIT_CPU, rl) == 0, "RLIMIT_CPU of one second, two hard");
    kf_cpu(99, 1);
    expect((ks_id(me)->sig_pending & (1ull << VIBEOS_SIGXCPU)) == 0u, "not before a second has run");
    kf_cpu(1, 1);
    expect((ks_id(me)->sig_pending & (1ull << VIBEOS_SIGXCPU)) != 0u, "SIGXCPU when it has");
    kf_cpu(100, 1);
    expect((ks_id(me)->sig_pending & (1ull << VIBEOS_SIGKILL)) != 0u, "SIGKILL at the hard limit");
    ks_id(me)->sig_pending = 0;

    /* NPROC: counted for a user who is not root. */
    me = fresh(505);
    rl = kf_ualloc(16);
    rp = (uint64_t *)kf_uptr(rl);
    rp[0] = 1;
    rp[1] = 1;
    expect(SYS2(160, LINUX_RLIMIT_NPROC, rl) == 0 && SYS0(57) > 0, "the superuser is not held to RLIMIT_NPROC");
    kf_set_current(me);
    expect(SYS1(105, 1000) == 0 && SYS0(57) == -VIBEOS_EAGAIN,
           "a user at RLIMIT_NPROC cannot fork");
    rp[0] = 1;
    rp[1] = 2;
    expect(SYS2(160, LINUX_RLIMIT_NPROC, rl) == -VIBEOS_EPERM, "nor raise a hard limit");

    /* prlimit64 on another process. */
    me = fresh(506);
    other = kf_spawn(507, 506);
    rl = kf_ualloc(16);
    rp = (uint64_t *)kf_uptr(rl);
    rp[0] = 33;
    rp[1] = 1024;
    expect(SYS4(302, 507, LINUX_RLIMIT_NOFILE, rl, 0) == 0 && ks_ps(other)->rlim_cur[LINUX_RLIMIT_NOFILE] == 33u &&
           ks_ps(other)->files.limit == 33u,
           "prlimit64 changes another process's limit");
    expect(SYS4(302, 999, LINUX_RLIMIT_NOFILE, 0, rl) == -VIBEOS_ESRCH, "and refuses one that does not exist");
    kf_set_current(other);
    (void)SYS1(105, 1000);
    expect(SYS4(302, 506, LINUX_RLIMIT_NOFILE, 0, rl) == -VIBEOS_EPERM,
           "and a user may not reach the superuser's");
    kf_set_current(me);

    /* getrusage. */
    me = fresh(508);
    ru = kf_ualloc(sizeof(linux_rusage_t));
    kf_cpu(5, 1);
    expect(SYS2(98, LINUX_RUSAGE_SELF, ru) == 0 && ((linux_rusage_t *)kf_uptr(ru))->ru_utime.tv_usec == 50000,
           "getrusage(SELF): the CPU time run");
    expect(SYS2(98, LINUX_RUSAGE_THREAD, ru) == 0 && ((linux_rusage_t *)kf_uptr(ru))->ru_utime.tv_usec == 50000 &&
           SYS2(98, 5, ru) == -VIBEOS_EINVAL,
           "RUSAGE_THREAD too; an unknown who is EINVAL");

    /* Priorities: 20 - nice, raw, as Linux answers. */
    me = fresh(509);
    other = kf_spawn(510, 509);
    expect(SYS2(140, LINUX_PRIO_PROCESS, 0) == 20, "getpriority answers 20 - nice");
    expect(SYS3(141, LINUX_PRIO_PROCESS, 0, 5) == 0 && ks_task_nice(me) == 5 && SYS2(140, LINUX_PRIO_PROCESS, 0) == 15,
           "setpriority sets the caller's nice");
    expect(SYS3(141, LINUX_PRIO_PROCESS, 510, 3) == 0 && ks_task_nice(other) == 3,
           "and another's, by pid");
    expect(SYS2(140, LINUX_PRIO_PGRP, 0) == 15 && SYS2(140, LINUX_PRIO_USER, 0) == 17,
           "a group answers for its members only, a user for all its processes: the highest priority");
    expect(SYS3(141, 3, 0, 0) == -VIBEOS_EINVAL && SYS2(140, LINUX_PRIO_PROCESS, 999) == -VIBEOS_ESRCH,
           "an unknown which is EINVAL, nobody is ESRCH");
    expect(SYS3(141, LINUX_PRIO_PROCESS, 0, 40) == 0 && ks_task_nice(me) == 19, "a nice past 19 is 19");
    (void)SYS1(105, 1000);
    expect(SYS3(141, LINUX_PRIO_PROCESS, 0, 10) == -VIBEOS_EACCES,
           "a user may not lower its own nice without RLIMIT_NICE");
    expect(SYS3(141, LINUX_PRIO_PROCESS, 510, 15) == -VIBEOS_EPERM,
           "nor touch the superuser's");
    r = SYS3(141, LINUX_PRIO_PROCESS, 0, 19);
    expect(r == 0, "raising it is anybody's");
}

/* ---- processes (docs/abi/ L2 step 5) ------------------------------------------------- */

/* Fork a child of the current task and return its slot, or -1. */
static int t_fork_child(long *pid_out) {
    long pid = SYS0(57);
    uint32_t k;

    *pid_out = pid;
    for (k = 0; k < ks_slots(); k++) {
        if (pid > 0 && ks_id((int)k)->pid == (uint32_t)pid) {
            return (int)k;
        }
    }
    return -1;
}

/* The child in `slot` ends with `code`, or killed by `sig`. */
static void t_child_ends(int slot, uint32_t code, uint32_t sig) {
    ks_id(slot)->exit_code = code;
    ks_id(slot)->exit_signal = sig;
    ks_id(slot)->exit_uid = 77;
    (void)vibeos_task_transition((uint32_t)slot, VIBEOS_TASK_ZOMBIE, "test");
}

static void t_processes(void) {
    uint64_t st, ru, info, idp, args, set;
    linux_siginfo_t *si;
    long pid, pid2, r;
    int me, c1, c2, fd;

    /* wait4 by group: 0 is the caller's, -pgid another's. */
    me = fresh(601);
    st = kf_ualloc(4);
    ru = kf_ualloc(sizeof(linux_rusage_t));
    c1 = t_fork_child(&pid);
    kf_set_current(me);
    c2 = t_fork_child(&pid2);
    kf_set_current(me);
    ks_id(c2)->pgid = 999;            /* the second child in a group of its own */
    t_child_ends(c1, 3, 0);
    t_child_ends(c2, 4, 0);
    expect(SYS3(61, (uint64_t)-999, st, 0) == pid2 && *(int *)kf_uptr(st) == (4 << 8),
           "waitpid(-pgid) reaps a child of that group");
    expect(sys(61, 0, st, 0, ru, 0, 0, 0) == pid && *(int *)kf_uptr(st) == (3 << 8),
           "waitpid(0) a child of the caller's group, with its rusage");
    expect(SYS3(61, 0, st, 0) == -VIBEOS_ECHILD, "and then there are none");

    /* waitid. */
    me = fresh(602);
    info = kf_ualloc(128);
    si = (linux_siginfo_t *)kf_uptr(info);
    c1 = t_fork_child(&pid);
    kf_set_current(me);
    expect(sys(247, LINUX_P_PID, (uint64_t)pid, info, LINUX_WEXITED | 1u /* WNOHANG */, 0, 0, 0) == 0 &&
           si->pid == 0 && si->si_signo == 0,
           "waitid with WNOHANG and nothing ended: 0, and a record of zeroes");
    t_child_ends(c1, 0, 9);
    expect(sys(247, LINUX_P_PID, (uint64_t)pid, info, LINUX_WEXITED | LINUX_WNOWAIT, 0, 0, 0) == 0 &&
           si->si_signo == 17 && si->si_code == LINUX_CLD_KILLED && si->pid == (int32_t)pid &&
           si->uid == 77u && (int32_t)si->value == 9 &&
           vibeos_task_state((uint32_t)c1) == VIBEOS_TASK_ZOMBIE,
           "waitid says who ended and how, and WNOWAIT leaves it to be reaped");
    expect(sys(247, LINUX_P_ALL, 0, info, LINUX_WEXITED, 0, 0, 0) == 0 && si->pid == (int32_t)pid &&
           vibeos_task_state((uint32_t)c1) == VIBEOS_TASK_FREE,
           "and without it, reaps");
    expect(sys(247, LINUX_P_ALL, 0, info, 0, 0, 0, 0) == -VIBEOS_EINVAL &&
           sys(247, 9, 0, info, LINUX_WEXITED, 0, 0, 0) == -VIBEOS_EINVAL &&
           sys(247, LINUX_P_PID, 0, info, LINUX_WEXITED, 0, 0, 0) == -VIBEOS_EINVAL,
           "no event asked for, an unknown idtype, a pid of 0: EINVAL");
    expect(sys(247, LINUX_P_ALL, 0, info, LINUX_WEXITED, 0, 0, 0) == -VIBEOS_ECHILD, "no children: ECHILD");

    /* pidfds. */
    me = fresh(603);
    info = kf_ualloc(128);
    si = (linux_siginfo_t *)kf_uptr(info);
    c1 = t_fork_child(&pid);
    kf_set_current(me);
    fd = (int)SYS2(434, (uint64_t)pid, 0);
    expect(fd >= 0, "pidfd_open names a child");
    expect(SYS2(434, 0, 0) == -VIBEOS_EINVAL && SYS2(434, 9999, 0) == -VIBEOS_ESRCH &&
           SYS2(434, (uint64_t)pid, 1) == -VIBEOS_EINVAL,
           "pid 0, nobody and an unknown flag are refused");
    expect(SYS4(424, (uint64_t)fd, 15, 0, 0) == 0 && (ks_id(c1)->sig_pending & (1ull << 15)) != 0u,
           "pidfd_send_signal reaches the process it names");
    expect(SYS4(424, (uint64_t)fd, 15, 0, 1) == -VIBEOS_EINVAL && SYS4(424, 0, 15, 0, 0) == -VIBEOS_EBADF,
           "a flag, or a descriptor that is not a pidfd, is refused");
    t_child_ends(c1, 5, 0);
    expect(sys(247, LINUX_P_PIDFD, (uint64_t)fd, info, LINUX_WEXITED, 0, 0, 0) == 0 && si->pid == (int32_t)pid,
           "waitid(P_PIDFD) waits for it");
    /* The slot is free now; a new child may take it and even its number. */
    c2 = t_fork_child(&pid2);
    kf_set_current(me);
    if (c2 == c1) {
        ks_id(c2)->pid = ks_id(c2)->tgid = (uint32_t)pid;   /* the number reused too */
    }
    expect(SYS4(424, (uint64_t)fd, 15, 0, 0) == -VIBEOS_ESRCH,
           "once reaped, a pidfd names nobody - not whoever has the slot or the number now");

    /* clone3. */
    me = fresh(604);
    args = kf_ualloc(sizeof(linux_clone_args_t));
    idp = kf_ualloc(4);
    {
        linux_clone_args_t *a = (linux_clone_args_t *)kf_uptr(args);
        a->flags = LINUX_CLONE_PIDFD;
        a->exit_signal = 17;
        a->pidfd = idp;
        *(int *)kf_uptr(idp) = -7;
        r = SYS2(435, args, sizeof(*a));
        expect(r > 0 && *(int *)kf_uptr(idp) >= 0, "clone3 forks, and CLONE_PIDFD hands back a pidfd");
        kf_set_current(me);
        expect(SYS2(435, args, 32) == -VIBEOS_EINVAL && SYS2(435, args, 200) == -VIBEOS_E2BIG,
               "a structure too small is EINVAL, a larger one than known E2BIG");
        a->flags = 0;
        a->set_tid = kf_ualloc(8);
        a->set_tid_size = 1;
        expect(SYS2(435, args, sizeof(*a)) == -VIBEOS_EINVAL, "a chosen pid is refused");
        a->set_tid = 0;
        a->set_tid_size = 0;
        a->stack_size = 4096;
        expect(SYS2(435, args, sizeof(*a)) == -VIBEOS_EINVAL, "a stack size with no stack is refused");
    }

    /* execveat's flags. */
    me = fresh(605);
    expect(sys(322, (uint64_t)(uint32_t)-100, ustr("/x"), 0, 0, 0x4, 0, 0) == -VIBEOS_EINVAL,
           "execveat refuses a flag it does not know");
    (void)set;
}

/* ---- M-078, M-079 ------------------------------------------------------------------- */

static void sibling_moves_the_break(vibeos_procstate_t *ps) {
    ps->brk_cur += 0x5000u;
    ps->mmap_cur += 0x9000u;
}

static void t_fork_snapshot_and_groups(void) {
    int parent, child, grand;
    uint64_t brk0, cur0;
    long pid, gpid;

    /* M-078: the child's break and cursor are the ones its regions were cloned
     * beside, whatever a sibling does the moment the lock is released. */
    parent = fresh(151);
    (void)SYS1(12, 0x10000000ull + 8192u);
    (void)MMAP(0, 4096, 3, MAP_PRIV_ANON, -1, 0);
    brk0 = ks_ps(parent)->brk_cur;
    cur0 = ks_ps(parent)->mmap_cur;
    kf_on_mm_unlock(sibling_moves_the_break);
    pid = SYS0(57);
    child = slot_of_pid(pid);
    expect(pid > 0 && child >= 0 && ks_ps(parent)->brk_cur == brk0 + 0x5000u,
           "a sibling moved the break as fork let go of the lock");
    expect(ks_ps(child)->brk_cur == brk0 && ks_ps(child)->mmap_cur == cur0,
           "the child's break and cursor belong to the address space it was given");
    ks_ps(parent)->brk_cur = brk0;
    ks_ps(parent)->mmap_cur = cur0;

    /* M-080: a descriptor whose path now names another file does not act on
     * that file. */
    {
        linux_stat_t st;
        long fd = tmp_open("/tmp/id-a", 0x42, 0644);

        expect(fd >= 0 && SYS2(82, ustr("/tmp/id-a"), ustr("/tmp/id-b")) == 0 &&
               SYS2(91, (uint64_t)fd, 0600) == -VIBEOS_ENOENT,
               "fchmod after the file was renamed away finds nothing at its path");
        expect(SYS1(3, (uint64_t)tmp_open("/tmp/id-a", 0x42, 0644)) == 0 &&
               SYS2(91, (uint64_t)fd, 0600) == -VIBEOS_ESTALE &&
               tmp_stat("/tmp/id-a", 1, &st) == 0 && (st.st_mode & 0777u) == 0644u,
               "and a new file at that path is not the descriptor's: ESTALE, and it is left alone");
        expect(SYS2(82, ustr("/tmp/id-b"), ustr("/tmp/id-a")) == 0 && SYS2(91, (uint64_t)fd, 0600) == 0 &&
               tmp_stat("/tmp/id-a", 1, &st) == 0 && (st.st_mode & 0777u) == 0600u,
               "back at its path, the descriptor's file is the one changed");
    }

    /* M-079: a group is joined only if somebody leads it. */
    kf_set_current(child);
    gpid = SYS0(57);
    grand = slot_of_pid(gpid);
    kf_set_current(parent);
    expect(gpid > 0 && grand >= 0 && ks_id(child)->pgid == 151u && ks_id(grand)->pgid == 151u,
           "children start in their parent's group");
    expect(SYS2(109, (uint64_t)gpid, (uint64_t)pid) == -VIBEOS_EPERM && ks_id(grand)->pgid == 151u,
           "setpgid into a group nobody leads is EPERM");
    expect(SYS2(109, (uint64_t)pid, (uint64_t)pid) == 0 && ks_id(child)->pgid == (uint32_t)pid,
           "a process may lead a group of its own");
    expect(SYS2(109, (uint64_t)gpid, (uint64_t)pid) == 0 && ks_id(grand)->pgid == (uint32_t)pid,
           "and then it can be joined");
    expect(SYS2(109, (uint64_t)gpid, 0) == 0 && ks_id(grand)->pgid == (uint32_t)gpid,
           "setpgid(pid, 0) makes the target its own leader");

    /* M-083: the group is the process's - every thread of it moves - and it
     * exists while something is in it, whether or not its leader still does. */
    {
        int th = kf_spawn(990, ks_id(child)->sid);

        ks_id(th)->tgid = (uint32_t)pid;
        ks_id(th)->is_thread = 1;
        ks_id(th)->pgid = ks_id(child)->pgid;
        kf_set_current(parent);
        expect(SYS2(109, (uint64_t)pid, (uint64_t)gpid) == 0 && ks_id(child)->pgid == (uint32_t)gpid &&
               ks_id(th)->pgid == (uint32_t)gpid, "setpgid moves every thread of the process");
        expect(SYS2(109, (uint64_t)gpid, 990) == -VIBEOS_EPERM && ks_id(grand)->pgid == (uint32_t)gpid,
               "a thread's id is not a group");
        /* The group's leader leaves it; the group is still there, and joined. */
        expect(SYS2(109, (uint64_t)gpid, 151) == 0 && ks_id(grand)->pgid == 151u &&
               SYS2(109, (uint64_t)gpid, (uint64_t)gpid) == 0 &&
               SYS2(109, (uint64_t)gpid, (uint64_t)gpid) == 0, "a process moves between groups that exist");
        expect(SYS2(109, (uint64_t)pid, 151) == 0 && SYS2(109, (uint64_t)gpid, 151) == 0 &&
               SYS2(109, (uint64_t)pid, (uint64_t)gpid) == -VIBEOS_EPERM,
               "a group everybody has left is gone, though the process it was named after lives");
        expect(SYS2(109, (uint64_t)pid, (uint64_t)-1) == -VIBEOS_EINVAL, "a negative group is EINVAL");
    }
}

/* ---- L3 step 2: shared mappings ------------------------------------------------------- */

#define MAP_SH 0x01u
#define MAP_SH_ANON 0x21u

static void t_mmap_shared(void) {
    uint64_t buf;
    uint8_t x = 'Z', y = 0;
    long fd, rfd, m, m2, pid;
    int parent, child;
    uint32_t i;
    uint64_t free1, free2;

    parent = fresh(141);
    buf = kf_ualloc(8192);

    /* Anonymous: one page of memory in two processes. */
    m = MMAP(0, 8192, 3, MAP_SH_ANON, -1, 0);
    expect(m > 0 && mem_is((uint64_t)m, 0, 8192) && kf_poke((uint64_t)m, &x, 1) == 0,
           "a shared anonymous mapping is zeroed memory");
    {
        /* It stays resident: an entry in swap has no way to say "shared".
         * Asked before the fork, while this is the page's only holder - after
         * it the page has two, and a page with two is refused whatever it is. */
        vibeos_vmspace_t v = ks_vm(parent);
        uint64_t refused = vibeos_mm_stats()->swap_refused_shared;
        expect(vibeos_vmspace_swap_out(&v, (uint64_t)m, 3u) != 0 &&
               vibeos_mm_stats()->swap_refused_shared == refused + 1u && mem_is((uint64_t)m, 'Z', 1),
               "a shared page is not paged out");
    }
    pid = SYS0(57);
    child = slot_of_pid(pid);
    expect(pid > 0 && child >= 0, "fork");
    kf_set_current(child);
    y = 'c';
    expect(mem_is((uint64_t)m, 'Z', 1) && kf_poke((uint64_t)m + 1u, &y, 1) == 0,
           "the child sees it and stores into it");
    kf_set_current(parent);
    y = 'p';
    expect(mem_is((uint64_t)m + 1u, 'c', 1) && kf_poke((uint64_t)m + 4096u, &y, 1) == 0,
           "the parent sees the child's store");
    kf_set_current(child);
    expect(mem_is((uint64_t)m + 4096u, 'p', 1), "and the child the parent's, made after the fork");
    kf_set_current(parent);
    expect(vibeos_mm_stats()->fork_kept_shared == 2u, "the fork handed on two pages as they were");
    /* A change of protection is not a change of kind. */
    expect(SYS3(10, (uint64_t)m, 4096, 1) == 0 && kf_poke((uint64_t)m, &x, 1) != 0 &&
           SYS3(10, (uint64_t)m, 4096, 3) == 0,
           "read-only, a shared page refuses a store instead of copying");
    pid = SYS0(57);
    child = slot_of_pid(pid);
    kf_set_current(child);
    y = 'd';
    (void)kf_poke((uint64_t)m, &y, 1);
    kf_set_current(parent);
    expect(mem_is((uint64_t)m, 'd', 1), "and after mprotect it is still shared across a fork");

    /* A file: the mapping is the file. */
    for (i = 0; i < 5000u; i++) {
        ((uint8_t *)kf_uptr(buf))[i] = PAT(i);
    }
    fd = tmp_open("/tmp/s", 0x42 /* O_CREAT|O_RDWR */, 0644);
    expect(SYS3(1, (uint64_t)fd, buf, 5000) == 5000, "a file of 5000 bytes");
    m = MMAP(0, 12288, 3, MAP_SH, fd, 0);
    expect(m > 0 && kf_peek((uint64_t)m + 4999u, &y, 1) == 0 && y == PAT(4999u) &&
           kf_peek((uint64_t)m + 613u, &y, 1) == 0 && y == PAT(613u),
           "a shared mapping of a file holds the file's bytes");
    expect(mem_is((uint64_t)m + 5000u, 0, 8192 - 5000), "and zeros to the end of its last page");
    expect(kf_peek((uint64_t)m + 8192u, &y, 1) != 0, "a page past the file's last is not there");
    x = 'S';
    expect(kf_poke((uint64_t)m + 10u, &x, 1) == 0 && SYS4(17, (uint64_t)fd, buf, 1, 10) == 1 &&
           ((uint8_t *)kf_uptr(buf))[0] == 'S', "a store through the mapping is read from the file");
    ((uint8_t *)kf_uptr(buf))[0] = 'W';
    expect(SYS4(18, (uint64_t)fd, buf, 1, 4100) == 1 && mem_is((uint64_t)m + 4100u, 'W', 1),
           "a write to the file is seen through the mapping");
    m2 = MMAP(0, 4096, 1, MAP_SH, fd, 4096);
    expect(m2 > 0 && mem_is((uint64_t)m2 + 4u, 'W', 1) && kf_poke((uint64_t)m2, &x, 1) != 0,
           "a second mapping, at an offset and read-only, is the same page");
    pid = SYS0(57);
    child = slot_of_pid(pid);
    kf_set_current(child);
    y = 'k';
    expect(kf_poke((uint64_t)m + 20u, &y, 1) == 0, "a forked child stores through it");
    kf_set_current(parent);
    expect(mem_is((uint64_t)m + 20u, 'k', 1) && SYS4(17, (uint64_t)fd, buf, 1, 20) == 1 &&
           ((uint8_t *)kf_uptr(buf))[0] == 'k', "and the parent and the file both have it");
    expect(SYS3(26, (uint64_t)m, 8192, 4 /* MS_SYNC */) == 0 && SYS3(26, (uint64_t)m, 100, 1) == 0,
           "msync has nothing left to write");
    expect(SYS3(26, (uint64_t)m + 1u, 4096, 4) == -VIBEOS_EINVAL &&
           SYS3(26, (uint64_t)m, 4096, 5 /* ASYNC|SYNC */) == -VIBEOS_EINVAL &&
           SYS3(26, (uint64_t)m, 4096, 8) == -VIBEOS_EINVAL,
           "msync refuses an unaligned address and flags that make no sense");
    expect(SYS2(11, (uint64_t)m + 4096u, 4096) == 0 &&
           SYS3(26, (uint64_t)m, 8192, 4) == -VIBEOS_ENOMEM,
           "and a range with a hole in it is ENOMEM");
    expect(SYS1(3, (uint64_t)fd) == 0 && mem_is((uint64_t)m + 20u, 'k', 1),
           "the mapping outlives the descriptor");

    /* A hole is given a page; a truncate under a mapping does not fault it. */
    fd = tmp_open("/tmp/h", 0x42, 0644);
    expect(SYS2(77, (uint64_t)fd, 8192) == 0 && (m = MMAP(0, 8192, 3, MAP_SH, fd, 0)) > 0 &&
           kf_poke((uint64_t)m + 4096u, &x, 1) == 0 && SYS4(17, (uint64_t)fd, buf, 1, 4096) == 1 &&
           ((uint8_t *)kf_uptr(buf))[0] == 'S', "a hole in the file is given a page to share");
    expect(SYS2(77, (uint64_t)fd, 0) == 0 && mem_is((uint64_t)m + 4096u, 'S', 1),
           "a file cut short under a mapping leaves the mapping its page");
    expect(SYS2(11, (uint64_t)m, 8192) == 0, "unmapped");
    /* The same again, twice, and nothing is left behind the second time: the
     * page tables are in place after the first. */
    (void)SYS2(77, (uint64_t)fd, 8192);
    m = MMAP(0, 8192, 3, MAP_SH, fd, 0);
    (void)kf_poke((uint64_t)m, &x, 1);
    (void)SYS2(11, (uint64_t)m, 8192);
    (void)SYS2(77, (uint64_t)fd, 0);
    free1 = vibeos_frame_free_count();
    (void)SYS2(77, (uint64_t)fd, 8192);
    m2 = MMAP((uint64_t)m, 8192, 3, MAP_SH | 0x10 /* MAP_FIXED */, fd, 0);
    (void)kf_poke((uint64_t)m2, &x, 1);
    (void)SYS2(77, (uint64_t)fd, 0);         /* the file lets go first */
    (void)SYS2(11, (uint64_t)m2, 8192);      /* then the mapping */
    free2 = vibeos_frame_free_count();
    expect(m2 == m && free2 == free1, "a shared page is freed when its last holder lets go, once");

    /* What is refused. */
    rfd = tmp_open("/tmp/s", 0, 0);
    expect(MMAP(0, 4096, 3, MAP_SH, rfd, 0) == -VIBEOS_EACCES,
           "shared and writable needs a descriptor that can write");
    m = MMAP(0, 4096, 1, MAP_SH, rfd, 0);
    expect(m > 0 && mem_is((uint64_t)m + 10u, 'S', 1) && kf_poke((uint64_t)m, &x, 1) != 0,
           "shared and read-only does not");
    expect(MMAP(0, 4096, 3, 2 /* MAP_PRIVATE */, rfd, 0) > 0,
           "nor does private and writable: its stores stay in the process");
    kf_fs_add("/g", "root file", 9, 0);
    expect(MMAP(0, 4096, 1, MAP_SH, SYS2(2, ustr("/g"), 0), 0) == -VIBEOS_ENODEV,
           "a filesystem with no pages to share says ENODEV");
    expect(kf_lock_imbalance() == 0, "shared mappings released every lock they took");
}

/* ---- L3 step 4: madvise, mincore, mlock, mremap, memfd_create ------------------------ */

#define PTE_FRAME 0x000FFFFFFFFFF000ull

static uint64_t entry_of(int slot, uint64_t va) {
    vibeos_vmspace_t v = ks_vm(slot);
    uint64_t *e = vibeos_vmspace_entry(&v, va);
    return e ? *e : 0u;
}

static void t_memory_calls(void) {
    uint64_t vec, name, buf;
    uint8_t x = 'A', y = 0, *vp;
    long m, m2, s, blk, fd, pid, r;
    int parent, child;
    uint64_t frame, free1, refused;
    vibeos_vmspace_t v;
    linux_stat_t st;
    char path[40];

    parent = fresh(201);
    vec = kf_ualloc(16);
    buf = kf_ualloc(64);
    vp = (uint8_t *)kf_uptr(vec);

    /* madvise */
    m = MMAP(0, 8192, 3, MAP_PRIV_ANON, -1, 0);
    (void)kf_poke((uint64_t)m, &x, 1);
    (void)kf_poke((uint64_t)m + 4096u, &x, 1);
    pid = SYS0(57);
    child = slot_of_pid(pid);
    kf_set_current(parent);
    expect(SYS3(28, (uint64_t)m, 4096, 4 /* MADV_DONTNEED */) == 0 && mem_is((uint64_t)m, 0, 4096) &&
           mem_is((uint64_t)m + 4096u, 'A', 1), "MADV_DONTNEED gives the page back: it reads zeros, its neighbour does not");
    expect(kf_poke((uint64_t)m, &x, 1) == 0 && mem_is((uint64_t)m, 'A', 1), "and it is still the program's to write");
    kf_set_current(child);
    expect(mem_is((uint64_t)m, 'A', 1), "a child that shared the page keeps what was in it");
    kf_set_current(parent);
    (void)SYS3(28, (uint64_t)m, 8192, 4);
    free1 = vibeos_frame_free_count();
    (void)kf_poke((uint64_t)m, &x, 1);
    expect(SYS3(28, (uint64_t)m, 8192, 8 /* MADV_FREE */) == 0 && mem_is((uint64_t)m, 0, 1) &&
           vibeos_frame_free_count() == free1, "MADV_FREE does the same, and a page is given back for each one taken");
    expect(SYS3(28, (uint64_t)m, 8192, 0) == 0 && SYS3(28, (uint64_t)m, 8192, 3) == 0 &&
           SYS3(28, (uint64_t)m, 8192, 21) == 0 && SYS3(28, (uint64_t)m, 0, 4) == 0,
           "advice about speed is taken by doing nothing");
    expect(SYS3(28, (uint64_t)m, 8192, 10 /* MADV_DONTFORK */) == -VIBEOS_EINVAL &&
           SYS3(28, (uint64_t)m, 8192, 99) == -VIBEOS_EINVAL && SYS3(28, (uint64_t)m + 1u, 4096, 4) == -VIBEOS_EINVAL,
           "advice this kernel cannot keep, advice nobody has, and an unaligned address are EINVAL");
    expect(SYS3(28, (uint64_t)m, 3 * 4096, 0) == -VIBEOS_ENOMEM, "a range with a hole in it is ENOMEM");
    s = MMAP(0, 4096, 3, MAP_SH_ANON, -1, 0);
    (void)kf_poke((uint64_t)s, &x, 1);
    expect(SYS3(28, (uint64_t)s, 4096, 4) == 0 && mem_is((uint64_t)s, 'A', 1) &&
           SYS3(28, (uint64_t)s, 4096, 8) == -VIBEOS_EINVAL,
           "a shared page is not this process's to discard, and MADV_FREE of one is EINVAL");

    /* mincore */
    vp[0] = vp[1] = vp[2] = 9;
    expect(SYS3(27, (uint64_t)m, 8192, vec) == 0 && vp[0] == 1 && vp[1] == 1 && vp[2] == 9,
           "mincore writes a byte a page, and no more");
    v = ks_vm(parent);
    expect(vibeos_vmspace_swap_out(&v, (uint64_t)m + 4096u, 7u) == 0 && SYS3(27, (uint64_t)m, 8192, vec) == 0 &&
           vp[0] == 1 && vp[1] == 0, "a page in swap is mapped and not in memory");
    expect(SYS3(27, (uint64_t)m + 1u, 4096, vec) == -VIBEOS_EINVAL &&
           SYS3(27, 0x30000000ull, 4096, vec) == -VIBEOS_ENOMEM &&
           SYS3(27, (uint64_t)m, 4096, 0x7000000000ull) == -VIBEOS_EFAULT,
           "mincore: an unaligned address, a hole, a vector that is nobody's");
    /* The fake has no swap to read back from: a locked page must be here, and
     * this one cannot be brought. */
    expect(SYS2(149, (uint64_t)m + 4096u, 1) == -VIBEOS_EAGAIN, "mlock of a page that cannot be brought back is EAGAIN");
    (void)SYS2(11, (uint64_t)m + 4096u, 4096);

    /* mlock */
    refused = vibeos_mm_stats()->swap_refused_locked;
    expect(SYS2(149, (uint64_t)m + 100u, 10) == 0 && (entry_of(parent, (uint64_t)m) & VIBEOS_PTE_LOCKED) &&
           vibeos_vmspace_swap_out(&v, (uint64_t)m, 8u) != 0 &&
           vibeos_mm_stats()->swap_refused_locked == refused + 1u,
           "mlock locks the pages its bytes fall in, and reclaim leaves them");
    expect(SYS3(28, (uint64_t)m, 4096, 4) == -VIBEOS_EINVAL, "a locked page is not discarded");
    pid = SYS0(57);
    child = slot_of_pid(pid);
    kf_set_current(parent);
    expect(!(entry_of(child, (uint64_t)m) & VIBEOS_PTE_LOCKED), "a lock is not inherited across fork");
    expect(kf_poke((uint64_t)m, &x, 1) == 0 && (entry_of(parent, (uint64_t)m) & VIBEOS_PTE_LOCKED),
           "the copy a write makes after the fork is still locked");
    expect(SYS3(10, (uint64_t)m, 4096, 1) == 0 && (entry_of(parent, (uint64_t)m) & VIBEOS_PTE_LOCKED),
           "and so is the page after mprotect");
    expect(SYS2(150, (uint64_t)m, 4096) == 0 && !(entry_of(parent, (uint64_t)m) & VIBEOS_PTE_LOCKED),
           "munlock lets it go");
    expect(SYS2(149, (uint64_t)m, 2 * 4096) == -VIBEOS_ENOMEM && SYS2(149, (uint64_t)m, 0) == 0 &&
           SYS3(325, (uint64_t)m, 4096, 2) == -VIBEOS_EINVAL && SYS3(325, (uint64_t)m, 4096, 1) == 0,
           "mlock: a hole is ENOMEM, nothing is nothing, mlock2 takes MLOCK_ONFAULT and no other flag");
    expect(SYS1(151, 1 /* MCL_CURRENT */) == 0 && (entry_of(parent, (uint64_t)s) & VIBEOS_PTE_LOCKED) &&
           SYS0(152) == 0 && !(entry_of(parent, (uint64_t)s) & VIBEOS_PTE_LOCKED) &&
           !(entry_of(parent, (uint64_t)m) & VIBEOS_PTE_LOCKED),
           "mlockall locks every mapping and munlockall lets them all go");
    expect(SYS2(149, (uint64_t)s, 4096) == 0 && SYS3(26, (uint64_t)s, 4096, 2 /* MS_INVALIDATE */) == -VIBEOS_EBUSY &&
           SYS2(150, (uint64_t)s, 4096) == 0 && SYS3(26, (uint64_t)s, 4096, 2) == 0,
           "msync(MS_INVALIDATE) of a locked page is EBUSY");
    expect(MMAP(0, 4096, 3, 0x23 /* SHARED_VALIDATE|ANONYMOUS */, -1, 0) > 0 &&
           MMAP(0, 4096, 3, 0x23 | 0x400, -1, 0) == -VIBEOS_EOPNOTSUPP &&
           MMAP(0, 4096, 3, 0x22 | 0x400, -1, 0) > 0,
           "MAP_SHARED_VALIDATE refuses a flag nobody knows, and the other kinds ignore it");
    m2 = MMAP(0, 4096, 3, MAP_PRIV_ANON | 0x2000 /* MAP_LOCKED */, -1, 0);
    expect(m2 > 0 && (entry_of(parent, (uint64_t)m2) & VIBEOS_PTE_LOCKED) &&
           !(entry_of(parent, (uint64_t)m) & VIBEOS_PTE_LOCKED), "MAP_LOCKED is mlock at the time of the mapping");
    expect(SYS1(151, 0) == -VIBEOS_EINVAL && SYS1(151, 8) == -VIBEOS_EINVAL && SYS1(151, 4) == -VIBEOS_EINVAL,
           "mlockall: no flags, an unknown one, and MCL_ONFAULT by itself are EINVAL");

    /* mremap */
    parent = fresh(202);
    m = MMAP(0, 8192, 3, MAP_PRIV_ANON, -1, 0);
    (void)kf_poke((uint64_t)m, &x, 1);
    y = 'B';
    (void)kf_poke((uint64_t)m + 4096u, &y, 1);
    expect(sys(25, (uint64_t)m, 8192, 4096, 0, 0, 0, 0) == m && mem_is((uint64_t)m, 'A', 1) &&
           kf_peek((uint64_t)m + 4096u, &y, 1) != 0, "mremap to a smaller size gives the tail back");
    expect(sys(25, (uint64_t)m, 4096, 8192, 0, 0, 0, 0) == m && mem_is((uint64_t)m, 'A', 1) &&
           mem_is((uint64_t)m + 4096u, 0, 4096), "to a larger one, with room after it, it grows where it stands");
    blk = MMAP((uint64_t)m + 8192u, 4096, 3, MAP_PRIV_ANON | 0x10 /* MAP_FIXED */, -1, 0);
    expect(blk == m + 8192 && sys(25, (uint64_t)m, 8192, 16384, 0, 0, 0, 0) == -VIBEOS_ENOMEM,
           "with something in the way and no leave to move, ENOMEM");
    (void)SYS2(149, (uint64_t)m, 4096);
    frame = entry_of(parent, (uint64_t)m) & PTE_FRAME;
    m2 = sys(25, (uint64_t)m, 8192, 16384, 1 /* MREMAP_MAYMOVE */, 0, 0, 0);
    expect(m2 > 0 && m2 != m && mem_is((uint64_t)m2, 'A', 1) && mem_is((uint64_t)m2 + 8192u, 0, 8192),
           "with leave to move, the mapping goes where there is room, contents first, zeros after");
    expect((entry_of(parent, (uint64_t)m2) & PTE_FRAME) == frame && (entry_of(parent, (uint64_t)m2) & VIBEOS_PTE_LOCKED),
           "the same frame at the new address, still locked: moved, not copied");
    expect(kf_peek((uint64_t)m, &y, 1) != 0 && SYS3(26, (uint64_t)m, 4096, 4) == -VIBEOS_ENOMEM &&
           SYS3(26, (uint64_t)m2, 16384, 4) == 0 && mem_is((uint64_t)blk, 0, 1),
           "the old range is unmapped and undescribed, the new one described, the neighbour untouched");
    expect(sys(25, (uint64_t)m2, 4096, 4096, 3 /* MAYMOVE|FIXED */, (uint64_t)blk, 0, 0) == blk &&
           mem_is((uint64_t)blk, 'A', 1) && kf_peek((uint64_t)m2, &y, 1) != 0,
           "MREMAP_FIXED moves it to the address named, over what was there");
    expect(sys(25, (uint64_t)blk, 4096, 8192, 3, (uint64_t)blk, 0, 0) == -VIBEOS_EINVAL &&
           sys(25, (uint64_t)blk, 4096, 4096, 2, (uint64_t)m, 0, 0) == -VIBEOS_EINVAL &&
           sys(25, (uint64_t)blk, 0, 4096, 1, 0, 0, 0) == -VIBEOS_EINVAL &&
           sys(25, (uint64_t)blk, 4096, 0, 1, 0, 0, 0) == -VIBEOS_EINVAL &&
           sys(25, (uint64_t)blk, 4096, 4096, 4, 0, 0, 0) == -VIBEOS_EINVAL &&
           sys(25, (uint64_t)blk + 1u, 4096, 4096, 1, 0, 0, 0) == -VIBEOS_EINVAL,
           "mremap refuses a target over its source, FIXED alone, a zero length, an unknown flag, an unaligned address");
    (void)SYS3(10, (uint64_t)m2 + 8192u, 4096, 1);
    expect(sys(25, (uint64_t)m2 + 4096u, 8192, 4096, 0, 0, 0, 0) == -VIBEOS_EFAULT &&
           sys(25, 0x30000000ull, 4096, 8192, 1, 0, 0, 0) == -VIBEOS_EFAULT,
           "a range across two mappings, or in none, is EFAULT");
    s = MMAP(0, 4096, 3, MAP_SH_ANON, -1, 0);
    (void)kf_poke((uint64_t)s, &x, 1);
    pid = SYS0(57);
    child = slot_of_pid(pid);
    kf_set_current(parent);
    expect(sys(25, (uint64_t)s, 4096, 8192, 1, 0, 0, 0) == -VIBEOS_ENOMEM, "a shared mapping does not grow");
    m = sys(25, (uint64_t)s, 4096, 4096, 3, (uint64_t)m2, 0, 0);
    kf_set_current(child);
    y = 'c';
    (void)kf_poke((uint64_t)s, &y, 1);
    kf_set_current(parent);
    expect(m == m2 && mem_is((uint64_t)m, 'c', 1), "moved, a shared page is still the one the child has");

    /* memfd_create */
    parent = fresh(203);
    buf = kf_ualloc(64);
    name = kf_ualloc(300);
    memset(kf_uptr(name), 'n', 250);
    fd = SYS2(319, ustr("results"), 1 /* MFD_CLOEXEC */);
    expect(fd >= 0 && SYS3(72, (uint64_t)fd, 1 /* F_GETFD */, 0) == 1 && SYS2(77, (uint64_t)fd, 4096) == 0,
           "memfd_create gives a descriptor, close-on-exec when asked, to a file that can be sized");
    snprintf(path, sizeof(path), "/tmp/.memfd-%u", 1u);
    m = MMAP(0, 4096, 3, MAP_SH, fd, 0);
    expect(m > 0 && kf_poke((uint64_t)m + 5u, &x, 1) == 0 && SYS4(17, (uint64_t)fd, buf, 1, 5) == 1 &&
           ((uint8_t *)kf_uptr(buf))[0] == 'A', "mapped shared, it is memory with a file's interface");
    r = tmp_stat(path, 1, &st);
    expect(r == 0 && SYS1(3, (uint64_t)fd) == 0 && tmp_stat(path, 1, &st) == -VIBEOS_ENOENT &&
           mem_is((uint64_t)m + 5u, 'A', 1), "its name goes with the last descriptor, and the mapping keeps the page");
    expect(SYS2(319, ustr("x"), 4) == -VIBEOS_EINVAL && SYS2(319, name, 0) == -VIBEOS_EINVAL &&
           SYS2(319, 0x7000000000ull, 0) == -VIBEOS_EFAULT && SYS2(319, ustr(""), 2 /* ALLOW_SEALING */) >= 0,
           "memfd_create refuses an unknown flag, a name too long and a name that is nobody's");
    expect(kf_lock_imbalance() == 0, "the memory calls released every lock they took");
}

/* ---- L2 step 1: credentials ------------------------------------------------------------ */

#define KEEP ((uint64_t)(uint32_t)-1)

/* Fork, and make the child the caller. */
static int become_child(void) {
    long pid = SYS0(57);
    int child = slot_of_pid(pid);
    kf_set_current(child);
    return child;
}

static uint32_t mode_of(const char *path) {
    linux_stat_t st;
    return tmp_stat(path, 1, &st) == 0 ? (st.st_mode & 07777u) : ~0u;
}

static void t_credentials(void) {
    uint64_t ids = kf_ualloc(16), list = kf_ualloc(16), buf = kf_ualloc(8);
    uint32_t *idp, *lp;
    linux_stat_t st;
    int root, user;
    long fd;

    root = fresh(211);
    ids = kf_ualloc(16);
    list = kf_ualloc(16);
    buf = kf_ualloc(8);
    idp = (uint32_t *)kf_uptr(ids);
    lp = (uint32_t *)kf_uptr(list);

    /* What root leaves for somebody else to meet. */
    (void)SYS2(83, ustr("/tmp/c"), 0755);
    (void)SYS2(83, ustr("/tmp/c/priv"), 0700);
    (void)SYS2(83, ustr("/tmp/c/open"), 0777);
    (void)SYS2(90, ustr("/tmp/c/open"), 0777);
    (void)SYS1(3, (uint64_t)tmp_open("/tmp/c/rootfile", 0x41, 0600));
    (void)SYS2(90, ustr("/tmp/c/rootfile"), 0600);
    fd = tmp_open("/tmp/c/pub", 0x41, 0644);
    memcpy(kf_uptr(buf), "data", 4);
    (void)SYS3(1, (uint64_t)fd, buf, 4);
    (void)SYS1(3, (uint64_t)fd);
    (void)SYS2(90, ustr("/tmp/c/pub"), 0644);
    (void)SYS1(3, (uint64_t)tmp_open("/tmp/c/priv/x", 0x41, 0644));
    (void)SYS1(3, (uint64_t)tmp_open("/tmp/rootsticky", 0x41, 0644));
    (void)SYS2(90, ustr("/tmp/rootsticky"), 0666);

    expect(SYS0(102) == 0 && SYS0(107) == 0 && SYS0(104) == 0 && SYS0(108) == 0, "a process starts as root");
    lp[0] = 100;
    lp[1] = 200;
    expect(SYS2(116, 2, list) == 0 && SYS2(115, 0, 0) == 2 && SYS2(115, 1, list) == -VIBEOS_EINVAL,
           "setgroups by root; getgroups(0) counts them, and a list too short is EINVAL");
    lp[0] = lp[1] = 0;
    expect(SYS2(115, 2, list) == 2 && lp[0] == 100 && lp[1] == 200, "getgroups reads them back");
    expect(SYS1(122, KEEP) == 0 && SYS1(122, KEEP) == 0 && SYS1(123, KEEP) == 0 && SYS1(123, KEEP) == 0,
           "setfsuid(-1) and setfsgid(-1) ask, even root's: -1 is never an id (L2 step 7, setfsuid02)");

    /* For good: root's setgid and setuid set every id, and there is no way back. */
    user = become_child();
    expect(SYS1(106, 100) == 0 && SYS1(105, 1000) == 0 && SYS0(102) == 1000 && SYS0(107) == 1000 &&
           SYS0(104) == 100 && SYS0(108) == 100, "root becomes user 1000, group 100");
    expect(SYS3(118, ids, ids + 4u, ids + 8u) == 0 && idp[0] == 1000 && idp[1] == 1000 && idp[2] == 1000 &&
           SYS3(120, ids, ids + 4u, ids + 8u) == 0 && idp[0] == 100 && idp[2] == 100,
           "getresuid and getresgid: real, effective and saved all changed");
    expect(SYS1(105, 0) == -VIBEOS_EPERM && SYS2(113, KEEP, 0) == -VIBEOS_EPERM &&
           SYS3(117, 0, KEEP, KEEP) == -VIBEOS_EPERM && SYS1(106, 0) == -VIBEOS_EPERM &&
           SYS2(116, 0, 0) == -VIBEOS_EPERM && SYS0(107) == 1000,
           "and cannot become root again by any of the calls");
    expect(SYS1(122, 0) == 1000 && SYS1(122, KEEP) == 1000 && SYS1(123, 200) == 100 && SYS1(123, KEEP) == 100,
           "setfsuid and setfsgid answer the old id and refuse one the process does not hold");

    /* Reading and writing, by the file's bits. */
    expect(tmp_open("/tmp/c/rootfile", 0, 0) == -VIBEOS_EACCES && (fd = tmp_open("/tmp/c/pub", 0, 0)) >= 0 &&
           tmp_open("/tmp/c/pub", 1, 0) == -VIBEOS_EACCES, "a file is opened as its bits allow this user");
    expect(tmp_open("/tmp/c/pub", 0x200 /* O_TRUNC, read-only */, 0) == -VIBEOS_EACCES && tmp_size("/tmp/c/pub") == 4,
           "an open that is refused has not truncated the file");
    expect(SYS2(76, ustr("/tmp/c/pub"), 0) == -VIBEOS_EACCES && tmp_size("/tmp/c/pub") == 4,
           "nor does truncate cut a file the user may not write");
    expect(tmp_open("/tmp/c/priv/x", 0, 0) == -VIBEOS_EACCES && tmp_stat("/tmp/c/priv/x", 1, &st) == -VIBEOS_EACCES &&
           SYS1(80, ustr("/tmp/c/priv")) == -VIBEOS_EACCES,
           "a directory the user may not search hides what is in it, and is not one to work in");

    /* Names: the directory's permission. */
    expect(tmp_open("/tmp/c/new", 0x41, 0644) == -VIBEOS_EACCES && SYS2(83, ustr("/tmp/c/d"), 0755) == -VIBEOS_EACCES &&
           SYS1(87, ustr("/tmp/c/pub")) == -VIBEOS_EACCES &&
           SYS2(82, ustr("/tmp/c/pub"), ustr("/tmp/c/open/pub")) == -VIBEOS_EACCES &&
           SYS2(88, ustr("x"), ustr("/tmp/c/l")) == -VIBEOS_EACCES &&
           SYS2(86, ustr("/tmp/c/pub"), ustr("/tmp/c/hard")) == -VIBEOS_EACCES,
           "a name is not made, taken away or moved in a directory the user may not write");
    fd = tmp_open("/tmp/c/open/mine", 0x42, 0640);
    expect(fd >= 0 && tmp_stat("/tmp/c/open/mine", 1, &st) == 0 && st.st_uid == 1000 && st.st_gid == 100 &&
           SYS2(83, ustr("/tmp/c/open/d"), 0755) == 0 && tmp_stat("/tmp/c/open/d", 1, &st) == 0 && st.st_uid == 1000,
           "what the user makes where it may is the user's");
    expect(SYS1(87, ustr("/tmp/rootsticky")) == -VIBEOS_EPERM &&
           SYS2(82, ustr("/tmp/rootsticky"), ustr("/tmp/stolen")) == -VIBEOS_EPERM &&
           SYS1(3, (uint64_t)tmp_open("/tmp/mine", 0x41, 0644)) == 0 && SYS1(87, ustr("/tmp/mine")) == 0,
           "in a sticky directory a name goes only for the file's owner");

    /* Attributes: the owner's. */
    expect(SYS2(90, ustr("/tmp/c/pub"), 0666) == -VIBEOS_EPERM && mode_of("/tmp/c/pub") == 0644u &&
           SYS2(90, ustr("/tmp/c/open/mine"), 0600) == 0 && mode_of("/tmp/c/open/mine") == 0600u,
           "chmod is the owner's");
    expect(SYS3(92, ustr("/tmp/c/open/mine"), 0, KEEP) == -VIBEOS_EPERM &&
           SYS3(92, ustr("/tmp/c/open/mine"), KEEP, 300) == -VIBEOS_EPERM &&
           SYS3(92, ustr("/tmp/c/open/mine"), KEEP, 200) == 0 &&
           tmp_stat("/tmp/c/open/mine", 1, &st) == 0 && st.st_gid == 200 && st.st_uid == 1000,
           "chown: a file is not given away, and its group only to one of the owner's own");
    ((int64_t *)kf_uptr(ids))[0] = 5;
    ((int64_t *)kf_uptr(ids))[1] = 0;
    expect(sys(280, (uint64_t)(uint32_t)-100, ustr("/tmp/c/pub"), 0, 0, 0, 0, 0) == -VIBEOS_EACCES &&
           sys(280, (uint64_t)(uint32_t)-100, ustr("/tmp/rootsticky"), 0, 0, 0, 0, 0) == 0 &&
           SYS2(132 /* utime */, ustr("/tmp/rootsticky"), ids) == -VIBEOS_EPERM,
           "a file's times: 'now' for whoever may write it, a chosen time for its owner alone");

    /* One class decides: the owner's bits for the owner, even where everybody
     * else would be let in. */
    expect(SYS2(90, ustr("/tmp/c/open/mine"), 0066) == 0 && tmp_open("/tmp/c/open/mine", 0, 0) == -VIBEOS_EACCES &&
           SYS2(90, ustr("/tmp/c/open/mine"), 0600) == 0 && SYS1(3, (uint64_t)tmp_open("/tmp/c/open/mine", 2, 0)) == 0,
           "an owner denied by the owner's bits is denied, whatever the others' say");

    /* access() answers for the real user. */
    expect(SYS2(21, ustr("/tmp/c/rootfile"), 4) == -VIBEOS_EACCES && SYS2(21, ustr("/tmp/c/pub"), 4) == 0 &&
           SYS2(21, ustr("/tmp/c/pub"), 2) == -VIBEOS_EACCES && SYS2(21, ustr("/tmp/c/pub"), 0) == 0,
           "access reports what this user may do");

    /* Signals: the superuser's process is not this user's to signal. */
    expect(SYS2(62, 211, 15) == -VIBEOS_EPERM, "a user may not signal root's process");
    kf_set_current(root);
    expect(SYS2(62, (uint64_t)ks_id(user)->pid, 0) == 0, "root may signal anybody");

    /* Borrowed privileges: real 1000, effective root, and the way back kept. */
    user = become_child();
    expect(SYS3(117, 1000, 1000, 0) == 0 && SYS0(102) == 1000 && SYS0(107) == 1000 &&
           tmp_open("/tmp/c/rootfile", 0, 0) == -VIBEOS_EACCES, "setresuid(1000, 1000, 0): a user, with root saved");
    expect(SYS3(117, KEEP, 0, KEEP) == 0 && SYS0(107) == 0 && tmp_open("/tmp/c/rootfile", 0, 0) >= 0,
           "the saved id is the way back: effective root opens the file");
    expect(SYS2(21, ustr("/tmp/c/rootfile"), 4) == -VIBEOS_EACCES &&
           sys(439, (uint64_t)(uint32_t)-100, ustr("/tmp/c/rootfile"), 4, 0x200 /* AT_EACCESS */, 0, 0, 0) == 0,
           "access still answers for the real user, and AT_EACCESS for the effective one");

    /* setreuid's rule for the saved id. */
    kf_set_current(root);
    user = become_child();
    expect(SYS2(113, 1000, 2000) == 0 && SYS3(118, ids, ids + 4u, ids + 8u) == 0 && idp[0] == 1000 &&
           idp[1] == 2000 && idp[2] == 2000, "setreuid(1000, 2000) by root: the saved id follows the effective one");
    expect(SYS2(113, KEEP, 1000) == 0 && SYS3(118, ids, ids + 4u, ids + 8u) == 0 && idp[1] == 1000 && idp[2] == 2000 &&
           SYS2(113, KEEP, 2000) == 0 && SYS0(107) == 2000 && SYS2(113, KEEP, 3000) == -VIBEOS_EPERM,
           "an effective id set to the real one keeps the saved one, which is the way back");
    kf_set_current(root);
    expect(kf_lock_imbalance() == 0, "the credential calls released every lock they took");
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
    t_dup_shares_offset();
    t_many_descriptors();
    t_pipe_is_a_fifo();
    t_cloexec();
    t_fork_shares_offsets();
    t_working_directory();
    t_path_errors();
    t_at_calls();
    t_fork_inherits_cwd();
    t_open_flags();
    t_positional();
    t_truncate_and_sync();
    t_kernel_copies();
    t_root_filesystem_writes();
    t_rename();
    t_rmdir_and_links();
    t_metadata();
    t_mknod_and_openat2();
    t_record_locks();
    t_description_locks();
    t_directories();
    t_xattr_and_unshare();
    t_terminal_modes();
    t_descriptor_requests_and_poll();
    t_mmap_placement();
    t_mmap_files();
    t_mmap_shared();
    t_memory_calls();
    t_credentials();
    t_fork_snapshot_and_groups();
    t_procfs();
    t_sa_restart();
    t_signals_l2();
    t_timers();
    t_limits();
    t_processes();
    t_procdev();
    t_event_loops();
    t_event_fds();
    t_futex_shared();
    t_clone3_checks();
    t_sleep();
    t_ltp_l1();
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

    /* mknod (133) and mknodat (259), L1: a FIFO has a name. */
    fresh(77);
    gap(133, SYS3(133, ustr("/tmp/fifo"), 0010644, 0) == 0, "mknod makes a FIFO");
    gap(259, SYS4(259, (uint64_t)(uint32_t)-100, ustr("/tmp/fifo2"), 0010644, 0) == 0, "mknodat makes a FIFO");

    /* renameat2 (316), L1: RENAME_EXCHANGE swaps two names. */
    fresh(78);
    {
        long a = SYS3(2, ustr("/tmp/x1"), 0x41, 0644), b = SYS3(2, ustr("/tmp/x2"), 0x41, 0644);
        gap(316, a >= 0 && b >= 0 &&
                 sys(316, (uint64_t)(uint32_t)-100, ustr("/tmp/x1"), (uint64_t)(uint32_t)-100,
                     ustr("/tmp/x2"), 2 /* RENAME_EXCHANGE */, 0, 0) == 0,
            "renameat2(RENAME_EXCHANGE) swaps two names");
    }

    /* openat2 (437), L1: RESOLVE_BENEATH opens a path that stays beneath. */
    fresh(79);
    {
        uint64_t open_how = kf_ualloc(24);
        long made = SYS3(2, ustr("/tmp/b"), 0x41, 0644);
        memset(kf_uptr(open_how), 0, 24);
        ((uint64_t *)kf_uptr(open_how))[2] = 8;   /* RESOLVE_BENEATH */
        gap(437, made >= 0 && SYS4(437, (uint64_t)(uint32_t)-100, ustr("tmp/b"), open_how, 24) >= 0,
            "openat2 with RESOLVE_BENEATH");
    }

    /* close_range (436), L6: CLOSE_RANGE_UNSHARE gives a thread its own table. */
    {
        int me = fresh(76);
        ks_ps(me)->files_users = 2u;   /* another thread shares the table */
        gap(436, SYS3(436, 3, ~0ull, 2 /* CLOSE_RANGE_UNSHARE */) == 0,
            "close_range with CLOSE_RANGE_UNSHARE in a process with threads");
    }

    /* mremap (25), L3: a shared mapping grows. */
    fresh(62);
    {
        long at = sys(9, 0, 4096, 3, 0x21 /* SHARED|ANONYMOUS */, (uint64_t)-1, 0, 0);
        r = sys(25, (uint64_t)at, 4096, 8192, 1 /* MAYMOVE */, 0, 0, 0);
        gap(25, at > 0 && r > 0, "mremap growing a shared mapping");
    }

    /* madvise (28), L3: MADV_DONTNEED on a private page of a file shows the
     * file's bytes again. */
    fresh(63);
    {
        long fd = SYS3(2, ustr("/tmp/dn"), 0x42, 0644);
        uint64_t b = kf_ualloc(8);
        uint8_t c = 0;
        long at;
        memcpy(kf_uptr(b), "file", 4);
        (void)SYS3(1, (uint64_t)fd, b, 4);
        at = sys(9, 0, 4096, 3, 2 /* PRIVATE */, (uint64_t)fd, 0, 0);
        c = 'X';
        (void)kf_poke((uint64_t)at, &c, 1);
        (void)SYS3(28, (uint64_t)at, 4096, 4);
        (void)kf_peek((uint64_t)at, &c, 1);
        gap(28, at > 0 && c == 'f', "MADV_DONTNEED on a private page of a file brings the file back");
    }

    /* mlockall (151), L3: MCL_FUTURE locks what is mapped afterwards. */
    {
        int me = fresh(64);
        vibeos_vmspace_t as;
        uint64_t *e;
        long at;
        r = SYS1(151, 3 /* CURRENT|FUTURE */);
        at = sys(9, 0, 4096, 3, 0x22, (uint64_t)-1, 0, 0);
        as = ks_vm(me);
        e = vibeos_vmspace_entry(&as, (uint64_t)at);
        gap(151, r == 0 && e && (*e & VIBEOS_PTE_LOCKED), "mlockall(MCL_FUTURE) locking a later mapping");
    }

    /* memfd_create (319), L3: seals. */
    fresh(65);
    {
        long fd = SYS2(319, ustr("s"), 2 /* MFD_ALLOW_SEALING */);
        gap(319, fd >= 0 && SYS3(72, (uint64_t)fd, 1033 /* F_ADD_SEALS */, 8) == 0, "sealing a memfd");
    }

    /* mmap (9), L3: a shared mapping of a file on a filesystem that keeps its
     * files somewhere other than in pages - here the fake's root, in the kernel
     * FAT. */
    fresh(61);
    {
        long fd;
        kf_fs_add("/shared", "on the root", 11, 0);
        fd = SYS2(2, ustr("/shared"), 0);
        r = sys(9, 0, 4096, 1, 1 /* MAP_SHARED */, (uint64_t)fd, 0, 0);
        gap(9, fd >= 0 && r > 0, "a shared mapping of a file that is not kept in pages");
    }

    /* ioctl (16), L1: TCFLSH - throw away what was typed and not read - is
     * one of the terminal requests still unanswered. */
    fresh(72);
    gap(16, SYS3(16, 0, 0x540Bu /* TCFLSH */, 0) == 0, "TCFLSH on the console");

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

    /* pwritev2 (328): RWF_HIPRI is advice Linux accepts. */
    fresh(64);
    kf_fs_add("/f", "abcdef", 6, 0);
    {
        uint64_t b = kf_ualloc(16), iov = kf_ualloc(16);
        long fd = SYS2(2, ustr("/f"), 1);
        uint64_t *v = (uint64_t *)kf_uptr(iov);
        memcpy(kf_uptr(b), "XY", 2);
        v[0] = b; v[1] = 2;
        gap(328, sys(328, (uint64_t)fd, iov, 1, 1, 0, 1 /* RWF_HIPRI */, 0) == 2,
            "pwritev2 with a flag Linux accepts");
    }
    /* preadv2 (327): RWF_HIPRI is advice Linux accepts. */
    fresh(65);
    {
        uint64_t b = kf_ualloc(16), iov = kf_ualloc(16);
        long fd = SYS3(2, ustr("/tmp/g"), 0x42, 0644);
        uint64_t *v = (uint64_t *)kf_uptr(iov);
        v[0] = b; v[1] = 2;
        (void)SYS3(1, (uint64_t)fd, b, 2);
        gap(327, sys(327, (uint64_t)fd, iov, 1, 0, 0, 1 /* RWF_HIPRI */, 0) == 2, "preadv2 with RWF_HIPRI");
        /* fallocate (285): punching a hole. */
        gap(285, sys(285, (uint64_t)fd, 3 /* PUNCH_HOLE|KEEP_SIZE */, 0, 1, 0, 0, 0) == 0,
            "fallocate punching a hole");
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

    /* setgroups (116), L2: as many groups as Linux keeps. */
    fresh(68);
    {
        uint64_t many = kf_ualloc(40u * 4u);
        gap(116, SYS2(116, 40, many) == 0, "setgroups with forty groups");
    }

    /* times (100), L2: time spent in the kernel is system time. */
    {
        uint64_t tms;
        fresh(71);
        tms = kf_ualloc(32);
        kf_cpu(10, 0);
        (void)SYS1(100, tms);
        gap(100, ((linux_tms_t *)kf_uptr(tms))->tms_stime > 0, "times reports time spent in the kernel");
    }

    /* rt_sigqueueinfo (129) and rt_tgsigqueueinfo (297), L2: a real-time
     * signal queued twice is there twice for sigtimedwait to take. */
    {
        uint32_t n;
        for (n = 129; n <= 297; n += 168) {
            int me = fresh(69);
            uint64_t qi = kf_ualloc(128), set = kf_ualloc(8), ts = kf_ualloc(16);
            linux_siginfo_t *q = (linux_siginfo_t *)kf_uptr(qi);
            long first, second;

            q->si_code = LINUX_SI_QUEUE;
            ks_id(me)->sig_blocked = 1ull << 34;
            *(uint64_t *)kf_uptr(set) = 1ull << 33;   /* SIGRTMIN, Linux-numbered */
            if (n == 129) {
                (void)SYS3(129, 69, 34, qi);
                (void)SYS3(129, 69, 34, qi);
            } else {
                (void)SYS4(297, 69, 69, 34, qi);
                (void)SYS4(297, 69, 69, 34, qi);
            }
            first = sys(128, set, 0, ts, 8, 0, 0, 0);
            second = sys(128, set, 0, ts, 8, 0, 0, 0);
            gap(n, first == 34 && second == 34, "a real-time signal queued twice is taken twice");
        }
    }

    /* futex (202), L6: FUTEX_CMP_REQUEUE with nobody waiting requeues nobody. */
    {
        uint64_t w = 0;
        fresh(70);
        w = kf_ualloc(8);
        r = sys(202, w, 4 /* FUTEX_CMP_REQUEUE */, 1, 1, w + 4u, 0, 0);
        gap(202, r == 0, "FUTEX_CMP_REQUEUE");
    }

    /* setrlimit (160) and prlimit64 (302), L2: RLIMIT_AS is kept, not
     * enforced - a mapping larger than it still succeeds. */
    {
        uint64_t nl;
        uint32_t n;
        for (n = 160; n <= 302; n += 142) {
            fresh(73);
            nl = kf_ualloc(16);
            ((uint64_t *)kf_uptr(nl))[0] = 1ull << 20;
            ((uint64_t *)kf_uptr(nl))[1] = 1ull << 20;
            if (n == 160) {
                (void)SYS2(160, LINUX_RLIMIT_AS, nl);
            } else {
                (void)SYS4(302, 0, LINUX_RLIMIT_AS, nl, 0);
            }
            r = sys(9, 0, 4ull << 20, 3, 0x22, (uint64_t)-1, 0, 0);
            gap(n, r < 0, "a mapping larger than RLIMIT_AS is refused");
        }
    }

    /* waitid (247), L2: a stopped child is never reported. */
    {
        long pid;
        int me = fresh(77), c;
        uint64_t info = kf_ualloc(128);

        c = t_fork_child(&pid);
        kf_set_current(me);
        if (c >= 0) {
            ks_id(c)->signal_stopped = 1;
        }
        (void)sys(247, LINUX_P_PID, (uint64_t)pid, info, LINUX_WSTOPPED | 1u, 0, 0, 0);
        gap(247, ((linux_siginfo_t *)kf_uptr(info))->pid == (int32_t)pid, "waitid(WSTOPPED) reports a stopped child");
    }

    /* clone3 (435), L2: no process that shares its parent's memory. */
    {
        uint64_t args;
        fresh(78);
        args = kf_ualloc(sizeof(linux_clone_args_t));
        ((linux_clone_args_t *)kf_uptr(args))->flags = LINUX_CLONE_VM;
        ((linux_clone_args_t *)kf_uptr(args))->exit_signal = 17;
        gap(435, SYS2(435, args, sizeof(linux_clone_args_t)) != -VIBEOS_ENOSYS,
            "clone3 with CLONE_VM and no CLONE_THREAD makes a process");
    }

    /* getrusage (98), L2: no resident-set high-water mark. */
    {
        uint64_t ru;
        fresh(75);
        ru = kf_ualloc(sizeof(linux_rusage_t));
        (void)SYS2(98, LINUX_RUSAGE_SELF, ru);
        gap(98, ((linux_rusage_t *)kf_uptr(ru))->ru_maxrss > 0, "getrusage reports ru_maxrss");
    }

    /* personality (135), L2: kept, and changes nothing - PER_LINUX32 makes
     * Linux's uname say i686. */
    {
        uint64_t u;
        fresh(76);
        u = kf_ualloc(6u * 65u);
        (void)SYS1(135, 0x0008 /* PER_LINUX32 */);
        (void)SYS1(63, u);
        gap(135, strcmp((const char *)kf_uptr(u) + 4u * 65u, "i686") == 0,
            "PER_LINUX32 changes the machine uname reports");
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
