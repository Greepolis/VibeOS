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
#define SYS_read   0
#define SYS_write  1
#define SYS_mmap   9
#define SYS_brk    12
#define SYS_getpid 39
#define SYS_exit   60
#define SYS_fork   57
#define SYS_wait4  61

static const char message[] = "Hello from a real user ELF (loaded by the kernel)\n";
static const char ok_heap[] = "heap+mmap ok\n";
static const char bad_ptr[] = "kernel pointer correctly rejected\n";
static const char in_child[] = "child process running (fork ok)\n";
static const char reaped_ok[] = "parent reaped child with status 7\n";

void _start(void) {
    long pid, brk0, brk1, map, rejected;

    user_syscall3(SYS_write, 1 /*stdout*/, (long)(unsigned long)message, sizeof(message) - 1);

    pid = user_syscall3(SYS_getpid, 0, 0, 0);

    /* Grow the heap, then take an anonymous mapping, and touch both. */
    brk0 = user_syscall3(SYS_brk, 0, 0, 0);
    brk1 = user_syscall3(SYS_brk, brk0 + 8192, 0, 0);
    if (brk1 > brk0) {
        *(volatile char *)(unsigned long)brk0 = 'x';
    }
    map = user_syscall3(SYS_mmap, 0, 4096, 0);
    if (map > 0) {
        *(volatile char *)(unsigned long)map = 'y';
    }
    if (brk1 > brk0 && map > 0) {
        user_syscall3(SYS_write, 1, (long)(unsigned long)ok_heap, sizeof(ok_heap) - 1);
    }

    /* The kernel must refuse a buffer that is not in our address space. */
    rejected = user_syscall3(SYS_write, 1, 0x4000000L /* kernel image */, 8);
    if (rejected < 0) {
        user_syscall3(SYS_write, 1, (long)(unsigned long)bad_ptr, sizeof(bad_ptr) - 1);
    }

    /* Classic POSIX process creation: fork, child exits, parent reaps it. */
    {
        long child = user_syscall3(SYS_fork, 0, 0, 0);
        if (child == 0) {
            user_syscall3(SYS_write, 1, (long)(unsigned long)in_child, sizeof(in_child) - 1);
            user_syscall3(SYS_exit, 7, 0, 0);
        } else if (child > 0) {
            volatile int status = 0;
            long reaped = user_syscall3(SYS_wait4, child, (long)(unsigned long)&status, 0);
            if (reaped == child && ((status >> 8) & 0xff) == 7) {
                user_syscall3(SYS_write, 1, (long)(unsigned long)reaped_ok, sizeof(reaped_ok) - 1);
            }
        }
    }

    user_syscall3(SYS_exit, pid, 0, 0);
    for (;;) {
        /* not reached */
    }
}
