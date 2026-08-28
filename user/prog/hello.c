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

/* Six-argument form. Linux puts the fourth, fifth and sixth arguments in r10,
 * r8 and r9 - not rcx, which the syscall instruction overwrites with the
 * return address. mmap is the reason this exists: with fewer arguments there
 * is no way to say "anonymous, private, no file". */
static long user_syscall6(long nr, long a1, long a2, long a3,
                          long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;
#if defined(VIBEOS_USE_SYSCALL_INSN)
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                           "r"(r10), "r"(r8), "r"(r9)
                         : "rcx", "r11", "memory");
#else
    __asm__ __volatile__("int $0x80"
                         : "=a"(ret)
                         : "a"(nr), "D"(a1), "S"(a2), "d"(a3),
                           "r"(r10), "r"(r8), "r"(r9)
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
#define SYS_execve 59
#define SYS_mprotect 10
#define SYS_munmap   11
#define SYS_futex    202
#define SYS_ioctl    16
#define SYS_writev   20
#define SYS_uname    63
#define SYS_arch_prctl 158
#define SYS_clock_gettime 228

static const char message[] = "Hello from a real user ELF (loaded by the kernel)\n";
static const char ok_heap[] = "heap+mmap ok\n";
static const char bad_ptr[] = "kernel pointer correctly rejected\n";
static const char in_child[] = "child process running (fork ok)\n";
static const char reaped_ok[] = "parent reaped child with status 7\n";
static const char exec_path[] = "EFI/BOOT/TASK.ELF";
static const char exec_done[] = "parent: exec'd child completed\n";
static const char kbd_prefix[] = "console read: ";
static const char shell_path[] = "EFI/BOOT/SH.ELF";

static const char argv_bad[] = "argv wrong\n";
static const char auxv_ok[] = "auxv ok: AT_PHDR points at our program headers\n";
static const char auxv_bad[] = "auxv wrong\n";

/* Walk the auxiliary vector the way a C runtime does: past the environment's
 * NULL terminator, then key/value pairs until AT_NULL. Finding AT_PHDR,
 * AT_PHNUM and AT_PHENT there - and finding a real program header table at the
 * address AT_PHDR gives - is what a real libc needs to start, so checking it
 * from ring 3 is the only proof that matters. */
static int check_auxv(char **envp) {
    unsigned long *aux;
    unsigned long phdr = 0, phnum = 0, phent = 0, at_entry = 0;
    const unsigned char *ph;

    while (*envp) {
        envp++;
    }
    aux = (unsigned long *)(void *)(envp + 1);
    while (aux[0] != 0ul) {
        if (aux[0] == 3ul) { phdr = aux[1]; }
        if (aux[0] == 4ul) { phent = aux[1]; }
        if (aux[0] == 5ul) { phnum = aux[1]; }
        if (aux[0] == 9ul) { at_entry = aux[1]; }
        aux += 2;
    }
    if (phdr == 0ul || phnum == 0ul || phent != 56ul || at_entry == 0ul) {
        return 0;
    }
    /* The first program header of a static executable we loaded is PT_LOAD,
     * and its p_vaddr is where the image starts. */
    ph = (const unsigned char *)phdr;
    if (ph[0] != 1u) {   /* p_type == PT_LOAD, little-endian low byte */
        return 0;
    }
    return 1;
}

static const char abi_ok[] = "linux abi ok: fs/uname/clock/writev/mmap\n";
static const char abi_fs[] = "abi: arch_prctl or %fs broken\n";
static const char abi_uname[] = "abi: uname wrong\n";
static const char abi_clock[] = "abi: clock_gettime wrong\n";
static const char abi_iov[] = "abi: writev wrong\n";
static const char abi_mm[] = "abi: mmap/mprotect/munmap wrong\n";
static const char abi_futex[] = "abi: futex did not check the value\n";
static const char tls_kept[] = "tls survived context switches\n";
static const char tls_lost[] = "abi: %fs lost across a context switch\n";
static const char iov_a[] = "iov";
static const char iov_b[] = "ec\n";

/* The thread-control block a libc would allocate. Its first word is the
 * self-pointer every x86-64 C runtime stores there, which is what makes
 * "%fs:0" mean "the address of my own thread state". */
static unsigned long tcb[16];

struct abi_iovec { const void *base; unsigned long len; };

/* Exercise the syscalls a static libc runs before main, and check the effect
 * rather than the return code: setting %fs is only meaningful if a %fs-relative
 * load afterwards actually reads through it. */
static const char *check_linux_abi(void) {
    unsigned long got = 0;
    unsigned long self;
    char un[6 * 65];
    unsigned long ts[2];
    struct abi_iovec iov[2];
    long r, page;

    tcb[0] = (unsigned long)(void *)tcb;
    if (user_syscall3(SYS_arch_prctl, 0x1002 /*ARCH_SET_FS*/,
                      (long)(unsigned long)tcb, 0) != 0) {
        return abi_fs;
    }
    /* Read the self-pointer back through the segment, not through the C
     * pointer: this is the access a libc makes, and it only works if the
     * kernel really wrote the MSR. */
    __asm__ __volatile__("movq %%fs:0, %0" : "=r"(self));
    if (self != (unsigned long)(void *)tcb) {
        return abi_fs;
    }
    if (user_syscall3(SYS_arch_prctl, 0x1003 /*ARCH_GET_FS*/,
                      (long)(unsigned long)&got, 0) != 0 ||
        got != (unsigned long)(void *)tcb) {
        return abi_fs;
    }
    /* A base outside user memory must be refused - it would fault in ring 0. */
    if (user_syscall3(SYS_arch_prctl, 0x1002, 0x1000, 0) == 0) {
        return abi_fs;
    }

    if (user_syscall3(SYS_uname, (long)(unsigned long)un, 0, 0) != 0) {
        return abi_uname;
    }
    if (un[0] != 'L' || un[1] != 'i' || un[2] != 'n' || un[3] != 'u' ||
        un[4] != 'x' || un[5] != 0) {
        return abi_uname;
    }
    if (un[4 * 65] != 'x' || un[4 * 65 + 1] != '8' || un[4 * 65 + 2] != '6') {
        return abi_uname;   /* machine field must be x86_64 */
    }

    if (user_syscall3(SYS_clock_gettime, 1 /*CLOCK_MONOTONIC*/,
                      (long)(unsigned long)ts, 0) != 0) {
        return abi_clock;
    }
    if (ts[1] >= 1000000000ul) {
        return abi_clock;   /* nanoseconds must be a fraction of a second */
    }

    /* stdout is not a terminal here, and a libc needs to be told so. */
    if (user_syscall3(SYS_ioctl, 1, 0x5401 /*TCGETS*/, 0) != -25 /*ENOTTY*/) {
        return abi_iov;
    }
    iov[0].base = iov_a;
    iov[0].len = sizeof(iov_a) - 1;
    iov[1].base = iov_b;
    iov[1].len = sizeof(iov_b) - 1;
    r = user_syscall3(SYS_writev, 1, (long)(unsigned long)iov, 2);
    if (r != (long)(sizeof(iov_a) - 1 + sizeof(iov_b) - 1)) {
        return abi_iov;
    }

    /* A real mmap/mprotect/munmap round trip: map it, write it, take write
     * away, give it back, then unmap. */
    page = user_syscall6(SYS_mmap, 0, 4096, 3 /*READ|WRITE*/,
                         0x22 /*PRIVATE|ANONYMOUS*/, -1, 0);
    if (page <= 0) {
        return abi_mm;
    }
    *(volatile unsigned char *)(unsigned long)page = 0x5A;
    if (*(volatile unsigned char *)(unsigned long)page != 0x5A) {
        return abi_mm;
    }
    if (user_syscall3(SYS_mprotect, page, 4096, 1 /*READ*/) != 0) {
        return abi_mm;
    }
    if (user_syscall3(SYS_mprotect, page, 4096, 3 /*READ|WRITE*/) != 0) {
        return abi_mm;
    }
    if (user_syscall3(SYS_munmap, page, 4096, 0) != 0) {
        return abi_mm;
    }
    /* An unmapped-but-still-fixed address must not be accepted afterwards. */
    if (user_syscall3(SYS_mprotect, page, 4096, 3) == 0) {
        return abi_mm;
    }
    /* FUTEX_WAIT must compare before it sleeps.
     *
     * That comparison is the whole contract: it is what makes a wake that
     * arrives between reading a word and deciding to sleep impossible to lose.
     * Asking to wait for a value the word does not hold must come straight
     * back with EAGAIN - and if the check were missing this call would block
     * forever instead, which is the failure it exists to prevent, so the test
     * either returns wrong or never returns.
     *
     * Deterministic on purpose: the race itself needs two threads and a
     * particular interleaving, and a test that only sometimes exercises a
     * check is a check that is only sometimes tested. */
    {
        volatile int word = 1;
        long r = user_syscall3(SYS_futex, (unsigned long)&word, 0 /*WAIT*/, 2);

        if (r != -11 /*EAGAIN*/) {
            return abi_futex;
        }
    }

    /* MAP_FIXED is refused rather than silently ignored. */
    if (user_syscall6(SYS_mmap, page, 4096, 3, 0x32 /*FIXED|ANON|PRIVATE*/,
                      -1, 0) > 0) {
        return abi_mm;
    }
    return abi_ok;
}

int vibeos_main(int argc, char **argv, char **envp) {
    long pid, brk0, brk1, map, rejected;

    user_syscall3(SYS_write, 1 /*stdout*/, (long)(unsigned long)message, sizeof(message) - 1);

    /* The kernel built this stack; confirm it arrived intact. */
    if (argc == 1 && argv[0] && argv[0][0] == 'i' && argv[1] == 0) {
        /* argv is well formed and terminated. */
    } else {
        user_syscall3(SYS_write, 1, (long)(unsigned long)argv_bad, sizeof(argv_bad) - 1);
    }
    if (check_auxv(envp)) {
        user_syscall3(SYS_write, 1, (long)(unsigned long)auxv_ok, sizeof(auxv_ok) - 1);
    } else {
        user_syscall3(SYS_write, 1, (long)(unsigned long)auxv_bad, sizeof(auxv_bad) - 1);
    }
    {
        const char *verdict = check_linux_abi();
        long n = 0;
        while (verdict[n]) {
            n++;
        }
        user_syscall3(SYS_write, 1, (long)(unsigned long)verdict, n);
    }

    pid = user_syscall3(SYS_getpid, 0, 0, 0);

    /* Grow the heap, then take an anonymous mapping, and touch both. */
    brk0 = user_syscall3(SYS_brk, 0, 0, 0);
    brk1 = user_syscall3(SYS_brk, brk0 + 8192, 0, 0);
    if (brk1 > brk0) {
        *(volatile char *)(unsigned long)brk0 = 'x';
    }
    map = user_syscall6(SYS_mmap, 0, 4096, 3 /*READ|WRITE*/,
                        0x22 /*PRIVATE|ANONYMOUS*/, -1, 0);
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

    /* fork + exec: the child replaces itself with a different program from disk. */
    {
        long child = user_syscall3(SYS_fork, 0, 0, 0);
        if (child == 0) {
            user_syscall3(SYS_execve, (long)(unsigned long)exec_path, 0, 0);
            user_syscall3(SYS_exit, 99, 0, 0); /* only if exec failed */
        } else if (child > 0) {
            user_syscall3(SYS_wait4, child, 0, 0);
            user_syscall3(SYS_write, 1, (long)(unsigned long)exec_done, sizeof(exec_done) - 1);
        }
    }

    /* Read a line from the console (keyboard) and echo it back. */
    {
        char kb[32];
        long k = user_syscall3(SYS_read, 0 /*stdin*/, (long)(unsigned long)kb, sizeof(kb));
        if (k > 0) {
            user_syscall3(SYS_write, 1, (long)(unsigned long)kbd_prefix, sizeof(kbd_prefix) - 1);
            user_syscall3(SYS_write, 1, (long)(unsigned long)kb, k);
        }
    }

    /* Hand off to the shell, the way init does: run it as a child and wait. */
    {
        long child = user_syscall3(SYS_fork, 0, 0, 0);
        if (child == 0) {
            user_syscall3(SYS_execve, (long)(unsigned long)shell_path, 0, 0);
            user_syscall3(SYS_exit, 127, 0, 0);
        } else if (child > 0) {
            user_syscall3(SYS_wait4, child, 0, 0);
        }
    }

    /* By now this task has forked, waited, and been preempted repeatedly, so
     * it has been switched off this CPU and back many times. Setting %fs is
     * easy; keeping it across those switches is the part that breaks, and it
     * cannot be observed at all without checking here rather than only next
     * to the arch_prctl call. */
    {
        unsigned long self_again;
        __asm__ __volatile__("movq %%fs:0, %0" : "=r"(self_again));
        if (self_again == (unsigned long)(void *)tcb) {
            user_syscall3(SYS_write, 1, (long)(unsigned long)tls_kept,
                          sizeof(tls_kept) - 1);
        } else {
            user_syscall3(SYS_write, 1, (long)(unsigned long)tls_lost,
                          sizeof(tls_lost) - 1);
        }
    }

    user_syscall3(SYS_exit, pid, 0, 0);
    for (;;) {
        /* not reached */
    }
}
