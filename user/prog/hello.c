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
#define SYS_execve 59

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

    user_syscall3(SYS_exit, pid, 0, 0);
    for (;;) {
        /* not reached */
    }
}
