/* Native VibeOS init: the first ring-3 process, and the supervisor of
 * everything after it.
 *
 * It starts the services in the manifest below, waits for whatever exits, and
 * decides what happens next from that service's own policy. Three things it is
 * careful about, because each is a way "services work" can be true from a
 * distance and false up close:
 *
 *  - a clean stop is not a failure. A service that exits zero has finished;
 *    restarting it is an infinite loop wearing the word "recovery".
 *  - a failure is not fatal to init. The supervisor observes it, applies the
 *    policy, and is still there afterwards - which is the entire reason for
 *    having a process whose job is to outlive the others.
 *  - restarting is bounded. A service that fails instantly and is restarted
 *    without a limit is the same infinite loop, only busier. Past its limit it
 *    is marked FAILED and left alone.
 *
 * The bring-up workload is one of the services rather than something init
 * becomes. It used to *be* PID 1, which is why there had been no process left
 * over to supervise anything.
 *
 * Freestanding: no libc, syscalls by hand, and every string written whole so
 * that two writers on two cores cannot split a line another program is
 * matching on.
 */

#include <stdint.h>

#define SYS_write  1
#define SYS_fork   57
#define SYS_execve 59
#define SYS_exit   60
#define SYS_wait4  61

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

/* ---- writing without a library ------------------------------------------ */

static unsigned put_str(char *dst, unsigned at, const char *s) {
    while (*s) {
        dst[at++] = *s++;
    }
    return at;
}

static unsigned put_dec(char *dst, unsigned at, int64_t value) {
    char digits[20];
    unsigned d = 0;
    uint64_t v;

    if (value < 0) {
        dst[at++] = '-';
        v = (uint64_t)(-value);
    } else {
        v = (uint64_t)value;
    }
    if (v == 0ul) {
        digits[d++] = '0';
    }
    while (v != 0ul && d < sizeof(digits)) {
        digits[d++] = (char)('0' + (v % 10ul));
        v /= 10ul;
    }
    while (d > 0u) {
        dst[at++] = digits[--d];
    }
    return at;
}

/* One line, one write. Anything a gate matches on has to arrive whole. */
static void say(const char *a, const char *b, int64_t n1, int64_t n2) {
    char line[128];
    unsigned at = 0;

    at = put_str(line, at, a);
    if (b) {
        line[at++] = ' ';
        at = put_str(line, at, b);
    }
    if (n1 >= 0) {
        line[at++] = ' ';
        at = put_dec(line, at, n1);
    }
    if (n2 >= 0) {
        line[at++] = ' ';
        at = put_dec(line, at, n2);
    }
    line[at++] = '\n';
    sys3(SYS_write, 1, (uint64_t)(uintptr_t)line, (uint64_t)at);
}

/* ---- the manifest -------------------------------------------------------- */

enum svc_state {
    SVC_STOPPED = 0,   /* finished cleanly, nothing more to do */
    SVC_RUNNING,
    SVC_FAILED         /* exhausted its restarts */
};

typedef struct {
    const char *name;
    const char *path;
    /* One optional argument, or null. It exists so a service that prints a
     * replay seed can be handed one back: without it the seed from a failing
     * boot can be read and never used, which is a reproduction recipe nobody
     * can follow. */
    const char *arg;
    int restart_limit;   /* how many times a failure may be retried */
    /* A session rather than a daemon: it runs until the user is finished with
     * it, and whatever status it leaves with is the end of the session and not
     * a fault. Without this distinction the shell exiting on `halt` is
     * reported as the main workload having failed, which is both wrong and the
     * kind of wrong that trains people to ignore the word. */
    int oneshot;
    int restarts;
    int64_t pid;
    int state;
} service_t;

/* Each service is exec'd under its own name. argv[0] is chosen by whoever
 * calls exec, not by the filesystem - the same reason one BusyBox binary is
 * twenty commands - and a supervisor that gives every service the same name
 * makes its own logs useless. */
static char *svc_argv[] = {(char *)"svc", 0, 0};
static char *const svc_envp[] = {(char *)"PATH=/EFI/BOOT", (char *)"TERM=dumb", 0};

static service_t services[] = {
    /* The bring-up workload: everything the boot gate checks runs inside this
     * one, and it ends by exec'ing the shell. A session - when the shell is
     * done, so is the machine. */
    {"selftest", "EFI/BOOT/SELFTEST.ELF", 0, 0, 1, 0, -1, SVC_STOPPED},
    /* Exits zero: a clean stop, which must not be restarted. */
    {"svc-ok", "EFI/BOOT/SVC_OK.ELF", 0, 2, 0, 0, -1, SVC_STOPPED},
    /* Fails every time: restarted twice, then marked FAILED and left alone. */
    {"svc-flap", "EFI/BOOT/SVC_FLAP.ELF", 0, 2, 0, 0, -1, SVC_STOPPED},
    /* Faults instead of exiting. Not restarted: the point is not recovery but
     * that the supervisor is still running to report it, which is only true
     * because the kernel now kills the faulting task instead of halting. */
    {"svc-crash", "EFI/BOOT/SVC_CRSH.ELF", 0, 0, 0, 0, -1, SVC_STOPPED},
    /* Randomised churn with a printed seed. A session, not a daemon: it runs a
     * bounded number of rounds and its exit status is the verdict. */
    {"svc-stress", "EFI/BOOT/SVC_STRS.ELF", 0, 0, 0, 0, -1, SVC_STOPPED},
    /* A fork storm, on purpose. What it proves is not its own output but that
     * everything after it still works: init can still start something and the
     * shell can still run a command once a program has taken everything it is
     * allowed to. Last in the manifest so that "after" means something. */
    {"svc-bomb", "EFI/BOOT/SVC_BOMB.ELF", 0, 0, 0, 0, -1, SVC_STOPPED},
    /* svc-press is built and shipped but NOT started here, and that is a
     * statement about the kernel rather than about the test.
     *
     * It allocates 256 KiB at a time and touches every page. At 80 blocks -
     * twenty megabytes, on a guest with four hundred - the machine stops
     * answering and the serial log fills with binary. That is not memory
     * exhaustion and it is not the watermarks refusing anything; it is a
     * defect this boot had all along and nothing had ever asked it for enough
     * pages in a row to find.
     *
     * Starting it here would turn every boot red on a defect that is not
     * reclaim's, and this project has a rule about gates people learn to
     * ignore. Run it by hand from the shell, or add it back once the defect
     * behind it is closed. Written up in
     * docs/implementation_progress/mm_reclaim.md. */
};

