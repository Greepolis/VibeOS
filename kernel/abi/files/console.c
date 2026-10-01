/* The console as a file (docs/abi/ A3): what descriptors 0, 1 and 2 name when a
 * program starts with nothing redirected.
 *
 * Moved from the Linux write and read handlers, where it was the branch taken for
 * descriptors 0-2 when no redirection was recorded. As a type it is reachable
 * from any descriptor that names it - a shell's `2>&1`, a dup of stdin - which
 * the old "0-2 unless redirected" rule could not express. */

#include "files_internal.h"

/* Console writes whose leading bytes read as NUL. */
uint64_t g_ring3_write_nul;

static long console_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    uint64_t i;
    (void)f;

    /* A text write whose leading bytes read as NUL, reported at the moment it
     * happens rather than reconstructed afterwards.
     *
     * Three boots in ten produced `write(ring3): <16 NULs>=120` where the
     * program had written "STRESS_OK rounds=120" from a buffer on its own
     * stack. The kernel reads that buffer directly - there is no copy to blame
     * - so the page it can see holds zeros where the process wrote.
     *
     * Two very different defects produce that, and nothing recorded so far
     * separates them: either the process's store went to a frame this mapping
     * no longer points at, or the store never landed. So the bytes are read a
     * second time. A difference means the page is moving under the kernel; the
     * same zeros twice means they were already zero when the syscall began.
     *
     * Deliberately not a range check on the whole buffer: the signature is
     * leading NULs followed by real text, and a detector that fires on any NUL
     * anywhere would catch every program that writes a binary byte. This
     * project has a rule about detectors that report healthy behaviour. */
    /* Every read of the user's buffer below goes through vibeos_uaccess_copy
     * (M-052): M-040 made the file branch fault-safe and left this one,
     * stdout and stderr, reading the buffer directly - the path every program uses. The
     * range was checked before the handler ran; a sibling's munmap since then
     * made the read fault in ring 0, under the console lock, and panic. */
    uint8_t head[8], tail = 0;
    if (len >= 8u &&
        (vibeos_uaccess_copy(head, (const void *)(uintptr_t)buf, 8u) != 0 ||
         vibeos_uaccess_copy(&tail, (const void *)(uintptr_t)(buf + len - 1u), 1u) != 0)) {
        return -VIBEOS_EFAULT;
    }
    if (len >= 8u && head[0] == 0 && tail != 0) {
        uint64_t a = 0ull, b = 0ull;
        uint8_t again[8];
        uint32_t k;
        for (k = 0; k < 8u; k++) {
            a |= (uint64_t)head[k] << (k * 8u);
        }
        /* Read a second time, from the user's page: a difference is the page
         * moving under the kernel, which is the question this asks. */
        if (vibeos_uaccess_copy(again, (const void *)(uintptr_t)buf, 8u) != 0) {
            return -VIBEOS_EFAULT;
        }
        for (k = 0; k < 8u; k++) {
            b |= (uint64_t)again[k] << (k * 8u);
        }
        g_ring3_write_nul++;
        ks_con_lock();
        ks_con_puts("[MM] RING3_WRITE_NUL task=0x");
        ks_con_hex((uint64_t)(int64_t)ks_current());
        ks_con_puts(" va=0x");
        ks_con_hex(buf);
        ks_con_puts(" len=0x");
        ks_con_hex(len);
        ks_con_puts(" first8=0x");
        ks_con_hex(a);
        ks_con_puts(" again=0x");
        ks_con_hex(b);
        ks_con_puts(" cpu=0x");
        ks_con_hex((uint64_t)ks_cpu_id());
        ks_con_puts(" cr3=0x");
        ks_con_hex(ks_cr3_now());
        ks_con_puts(" tail=0x");
        ks_con_hex((uint64_t)tail);
        /* Every copy-on-write fault this boot took on the corrupted page, in
         * the same critical section as the line above: the two are one fact,
         * and a diagnostic split across calls comes back interleaved from
         * different cores and reads as a contradiction. */
        ks_con_cow_faults(buf & ~0xFFFull);
        ks_con_puts("\n");
        ks_con_unlock();
    }

    /* User output goes to both consoles: the serial line (logs, CI) and the
     * display framebuffer (what a user in front of the machine sees). */
    /* One critical section for the whole line, as before, with the bytes copied
     * in chunks inside it. That is safe under the console lock: a copy that
     * faults resumes at its recovery point before the trap handler prints
     * anything, so the fault path never asks for the lock this core holds. */
    ks_con_lock();
    ks_con_puts("[HW][SYS] write(ring3): ");
    for (i = 0; i < len; ) {
        char chunk[128];
        uint64_t n = len - i, k;
        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (vibeos_uaccess_copy(chunk, (const void *)(uintptr_t)(buf + i), n) != 0) {
            ks_con_puts("\n");
            ks_con_unlock();
            return (i > 0u) ? (long)i : -VIBEOS_EFAULT;
        }
        for (k = 0; k < n; k++) {
            char c = chunk[k];
            if (c == '\n') {
                ks_con_putc('\r');
            }
            ks_con_putc(c);
            ks_console_echo(c);
        }
        i += n;
    }
    ks_con_unlock();
    return (long)len;
}

