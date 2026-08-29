/* A service that fails every time it is started.
 *
 * Deliberately not a crash: it exits with a non-zero status, which is the
 * failure a supervisor is supposed to handle by policy rather than by dying
 * with it. Restarting it must be bounded - a service that fails instantly and
 * is restarted without a limit is an infinite loop wearing the word "recovery".
 */
#include <stdint.h>

#define SYS_write 1
#define SYS_exit  60

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

static const char msg[] = "SVC_FLAP_STARTING\n";

int vibeos_main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    sys3(SYS_write, 1, (uint64_t)(uintptr_t)msg, sizeof(msg) - 1);
    sys3(SYS_exit, 3, 0, 0);   /* always fails */
    return 3;
}
