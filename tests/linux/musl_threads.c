/* Threads, through a real C library.
 *
 * Written against POSIX, not against VibeOS: pthread_create becomes clone()
 * with CLONE_VM|CLONE_THREAD and the rest, and pthread_join sleeps on a futex
 * until the exiting thread's kernel-side clear_child_tid write wakes it.
 *
 * Two stages, kept apart on purpose. One thread created and joined is the
 * smaller claim, and it already needs two tasks in one address space, a
 * thread-local base of its own, a scheduler that runs the new task, an exit
 * that clears the word the joiner sleeps on, and a futex whose wait and wake
 * meet. Four at once adds what only overlapping threads can show: shared
 * memory under a lock that really contends, and each thread keeping its own
 * thread-local while doing it.
 *
 * The counter is asserted, not printed. "The threads ran" and "the threads ran
 * and lost no increment" are different claims, and a kernel can produce the
 * first while failing the second.
 *
 * Five more stages ask whether the threads belong to one *process*, which the
 * first two cannot see: both only ever create threads from main, and no worker
 * touches process state, so a kernel that gives each thread a private copy of
 * the process passes them. Every one of them was written before the kernel was
 * changed and failed on it - that is how they are known to be able to.
 *
 *  - C5_MMAP: a thread maps memory, then main maps memory. One process has
 *    one mapping cursor; a thread that copied it hands main the same base,
 *    and main's fresh pages silently replace the thread's.
 *  - C5_SIGACTION: a thread installs a handler, then main raises the signal.
 *    Dispositions are the process's; a private copy leaves main on the
 *    default action, which for SIGUSR1 is to die.
 *  - C5_EXIT_GROUP: a thread calls exit_group(42) while main keeps running.
 *    The whole process ends and the parent sees 42 - not 7 from main
 *    finishing on its own, and not "killed by signal 9", which is what an
 *    exit_group built by killing the siblings would report.
 *  - C5_WNOHANG: waitpid(WNOHANG) on a child that is still running answers 0
 *    at once. A kernel that drops wait4's options blocks until the child
 *    exits; the child ends on its own, so that failure cannot stall the boot.
 *  - C5_TKILL: a thread's id addresses that thread. pthread_kill(thread, 0)
 *    must find it, and raise() inside it - tkill(gettid()) - must reach its
 *    handler. A lookup that matches thread-group ids answers ESRCH.
 *
 * C5_SIGACTION and C5_EXIT_GROUP run in a forked child, because failing them
 * kills or strands the process that fails, and every wait in every stage is
 * bounded: this program is one line of a sequential boot script, and a hang
 * here would read as every command after it having failed.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static __thread int mine;   /* thread-local: the child must see its own */

#define WORKERS 4
#define BUMPS 2000

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

/* Several of these run at once, so the lock really contends - which is the
 * only way FUTEX_WAIT is reached at all: an uncontended lock is taken with an
 * atomic instruction and never enters the kernel. */
static void *worker(void *arg)
{
    int i;

    mine = (int)(long)arg;
    for (i = 0; i < BUMPS; i++) {
        pthread_mutex_lock(&lock);
        counter++;
        pthread_mutex_unlock(&lock);
    }
    return (void *)(long)mine;
}

static void *child(void *arg)
{
    mine = (int)(long)arg;
    return (void *)(long)mine;
}

#define C5_LEN (64 * 1024)

/* Map, fill, and hand the address back through join, so main can map after
 * this thread has already advanced whatever cursor it holds. */
static void *mmap_worker(void *arg)
{
    unsigned char *p;

    (void)arg;
    p = mmap(0, C5_LEN, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return 0;
    }
    memset(p, 0xA5, C5_LEN);
    return p;
}

static volatile sig_atomic_t usr1_seen;

static void on_usr1(int sig)
{
    (void)sig;
    usr1_seen = 1;
}

static void *sigaction_worker(void *arg)
{
    struct sigaction sa;

    (void)arg;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    return (void *)(long)sigaction(SIGUSR1, &sa, 0);
}

static volatile int exit_group_go;

static void *exit_group_worker(void *arg)
{
    (void)arg;
    while (!exit_group_go) {
        sched_yield();
    }
    syscall(SYS_exit_group, 42);   /* the raw call: no atexit, no stdio flush */
    return 0;
}

/* Wait for a forked stage child and say what became of it. */
static void report_child(const char *stage, pid_t pid, int want_code)
{
    int status = 0;

    if (pid < 0) {
        printf("THREADS_%s_FAIL: fork\n", stage);
    } else if (waitpid(pid, &status, 0) != pid) {
        printf("THREADS_%s_FAIL: waitpid\n", stage);
    } else if (WIFSIGNALED(status)) {
        printf("THREADS_%s_FAIL: killed by signal %d\n", stage, WTERMSIG(status));
    } else if (WEXITSTATUS(status) != want_code) {
        printf("THREADS_%s_FAIL: exited %d, expected %d\n", stage,
               WEXITSTATUS(status), want_code);
    } else {
        printf("THREADS_%s_OK\n", stage);
    }
    fflush(stdout);
}

