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
#include "vibeos/linux_layout.h"
#include "vibeos/vfs.h"
#include "vibeos/ksvc.h"
#include "vibeos/mm_stats.h"

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
    expect(SYS3(7, up, 1, 0) == 1 && p[0].revents == (1 | 0x10),
           "a pipe nobody can write any more is readable - end of file - and hung up");
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

    /* poll (7), L4: a socket with nothing to read is not readable. */
    {
        uint64_t pf, sa;
        long fd;
        fresh(80);
        kf_net_up(0x0A00020Fu, 0x0A000202u);
        pf = kf_ualloc(8);
        sa = kf_ualloc(16);
        fd = SYS2(41, 2, 2);
        ((uint8_t *)kf_uptr(sa))[0] = 2;
        ((uint8_t *)kf_uptr(sa))[2] = 0x10; ((uint8_t *)kf_uptr(sa))[3] = 0x93;
        (void)SYS2(49, (uint64_t)fd, sa);
        ((int32_t *)kf_uptr(pf))[0] = (int32_t)fd;
        ((int16_t *)kf_uptr(pf))[2] = 1;   /* POLLIN */
        ((int16_t *)kf_uptr(pf))[3] = 0;
        gap(7, fd >= 0 && SYS3(7, pf, 1, 0) == 0, "poll on a socket with nothing to read");
    }

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

    /* mmap (9), L3: a shared mapping of a file. */
    fresh(61);
    {
        long fd = SYS3(2, ustr("/tmp/shared"), 0x42, 0644);
        (void)SYS2(77, (uint64_t)fd, 4096);
        r = sys(9, 0, 4096, 3, 1 /* MAP_SHARED */, (uint64_t)fd, 0, 0);
        gap(9, fd >= 0 && r > 0, "a shared mapping of a file");
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
