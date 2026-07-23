/* Freestanding user-space program for VibeOS.
 *
 * Built as a static, position-dependent ELF64 (see user.ld) with no libc. It
 * talks to the kernel only through syscalls, using the Linux x86-64 argument
 * order (nr in rax; args in rdi, rsi, rdx). The kernel loads this ELF and runs
 * it in ring 3.
 *
 * VIBEOS_USE_SYSCALL_INSN selects the entry mechanism:
 *   - unset: legacy `int 0x80` gate (bring-up path)
 *   - set:   the native `syscall` instruction (real Linux ABI)
 */

static long user_syscall3(long nr, long a1, long a2, long a3) {
    long ret;
#if defined(VIBEOS_USE_SYSCALL_INSN)
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "rcx", "r11", "memory");
#else
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                         : "memory");
#endif
    return ret;
}

/* Linux syscall numbers (x86-64). */
#define SYS_write 1
#define SYS_exit  60

static const char message[] = "Hello from a real user ELF (loaded by the kernel)\n";

void _start(void) {
    user_syscall3(SYS_write, 1 /*stdout*/, (long)(unsigned long)message, sizeof(message) - 1);
    user_syscall3(SYS_exit, 0, 0, 0);
    for (;;) {
        /* not reached */
    }
}
