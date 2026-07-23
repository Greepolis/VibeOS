/* Looping user task for the VibeOS scheduler demo.
 *
 * The kernel passes this task's id in rdi at entry. The task prints a per-id
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

void _start(void) {
    long id;
    __asm__ __volatile__("mov %%rdi, %0" : "=r"(id)); /* task id from the kernel */

    char line[2];
    line[0] = (char)('A' + (int)id);
    line[1] = '\n';

    /* Write a small, bounded number of times (so serial output stays tiny even
     * on slow emulated CI), then spin quietly. The kernel stops the demo after
     * a fixed number of timer preemptions regardless. */
    int writes = 0;
    for (;;) {
        if (writes < 3) {
            user_write(1, line, 2);
            writes++;
        }
        for (volatile long d = 0; d < 300000L; d++) {
            /* short busy-wait so the timer preempts us mid-task */
        }
    }
}