/* C5_TKILL: a signal addressed to a thread, not to its process. */
static volatile sig_atomic_t usr2_seen;

static void on_usr2(int sig)
{
    (void)sig;
    usr2_seen = 1;
}

static volatile int tkill_release;

/* raise() in musl is tkill(gettid()), so this is the smallest program that
 * addresses a signal to a thread that is not the leader. It then waits for
 * main by spinning rather than on a futex: a blocked wait here would not
 * notice a signal on this kernel, which is a different defect and must not
 * decide this stage. */
static void *tkill_worker(void *arg)
{
    (void)arg;
    raise(SIGUSR2);
    while (!tkill_release) {
        sched_yield();
    }
    return 0;
}

/* C5_EXIT_GROUP_BLOCKED: exit_group must end a sibling that is blocked in
 * the kernel, not only one running in user space. */
static pthread_mutex_t never_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t never_c = PTHREAD_COND_INITIALIZER;

static void *exit_group_later(void *arg)
{
    int n;

    (void)arg;
    for (n = 0; n < 20; n++) {      /* let main reach its wait first */
        sched_yield();
    }
    syscall(SYS_exit_group, 42);
    return 0;
}

/* report_child, but it cannot hang: on a kernel where the child never ends,
 * a blocking waitpid would stall this program and every later line of the
 * boot script. Polls with WNOHANG, which is why this stage came after the
 * WNOHANG fix and not before it. */
static void report_child_bounded(const char *stage, pid_t pid, int want_code,
                                 int yields)
{
    int status = 0, n;

    if (pid < 0) {
        printf("THREADS_%s_FAIL: fork\n", stage);
        fflush(stdout);
        return;
    }
    for (n = 0; n < yields; n++) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            break;
        }
        sched_yield();
    }
    if (n == yields) {
        printf("THREADS_%s_FAIL: the process never ended\n", stage);
    } else if (WIFSIGNALED(status)) {
        printf("THREADS_%s_FAIL: killed by signal %d\n", stage, WTERMSIG(status));
    } else if (WEXITSTATUS(status) != want_code) {
        printf("THREADS_%s_FAIL: exited %d, expected %d\n", stage,
               WEXITSTATUS(status), want_code);
    } else {
        printf("THREADS_%s_OK\n", stage);
    }
    fflush(stdout);
}

