/* Native VibeOS init: the first ring-3 process, and the one that owns the
 * lifecycle of everything after it.
 *
 * It starts the bring-up workload as a *child* rather than being it. That
 * distinction is the whole point of the program: until now the kernel spawned
 * a fixed ELF that did the work and then replaced itself with the shell, so
 * there was no process whose job was to outlive anything. Here the child can
 * exit, or fail, and init is still there to say so and decide what happens
 * next.
 *
 * The child is the existing self-test, deliberately. Making init the parent of
 * what already runs keeps every check that already exists while moving the
 * boundary; replacing it with something smaller would have traded real
 * coverage for a cleaner-looking boot.
 */

#define SYS_write 1
#define SYS_fork 57
#define SYS_execve 59
#define SYS_exit 60
#define SYS_wait4 61

#include <stdint.h>

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

static const char init_ready[] = "NATIVE_INIT_READY\n";
static const char child_gone[] = "NATIVE_INIT_CHILD_EXITED\n";
static const char no_child[] = "NATIVE_INIT_FORK_FAILED\n";

/* The bring-up workload. It ends by exec'ing the shell, so this one child
 * covers the whole session and init waits for all of it. */
static const char child_path[] = "EFI/BOOT/SELFTEST.ELF";
static const char child_name[] = "selftest";
static char *const init_argv[] = {(char *)child_name, 0};
static char *const init_envp[] = {(char *)"PATH=/EFI/BOOT", (char *)"TERM=dumb", 0};

int vibeos_main(int argc, char **argv, char **envp) {
    int64_t child;
    int64_t status = 0;
    (void)argc;
    (void)argv;
    (void)envp;
    sys3(SYS_write, 1, (uint64_t)(uintptr_t)init_ready, sizeof(init_ready) - 1);

    child = sys3(SYS_fork, 0, 0, 0);
    if (child == 0) {
        sys3(SYS_execve, (uint64_t)(uintptr_t)child_path,
             (uint64_t)(uintptr_t)init_argv, (uint64_t)(uintptr_t)init_envp);
        /* exec only returns when it failed, and there is nothing else this
         * process can usefully be. */
        sys3(SYS_exit, 127, 0, 0);
    }
    if (child < 0) {
        sys3(SYS_write, 1, (uint64_t)(uintptr_t)no_child, sizeof(no_child) - 1);
        return 127;
    }

    /* Say which process this is now responsible for.
     *
     * Not decoration: it is the only line in the whole boot that distinguishes
     * an init which forked a child from one that simply became the workload
     * itself. Everything else in the log looks identical either way, so a gate
     * without this proves that init ran and nothing about what it is for.
     */
    {
        static const char pid_prefix[] = "NATIVE_INIT_CHILD_PID=";
        char line[48];
        char digits[20];
        unsigned long v = (unsigned long)child;
        unsigned n = 0;
        unsigned d = 0;

        while (n < sizeof(pid_prefix) - 1u) {
            line[n] = pid_prefix[n];
            n++;
        }
        if (v == 0ul) {
            digits[d++] = '0';
        }
        while (v != 0ul && d < sizeof(digits)) {
            digits[d++] = (char)('0' + (v % 10ul));
            v /= 10ul;
        }
        while (d > 0u) {
            line[n++] = digits[--d];
        }
        line[n++] = '\n';
        sys3(SYS_write, 1, (uint64_t)(uintptr_t)line, (uint64_t)n);
    }

    if (sys3(SYS_wait4, (uint64_t)child, (uint64_t)(uintptr_t)&status, 0) == child) {
        /* Said out loud: a child that ends is an event init observed, not a
         * silence. Reporting it is what makes init a supervisor rather than a
         * launcher. */
        sys3(SYS_write, 1, (uint64_t)(uintptr_t)child_gone, sizeof(child_gone) - 1);
        return (int)((status >> 8) & 0xff);
    }
    return 127;
}
