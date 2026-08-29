/* A service that does not exit: it faults.
 *
 * svc-flap proves the restart policy, but it exits with a status - which is a
 * cooperative death, and the supervisor never learns whether it can survive an
 * uncooperative one. This one dereferences a null pointer, so the kernel has
 * to decide what a ring-3 fault means. Until that decision existed, the answer
 * was "the machine halts", and the claim that the kernel stays up after a
 * service crashes was true only because nothing had ever really crashed.
 */

#include <stdint.h>

static int64_t sys3(int64_t nr, uint64_t a1, uint64_t a2, uint64_t a3) {
    int64_t ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
    return ret;
}

int vibeos_main(int argc, char **argv, char **envp) {
    static const char msg[] = "SVC_CRASH_FAULTING\n";
    volatile uint64_t *nowhere = (volatile uint64_t *)0;

    (void)argc;
    (void)argv;
    (void)envp;

    sys3(1 /* write */, 1, (uint64_t)(uintptr_t)msg, sizeof(msg) - 1u);
    *nowhere = 1;   /* #PF in ring 3: the kernel must kill this task only */

    /* Not reached. If it ever is, the fault was swallowed rather than acted
     * on, and saying so is better than exiting zero and looking healthy. */
    sys3(1, 1, (uint64_t)(uintptr_t)"SVC_CRASH_SURVIVED\n", 19u);
    sys3(60 /* exit */, 0, 0, 0);
    return 0;
}
