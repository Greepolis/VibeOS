/* A service that starts, says so, and exits cleanly.
 *
 * The point of it is the contrast with svc_flap.c: a supervisor that cannot
 * tell a clean stop from a failure will either restart this one forever or
 * give up on the other one, and both mistakes look like "services work" from
 * a distance.
 */
#include <stdint.h>

#define SYS_write 1
#define SYS_exit  60

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    /* r10, r8 and r9 zero, not what the compiler left there: a fourth
     * argument is read whether or not it was meant (see init.c's sys3). */
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

static const char msg[] = "SVC_OK_RUNNING\n";

int vibeos_main(int argc, char **argv, char **envp) {
    (void)argc; (void)argv; (void)envp;
    sys3(SYS_write, 1, (uint64_t)(uintptr_t)msg, sizeof(msg) - 1);
    sys3(SYS_exit, 0, 0, 0);
    return 0;
}
