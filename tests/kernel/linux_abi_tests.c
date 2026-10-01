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
    expect(SYS0(84) == -VIBEOS_ENOSYS && g_abi_unimplemented == before + 1u &&
           g_abi_last_nr == 84u, "rmdir is missing, counted and named");
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

#define SYS4(nr, a, b, c, d) sys((nr), (a), (b), (c), (d), 0, 0, 0)

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

    /* fcntl (72), L1: a record lock is taken. */
    {
        uint64_t fl = 0;
        long fd;
        fresh(75);
        kf_fs_add("/db", "x", 1, 0);
        fd = SYS2(2, ustr("db"), 0);
        fl = kf_ualloc(32);   /* struct flock: F_RDLCK over the whole file */
        gap(72, fd >= 0 && SYS3(72, (uint64_t)fd, 6 /* F_SETLK */, fl) == 0, "F_SETLK takes a lock");
    }

    /* unlinkat (263), L1: AT_REMOVEDIR removes an empty directory. */
    fresh(77);
    kf_fs_add("/empty", 0, 0, 1);
    gap(263, SYS3(263, (uint64_t)(uint32_t)-100, ustr("empty"), 0x200) == 0,
        "unlinkat(AT_REMOVEDIR) removes an empty directory");

    /* close_range (436), L1: CLOSE_RANGE_UNSHARE gives the caller its own table. */
    fresh(76);
    gap(436, SYS3(436, 3, ~0ull, 2 /* CLOSE_RANGE_UNSHARE */) == 0, "close_range with CLOSE_RANGE_UNSHARE");

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
