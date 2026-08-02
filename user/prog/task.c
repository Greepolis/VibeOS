/* Looping user task for the VibeOS scheduler demo.
 *
 * The kernel passes this task's id as argv[0], the way any program gets its
 * arguments. The task prints a per-id
 * letter (id 0 -> 'A', id 1 -> 'B', ...) in an endless loop with a busy-wait
 * delay, so the periodic timer interrupt preempts it and two copies interleave.
 * It never exits; the kernel stops the demo after a bounded number of context
 * switches and returns to its boot flow.
 */

static long user_write(long fd, const char *buf, long len) {
    long ret;
    __asm__ __volatile__("syscall"
                         : "=a"(ret)
                         : "a"(1L), "D"(fd), "S"(buf), "d"(len)
                         : "rcx", "r11", "memory");
    return ret;
}

static void user_exit(long code) {
    __asm__ __volatile__("syscall" : : "a"(60L), "D"(code) : "rcx", "r11", "memory");
    for (;;) {
        /* not reached */
    }
}

int vibeos_main(int argc, char **argv, char **envp) {
    /* argv[0] is a single digit chosen by the kernel: task 0 prints 'A',
     * task 1 prints 'B', and so on. */
    long id = 0;
    char line[2];
    int writes;

    (void)envp;
    if (argc > 0 && argv[0] && argv[0][0] >= '0' && argv[0][0] <= '9') {
        id = (long)(argv[0][0] - '0');
    }

    line[0] = (char)('A' + (int)id);
    line[1] = '\n';

    /* Write a small, bounded number of times (so serial output stays tiny even
     * on slow emulated CI), with a short busy-wait so the timer preempts us
     * mid-task, then exit - the kernel retires us and schedules someone else. */
    for (writes = 0; writes < 3; writes++) {
        user_write(1, line, 2);
        for (volatile long d = 0; d < 300000L; d++) {
            /* busy-wait */
        }
    }
    user_exit(id);
    return 0;   /* not reached */
}
