/* A fork storm, on purpose. Phase S-P6 of docs/sched/.
 *
 * The property being tested is not "fork eventually fails" - it always did, and
 * returned an error as it should. It is that when a program takes everything it
 * is allowed to, **the machine is still administrable**: init can still start a
 * service, the shell can still run a command. A machine that is alive and
 * cannot be acted on is worse than one that refuses.
 *
 * So this service forks until the kernel says no, reports how far it got and
 * which rule stopped it, reaps every child, and exits cleanly. What proves the
 * point is not this program's output - it is that everything after it in the
 * boot still works, and that the task table comes back to the shape it started
 * in.
 *
 * Children exit immediately rather than sleeping. A storm of live children
 * would test the same limit, but a storm of *zombies* is the harder case for
 * the kernel and the one that has actually broken things here: a zombie still
 * holds a slot, so the parent must reap them all or the table stays full for
 * everybody else.
 */
#include <stdint.h>

#define SYS_write  1
#define SYS_fork   57
#define SYS_exit   60
#define SYS_wait4  61

/* Whatever the guard allows, plus enough headroom that this program stops
 * because the kernel stopped it and not because it ran out of patience. If it
 * ever reaches this number the guard is not working, and the report says so
 * rather than the program quietly succeeding. */
#define ATTEMPTS 64

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

static void put(const char *s, uint64_t n) {
    uint64_t len = 0;
    while (s[len]) {
        len++;
    }
    sys3(SYS_write, 1, (uint64_t)(uintptr_t)s, len);
    if (n != (uint64_t)-1) {
        char buf[24];
        int i = 0, j;
        if (n == 0ull) {
            buf[i++] = '0';
        }
        while (n > 0ull && i < 20) {
            buf[i++] = (char)('0' + (int)(n % 10ull));
            n /= 10ull;
        }
        for (j = 0; j < i / 2; j++) {
            char t = buf[j];
            buf[j] = buf[i - 1 - j];
            buf[i - 1 - j] = t;
        }
        buf[i++] = '\n';
        sys3(SYS_write, 1, (uint64_t)(uintptr_t)buf, (uint64_t)i);
    } else {
        sys3(SYS_write, 1, (uint64_t)(uintptr_t)"\n", 1);
    }
}

int vibeos_main(int argc, char **argv, char **envp) {
    int64_t kids[ATTEMPTS];
    int made = 0;
    int64_t refusal = 0;
    int i;

    (void)argc; (void)argv; (void)envp;

    put("SVC_BOMB_START", (uint64_t)-1);

    for (i = 0; i < ATTEMPTS; i++) {
        int64_t pid = sys3(SYS_fork, 0, 0, 0);

        if (pid == 0) {
            /* A child of the storm. It exits at once and becomes a zombie
             * holding a slot, which is the state the parent has to clean up. */
            sys3(SYS_exit, 0, 0, 0);
        }
        if (pid < 0) {
            refusal = pid;
            break;
        }
        kids[made++] = pid;
    }

    put("SVC_BOMB_FORKED ", (uint64_t)made);
    /* The errno the kernel chose, negated. EAGAIN is 11 and is what the guard
     * returns: the refusal is temporary by nature, since reaping undoes it. */
    put("SVC_BOMB_REFUSED_WITH ", (uint64_t)(-refusal));

    if (refusal == 0) {
        /* Never refused. Either the guard is not working or ATTEMPTS is below
         * the limit; either way this is not the test passing. */
        put("SVC_BOMB_UNBOUNDED", (uint64_t)-1);
    }

    for (i = 0; i < made; i++) {
        int status = 0;
        (void)sys3(SYS_wait4, (uint64_t)kids[i],
                   (uint64_t)(uintptr_t)&status, 0);
    }

    /* Said last, and it is the line the gate matches: the storm is over and
     * every slot it took has been given back. Everything the boot does after
     * this is the actual assertion. */
    put("SVC_BOMB_DONE", (uint64_t)-1);
    sys3(SYS_exit, 0, 0, 0);
    return 0;
}