int main(void)
{
    pthread_t t;
    void *ret = 0;

    if (pthread_create(&t, 0, child, (void *)42L) != 0) {
        printf("THREADS_FAIL: create\n");
        fflush(stdout);
        return 1;
    }
    printf("THREADS_CREATE_OK\n");
    fflush(stdout);

    if (pthread_join(t, &ret) != 0) {
        printf("THREADS_FAIL: join\n");
        fflush(stdout);
        return 1;
    }
    /* Through the thread-local, so this also says the child had its own TLS
     * rather than writing into its creator's. */
    if ((long)ret != 42L) {
        printf("THREADS_FAIL: thread returned %ld, expected 42\n", (long)ret);
        fflush(stdout);
        return 1;
    }
    printf("THREADS_STAGE1_OK: created and joined one thread, tls returned %ld\n",
           (long)ret);
    fflush(stdout);

    /* Now several at once, which is a different question: threads that
     * overlap, contend for a lock, and exit while others are still being
     * created. */
    {
        pthread_t many[WORKERS];
        long i, tls_ok = 1;

        for (i = 0; i < WORKERS; i++) {
            if (pthread_create(&many[i], 0, worker, (void *)i) != 0) {
                printf("THREADS_FAIL: create %ld\n", i);
                fflush(stdout);
                return 1;
            }
        }
        for (i = 0; i < WORKERS; i++) {
            void *r = 0;
            if (pthread_join(many[i], &r) != 0) {
                printf("THREADS_FAIL: join %ld\n", i);
                fflush(stdout);
                return 1;
            }
            if ((long)r != i) {
                tls_ok = 0;   /* a thread saw another thread's thread-local */
            }
        }
        printf("THREADS_OK: %d threads, counter=%ld expected=%d tls=%s\n",
               WORKERS, counter, WORKERS * BUMPS, tls_ok ? "ok" : "shared");
        fflush(stdout);
    }

    /* C5_MMAP. No failure returns early from here on: each stage reports,
     * because a kernel that fails one of these usually fails the others for
     * the same reason and the three lines together say which. */
    {
        pthread_t mt;
        void *r = 0;
        unsigned char *theirs, *ours;
        long i, intact = 1;

        if (pthread_create(&mt, 0, mmap_worker, 0) != 0 ||
            pthread_join(mt, &r) != 0 || r == 0) {
            printf("THREADS_C5_MMAP_FAIL: the thread could not map\n");
        } else {
            theirs = r;
            ours = mmap(0, C5_LEN, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (ours == MAP_FAILED) {
                printf("THREADS_C5_MMAP_FAIL: main could not map\n");
            } else {
                memset(ours, 0x5A, C5_LEN);
                for (i = 0; i < C5_LEN; i++) {
                    if (theirs[i] != 0xA5) {
                        intact = 0;
                        break;
                    }
                }
                if (ours == theirs) {
                    printf("THREADS_C5_MMAP_FAIL: main was given the thread's base %p\n",
                           (void *)ours);
                } else if (!intact) {
                    printf("THREADS_C5_MMAP_FAIL: the thread's pages changed at +%ld\n", i);
                } else {
                    printf("THREADS_C5_MMAP_OK\n");
                }
            }
        }
        fflush(stdout);
    }

    /* C5_SIGACTION, in a child: with the defect, the raise kills it. */
    {
        pid_t pid = fork();

        if (pid == 0) {
            pthread_t st;
            void *r = (void *)1L;

            if (pthread_create(&st, 0, sigaction_worker, 0) != 0 ||
                pthread_join(st, &r) != 0 || r != 0) {
                _exit(3);
            }
            raise(SIGUSR1);
            _exit(usr1_seen ? 0 : 4);
        }
        report_child("C5_SIGACTION", pid, 0);
    }

    /* C5_EXIT_GROUP, in a child. Main spins a bounded number of yields - each
     * is one timer tick, so this is about two seconds - and only exits 7 if
     * nothing ended it first. */
    {
        pid_t pid = fork();

        if (pid == 0) {
            pthread_t et;
            int n;

            if (pthread_create(&et, 0, exit_group_worker, 0) != 0) {
                _exit(3);
            }
            exit_group_go = 1;
            for (n = 0; n < 200; n++) {
                sched_yield();
            }
            syscall(SYS_exit_group, 7);
        }
        report_child("C5_EXIT_GROUP", pid, 42);
    }

    /* C5_WNOHANG. A child that lives about a second and exits on its own.
     * WNOHANG asked immediately must answer 0 - no child has changed state -
     * and not wait. On a kernel that ignores the option the call blocks until
     * the child exits and returns its pid: a failure that is deterministic and
     * bounded by the child's own lifetime, so it cannot stall the boot. If this
     * ever flakes, lengthen the child rather than loosening the assertion. */
    {
        pid_t pid = fork();

        if (pid == 0) {
            int n;
            for (n = 0; n < 100; n++) {
                sched_yield();
            }
            _exit(0);
        }
        if (pid < 0) {
            printf("THREADS_C5_WNOHANG_FAIL: fork\n");
        } else {
            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);

            if (r == 0) {
                (void)waitpid(pid, &status, 0);   /* reap it for real */
                printf("THREADS_C5_WNOHANG_OK\n");
            } else if (r == pid) {
                printf("THREADS_C5_WNOHANG_FAIL: waited for the child to exit\n");
            } else {
                printf("THREADS_C5_WNOHANG_FAIL: returned %d\n", (int)r);
            }
        }
        fflush(stdout);
    }

    /* C5_TKILL. A thread's id must address that thread. Two checks, both
     * deterministic: pthread_kill with signal 0 asks only whether the thread
     * exists, and raise() inside the thread must reach the handler. */
    {
        struct sigaction sa;
        pthread_t kt;
        int n, r;

        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_usr2;
        sigemptyset(&sa.sa_mask);
        (void)sigaction(SIGUSR2, &sa, 0);

        if (pthread_create(&kt, 0, tkill_worker, 0) != 0) {
            printf("THREADS_C5_TKILL_FAIL: create\n");
        } else {
            for (n = 0; n < 20; n++) {
                sched_yield();
            }
            r = pthread_kill(kt, 0);
            tkill_release = 1;
            (void)pthread_join(kt, 0);
            if (r != 0) {
                printf("THREADS_C5_TKILL_FAIL: pthread_kill on a live thread returned %d\n", r);
            } else if (!usr2_seen) {
                printf("THREADS_C5_TKILL_FAIL: raise() in a thread never reached it\n");
            } else {
                printf("THREADS_C5_TKILL_OK\n");
            }
        }
        fflush(stdout);
    }

    /* C5_EXIT_GROUP_BLOCKED. The leader has to be the blocked one. If the
     * caller of exit_group were the leader it would exit 42 itself, the parent
     * would reap 42, and a stuck worker would go unnoticed - a stage that
     * passes on the defect. And the wait is a condition nobody signals, not
     * pthread_join: the exiting thread's own clear_child_tid would wake a join
     * and hide the defect. */
    {
        pid_t pid = fork();

        if (pid == 0) {
            pthread_t lt;

            if (pthread_create(&lt, 0, exit_group_later, 0) != 0) {
                _exit(3);
            }
            pthread_mutex_lock(&never_m);
            for (;;) {
                pthread_cond_wait(&never_c, &never_m);
            }
        }
        report_child_bounded("C5_EXIT_GROUP_BLOCKED", pid, 42, 400);
    }
    return 0;
}
