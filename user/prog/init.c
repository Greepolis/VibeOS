/* SPDX-License-Identifier: GPL-3.0-or-later */
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

#define SYS_read   0
#define SYS_write  1
#define SYS_open   2
#define SYS_close  3
#define SYS_fork   57
#define SYS_execve 59
#define SYS_exit   60
#define SYS_wait4  61

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    /* r10, r8 and r9 are zero, not whatever the compiler left in them: a call
     * that has a fourth argument reads it whether or not the caller meant one.
     * wait4 has filled its rusage since docs/abi/ L2 step 5, and clang kept
     * this file's pointer to the service table in r10 - so every reap wrote
     * 144 bytes over the services, and init read a name that was the number 1
     * (L2 step 7; gcc's registers happened to hold nothing that mattered). */
    register uint64_t r10 __asm__("r10") = 0;
    register uint64_t r8 __asm__("r8") = 0;
    register uint64_t r9 __asm__("r9") = 0;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                           "r"(r10), "r"(r8), "r"(r9)
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
static char *const svc_envp[] = {(char *)"PATH=/usr/bin", (char *)"TERM=dumb", 0};

static service_t services[] = {
    /* The bring-up workload: everything the boot gate checks runs inside this
     * one, and it ends by exec'ing the shell. A session - when the shell is
     * done, so is the machine. */
    {"selftest", "/usr/bin/SELFTEST.ELF", 0, 0, 1, 0, -1, SVC_STOPPED},
    /* Exits zero: a clean stop, which must not be restarted. */
    {"svc-ok", "/usr/bin/SVC_OK.ELF", 0, 2, 0, 0, -1, SVC_STOPPED},
    /* Fails every time: restarted twice, then marked FAILED and left alone. */
    {"svc-flap", "/usr/bin/SVC_FLAP.ELF", 0, 2, 0, 0, -1, SVC_STOPPED},
    /* Faults instead of exiting. Not restarted: the point is not recovery but
     * that the supervisor is still running to report it, which is only true
     * because the kernel now kills the faulting task instead of halting. */
    {"svc-crash", "/usr/bin/SVC_CRSH.ELF", 0, 0, 0, 0, -1, SVC_STOPPED},
    /* Randomised churn with a printed seed. A session, not a daemon: it runs a
     * bounded number of rounds and its exit status is the verdict. */
    {"svc-stress", "/usr/bin/SVC_STRS.ELF", 0, 0, 0, 0, -1, SVC_STOPPED},
    /* A fork storm, on purpose. What it proves is not its own output but that
     * everything after it still works: init can still start something and the
     * shell can still run a command once a program has taken everything it is
     * allowed to. Last in the manifest so that "after" means something. */
    /* 64 MiB, 256 KiB at a time, every page touched, checked and given back.
     *
     * It was built long before it could be started: the first 28 MiB of
     * allocation on this machine reached the kernel's own image, because the
     * loader handed over a memory map taken before it loaded the kernel
     * (M-060), and unmapping the 64 MiB afterwards leaked 2,486 frames through
     * the TLB quarantine (M-061). Both are what a boot now gates by running it.
     * It does not reach the watermarks; svc-reclaim does, run last in the
     * boot's self-test rather than from here so that nothing runs beside it. */
    {"svc-press", "/usr/bin/SVC_PRES.ELF", 0, 0, 0, 0, -1, SVC_STOPPED},
    {"svc-bomb", "/usr/bin/SVC_BOMB.ELF", 0, 0, 0, 0, -1, SVC_STOPPED},
};

/* ---- the manifest on the root ------------------------------------------- */

/* A system that is not the boot gate's says what it runs in
 * /etc/vibeos/services, one service a line:
 *
 *     <name> <path> <restart_limit> <oneshot> [<arg>]
 *
 * '#' starts a comment. Without the file - the gate's disk, a bare ESP - init
 * runs the table above, which is the self-test (E4-023: the test workload was
 * the only thing any image could run, so an image for people booted the
 * gate's two minutes of stress before it reached a prompt). The file is read
 * whole into one buffer and the table points into it. */