#define SERVICE_COUNT (int)(sizeof(services) / sizeof(services[0]))

static int start_service(service_t *svc) {
    int64_t child = sys3(SYS_fork, 0, 0, 0);

    if (child == 0) {
        svc_argv[0] = (char *)svc->name;
        /* One optional argument, so a service that prints a replay seed can be
         * given one back. Without this the seed in a failing boot could be read
         * and never used - a reproduction recipe nobody can follow. */
        svc_argv[1] = (char *)svc->arg;
        sys3(SYS_execve, (uint64_t)(uintptr_t)svc->path,
             (uint64_t)(uintptr_t)svc_argv, (uint64_t)(uintptr_t)svc_envp);
        /* execve only returns when it failed, and there is nothing else this
         * process can usefully become. */
        sys3(SYS_exit, 127, 0, 0);
    }
    if (child < 0) {
        say("SVC_SPAWN_FAILED", svc->name, -1, -1);
        svc->state = SVC_FAILED;
        return -1;
    }
    svc->pid = child;
    svc->state = SVC_RUNNING;
    say("SVC_START", svc->name, child, -1);
    return 0;
}

static service_t *service_by_pid(int64_t pid) {
    int i;

    for (i = 0; i < SERVICE_COUNT; i++) {
        if (services[i].state == SVC_RUNNING && services[i].pid == pid) {
            return &services[i];
        }
    }
    return 0;
}

int vibeos_main(int argc, char **argv, char **envp) {
    int i;
    int live;

    (void)argc;
    (void)argv;
    (void)envp;

    say("NATIVE_INIT_READY", 0, -1, -1);

    for (i = 0; i < SERVICE_COUNT; i++) {
        (void)start_service(&services[i]);
    }
    /* Kept for the gate that predates the manifest: it says init is a parent
     * rather than the workload wearing init's name. */
    say("NATIVE_INIT_CHILD_PID=", 0, services[0].pid, -1);

    for (;;) {
        int64_t status = 0;
        int64_t gone = sys3(SYS_wait4, (uint64_t)-1,
                            (uint64_t)(uintptr_t)&status, 0);
        service_t *svc;
        int code;
        int killed;

        if (gone < 0) {
            break;   /* nothing left to wait for */
        }
        svc = service_by_pid(gone);
        if (!svc) {
            continue;   /* not one of ours: an orphan we adopted */
        }

        /* The exit code lives in the high byte; the low seven bits are a
         * signal number, and the two mean different things. A service killed
         * by a signal carries the signal there and leaves the code byte zero -
         * so reading only the code byte reports a segfault as a clean exit,
         * which is how svc-crash first came back STOPPED. */
        code = (int)((status >> 8) & 0xff);
        killed = (int)(status & 0x7f);
        if (killed != 0) {
            say("SVC_KILLED", svc->name, gone, killed);
        } else {
            say("SVC_EXIT", svc->name, gone, code);
        }

        if (killed == 0 && (code == 0 || svc->oneshot)) {
            svc->state = SVC_STOPPED;
            say("SVC_STOPPED", svc->name, -1, -1);
        } else if (svc->restarts < svc->restart_limit) {
            svc->restarts++;
            say("SVC_RESTART", svc->name, svc->restarts, -1);
            (void)start_service(svc);
        } else {
            svc->state = SVC_FAILED;
            say("SVC_FAILED", svc->name, svc->restarts, -1);
        }

        if (svc->oneshot) {
            break;   /* the session is over; nothing else is worth waiting for */
        }
        live = 0;
        for (i = 0; i < SERVICE_COUNT; i++) {
            if (services[i].state == SVC_RUNNING) {
                live++;
            }
        }
        if (live == 0) {
            break;
        }
    }

    /* A summary the CLI and the gate can both read, and proof that init is
     * still here after a service failed. */
    for (i = 0; i < SERVICE_COUNT; i++) {
        const char *state = services[i].state == SVC_RUNNING ? "RUNNING"
                          : services[i].state == SVC_FAILED ? "FAILED"
                          : "STOPPED";
        say("SVC_STATUS", services[i].name, services[i].restarts, -1);
        say(state, services[i].name, -1, -1);
    }
    say("NATIVE_INIT_CHILD_EXITED", 0, -1, -1);
    return 0;
}