/* A blocking keyboard read. Returns after at least one character; blocks
 * (BLOCKED + wait_input) until the keyboard IRQ enqueues input and wakes the
 * task. The interrupts-off window makes the check-and-block race-free against
 * the IRQ. */
static long console_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    (void)f;
    if (len == 0u) {
        return 0;
    }
    for (;;) {
        uint64_t copied = 0;
        int c;

        ks_irq_off();
        c = ks_console_getc();
        if (c >= 0) {
            /* Line discipline: echo what was typed and let backspace erase the
             * previous character before the line is handed to the program. */
            while (copied < len && c >= 0) {
                if (c == '\b' || c == 127) {
                    if (copied > 0) {
                        copied--;
                        ks_con_puts("\b \b");
                        ks_console_echo('\b');
                    }
                    c = ks_console_getc();
                    continue;
                }
                {
                    /* The line waits in this loop for keystrokes; the buffer
                     * can be unmapped under it (H-010). */
                    uint8_t ch = (uint8_t)c;
                    if (vibeos_uaccess_copy((void *)(uintptr_t)(buf + copied), &ch, 1u) != 0) {
                        ks_irq_on();
                        return copied > 0u ? (long)copied : -VIBEOS_EFAULT;
                    }
                    copied++;
                }
                /* Under the console lock, like every other writer. Echoing
                 * without it lets a character land in the middle of another
                 * core's write() - which does not merely look untidy: it
                 * splits the markers the boot gate matches on, so a passing
                 * run reports a failure that never happened. */
                ks_con_lock();
                if (c == '\n') {
                    ks_con_putc('\r');
                }
                ks_con_putc((char)c);
                ks_con_unlock();
                ks_console_echo((char)c);
                if ((uint8_t)c == '\n') {
                    break; /* line-oriented: stop at newline */
                }
                c = ks_console_getc();
            }
            ks_irq_on();
            return (long)copied;
        }
        if (ks_current() >= 0) {
            ks_id(ks_current())->wait_input = 1;
            (void)ks_set_state(ks_current(), VIBEOS_TASK_BLOCKED, __func__);
            /* Blocked first and asked second, so a signal raised in between
             * finds the task BLOCKED and wakes it. ks_signal_raise already
             * cleared wait_input for this, and nothing ever read it here. */
            if (ks_signal_interrupts(ks_current())) {
                ks_id(ks_current())->wait_input = 0;
                (void)ks_set_state(ks_current(), VIBEOS_TASK_READY, __func__);
                ks_mark_ready(ks_current(), "read_interrupted");
                ks_irq_on();
                return -VIBEOS_EINTR;
            }
        }
        ks_block_point();
    }
}

/* A character device, and deliberately not a terminal - the same answer ioctl
 * gives for everything but the process group. */
static int console_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    (void)f;
    out->mode = VIBEOS_S_IFCHR | 0620u;
    out->size = 0;
    out->ino = 1;
    return 0;
}

/* The foreground process group is the one terminal question a shell asks and
 * this console answers; ENOTTY for the rest is the truthful answer, and what a
 * libc uses to decide stdout is not a terminal and should be block buffered. */
static long console_ioctl(vibeos_file_t *f, uint64_t req, uint64_t arg) {
    (void)f;
    if (req == VIBEOS_IOCTL_GET_PGRP) {
        uint32_t v = ks_foreground_pgid();
        if (vibeos_uaccess_copy((void *)(uintptr_t)arg, &v, sizeof(v)) != 0) {
            return -VIBEOS_EFAULT;   /* H-025 */
        }
        return 0;
    }
    if (req == VIBEOS_IOCTL_SET_PGRP) {
        uint32_t pgid;
        int me = ks_current(), group;
        if (me < 0) {
            return -VIBEOS_EFAULT;
        }
        if (vibeos_uaccess_copy(&pgid, (const void *)(uintptr_t)arg, sizeof(pgid)) != 0) {
            return -VIBEOS_EFAULT;   /* H-025 */
        }
        group = ks_task_by_pid(pgid);
        if (group < 0 || ks_id(group)->sid != ks_id(me)->sid) {
            return -VIBEOS_EPERM;
        }
        ks_set_foreground_pgid(ks_id(group)->pgid);
        return 0;
    }
    return -VIBEOS_ENOTTY;
}

const vibeos_file_ops_t vibeos_fops_console = {
    .name = "console",
    .read = console_read,
    .write = console_write,
    .stat = console_stat,
    .ioctl = console_ioctl,
};

int vibeos_files_std_console(vibeos_fdtable_t *t) {
    vibeos_file_t *c = vibeos_file_alloc(&vibeos_fops_console, VIBEOS_O_RDWR);
    uint32_t fd;

    if (!c) {
        return -1;
    }
    vibeos_file_get(c);
    vibeos_file_get(c);
    for (fd = 0; fd < 3u; fd++) {
        vibeos_file_t *old = 0;
        if (vibeos_fdtable_install_at(t, fd, c, 0, &old) != 0) {
            /* The references not installed are this function's to give back;
             * the installed ones go with the table. */
            for (; fd < 3u; fd++) {
                vibeos_file_put(c);
            }
            return -1;
        }
        if (old) {
            vibeos_file_put(old);
        }
    }
    return 0;
}