#define MANIFEST_MAX 16
static char manifest_text[2048];
static service_t manifest[MANIFEST_MAX];
static service_t *svcs = services;
static int nsvcs = (int)(sizeof(services) / sizeof(services[0]));

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

/* A non-negative decimal, or -1. */
static int parse_num(const char *t) {
    int v = 0;

    if (!t || !*t) {
        return -1;
    }
    for (; *t; t++) {
        if (*t < '0' || *t > '9' || v > 1000) {
            return -1;
        }
        v = v * 10 + (*t - '0');
    }
    return v;
}

static void load_manifest(void) {
    int64_t fd = sys3(SYS_open, (uint64_t)(uintptr_t)"/etc/vibeos/services", 0, 0);
    int64_t got = 0, n;
    int count = 0, bad = 0;
    char *at, *end;

    if (fd < 0) {
        say("NATIVE_INIT_MANIFEST builtin", 0, -1, -1);
        return;
    }
    while (got < (int64_t)sizeof(manifest_text) - 1 &&
           (n = sys3(SYS_read, (uint64_t)fd, (uint64_t)(uintptr_t)(manifest_text + got),
                     (uint64_t)(sizeof(manifest_text) - 1u - (uint64_t)got))) > 0) {
        got += n;
    }
    sys3(SYS_close, (uint64_t)fd, 0, 0);
    manifest_text[got] = 0;

    at = manifest_text;
    end = manifest_text + got;
    while (at < end) {
        char *line = at, *tok[5];
        int k = 0;

        while (at < end && *at != '\n') {
            at++;
        }
        *at++ = 0;
        /* Comments end the line; then split on blanks, in place. */
        {
            char *c = line;
            while (*c && *c != '#') {
                c++;
            }
            *c = 0;
        }
        while (*line && k < 5) {
            while (is_space(*line)) {
                *line++ = 0;
            }
            if (!*line) {
                break;
            }
            tok[k++] = line;
            while (*line && !is_space(*line)) {
                line++;
            }
        }
        if (k == 0) {
            continue;
        }
        if (k < 4 || count >= MANIFEST_MAX || parse_num(tok[2]) < 0 || parse_num(tok[3]) < 0) {
            bad++;
            continue;
        }
        manifest[count].name = tok[0];
        manifest[count].path = tok[1];
        manifest[count].restart_limit = parse_num(tok[2]);
        manifest[count].oneshot = parse_num(tok[3]) != 0;
        manifest[count].arg = k == 5 ? tok[4] : 0;
        manifest[count].restarts = 0;
        manifest[count].pid = -1;
        manifest[count].state = SVC_STOPPED;
        count++;
    }
    /* A file that names nothing usable is a broken system, not a reason to run
     * the self-test on somebody's machine: say so, and run nothing. */
    say("NATIVE_INIT_MANIFEST file", 0, count, bad);
    svcs = manifest;
    nsvcs = count;
}

#define SERVICE_COUNT nsvcs

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
        if (svcs[i].state == SVC_RUNNING && svcs[i].pid == pid) {
            return &svcs[i];
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
    load_manifest();

    for (i = 0; i < SERVICE_COUNT; i++) {
        (void)start_service(&svcs[i]);
    }
    /* Kept for the gate that predates the manifest: it says init is a parent
     * rather than the workload wearing init's name. */
    say("NATIVE_INIT_CHILD_PID=", 0, nsvcs > 0 ? svcs[0].pid : -1, -1);

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
            if (svcs[i].state == SVC_RUNNING) {
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
        const char *state = svcs[i].state == SVC_RUNNING ? "RUNNING"
                          : svcs[i].state == SVC_FAILED ? "FAILED"
                          : "STOPPED";
        say("SVC_STATUS", svcs[i].name, svcs[i].restarts, -1);
        say(state, svcs[i].name, -1, -1);
    }
    say("NATIVE_INIT_CHILD_EXITED", 0, -1, -1);
    return 0;
}
