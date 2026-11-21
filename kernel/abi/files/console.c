/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The console as a file (docs/abi/ A3): what descriptors 0, 1 and 2 name when a
 * program starts with nothing redirected.
 *
 * Moved from the Linux write and read handlers, where it was the branch taken for
 * descriptors 0-2 when no redirection was recorded. As a type it is reachable
 * from any descriptor that names it - a shell's `2>&1`, a dup of stdin - which
 * the old "0-2 unless redirected" rule could not express. */

#include "files_internal.h"
#include "vibeos/devfs.h"
#include "vibeos/tty.h"

/* ---- the terminal behind it (docs/shell/s1.md step 3; see vibeos/tty.h) ------------- */

/* What typing shows. Under the console lock, like every other writer: echoing
 * without it lets a character land in the middle of another core's write() -
 * which does not merely look untidy, it splits the markers the boot gate
 * matches on, so a passing run reports a failure that never happened. */
static void con_echo(void *ctx, char c) {
    (void)ctx;
    ks_con_lock();
    if (c == '\n') {
        ks_con_putc('\r');
    }
    ks_con_putc(c);
    ks_con_unlock();
    ks_console_echo(c);
}

static void con_rub(void *ctx) {
    (void)ctx;
    ks_con_lock();
    ks_con_puts("\b \b");
    ks_con_unlock();
    ks_console_echo('\b');
}

static int con_getc(void *ctx) {
    (void)ctx;
    return ks_console_getc();
}

static uint32_t con_get_fg(void *ctx) {
    (void)ctx;
    return ks_foreground_pgid();
}

static void con_set_fg(void *ctx, uint32_t pgid) {
    (void)ctx;
    ks_set_foreground_pgid(pgid);
}

static const vibeos_term_ops_t g_console_ops = {
    .getc = con_getc, .echo = con_echo, .rub = con_rub, .get_fg = con_get_fg, .set_fg = con_set_fg,
};
static vibeos_term_t g_console = {.ops = &g_console_ops};

/* The console's terminal, for the callers that have only ever had the one. */
void vibeos_tty_reset(void) { vibeos_term_reset(&g_console); }
void vibeos_tty_get(vibeos_tty_modes_t *out) { vibeos_term_get(&g_console, out); }
void vibeos_tty_set(const vibeos_tty_modes_t *in, int flush) { vibeos_term_set(&g_console, in, flush); }
void vibeos_tty_get_size(vibeos_tty_size_t *out) { vibeos_term_get_size(&g_console, out); }
void vibeos_tty_set_size(const vibeos_tty_size_t *in) { vibeos_term_set_size(&g_console, in); }
uint32_t vibeos_tty_pending(void) { return vibeos_term_pending(&g_console); }
int vibeos_tty_control(uint8_t c) { return vibeos_term_control(&g_console, c); }
void vibeos_tty_set_session(uint32_t sid) { vibeos_term_set_session(&g_console, sid); }
long vibeos_tty_job_check(uint32_t sig) { return vibeos_term_job_check(&g_console, sig); }

/* Console writes whose leading bytes read as NUL. */
uint64_t g_ring3_write_nul;

static long console_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    uint64_t i;
    vibeos_tty_modes_t modes;
    int onlcr;
    (void)f;

    /* A newline goes out as CR LF unless the program turned that off - which
     * one drawing the screen itself does, and then sends its own. */
    vibeos_tty_get(&modes);
    onlcr = (modes.oflag & VIBEOS_TTY_OPOST) && (modes.oflag & VIBEOS_TTY_ONLCR);
    if (modes.lflag & VIBEOS_TTY_TOSTOP) {
        long job = vibeos_tty_job_check(VIBEOS_SIGTTOU);
        if (job != 0) {
            return job;
        }
    }

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
            if (c == '\n' && onlcr) {
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

static long console_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    return vibeos_term_read(&g_console, buf, len, (f->flags & VIBEOS_O_NONBLOCK) != 0u);
}

static uint32_t console_ready(vibeos_file_t *f) {
    (void)f;
    return vibeos_term_ready(&g_console);
}

/* A character device, as a terminal is. */
static int console_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    out->mode = VIBEOS_S_IFCHR | 0620u;
    out->size = 0;
    /* Opened by name (/dev/tty, /dev/console, L2 step 6), the node it was
     * opened through, so fstat of the descriptor and stat of the name agree -
     * which is how ttyname() decides a name is the terminal's. */
    out->ino = f->node ? f->node : 1u;
    out->rdev = f->rdev ? f->rdev : VIBEOS_DEV_CONSOLE;
    return 0;
}

static long console_ioctl(vibeos_file_t *f, uint64_t req, uint64_t arg) {
    (void)f;
    return vibeos_term_ioctl(&g_console, req, arg);
}

const vibeos_file_ops_t vibeos_fops_console = {
    .name = "console",
    .read = console_read,
    .write = console_write,
    .stat = console_stat,
    .ioctl = console_ioctl,
    .ready = console_ready,
};

int vibeos_files_std_console(vibeos_fdtable_t *t) {
    vibeos_file_t *c = vibeos_file_alloc(&vibeos_fops_console, VIBEOS_O_RDWR);
    uint32_t fd, left;

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
            for (left = fd; left < 3u; left++) {
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
