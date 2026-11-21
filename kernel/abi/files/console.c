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

/* ---- the terminal (docs/abi/ L1 step 7; see vibeos/tty.h) -----------------------------
 *
 * One terminal: its modes, its size, and the line being typed. `len` bytes have
 * been taken from the keyboard; the first `ready` of them may be read - in
 * canonical mode that is whole lines, the rest being a line still open to
 * erasing. The lock is the terminal's own: the modes are set by one program
 * while another reads. */
#define TTY_LINE 256u

static struct {
    vibeos_tty_modes_t m;
    vibeos_tty_size_t size;
    uint8_t line[TTY_LINE];
    uint32_t len;
    uint32_t ready;
    int eof;                      /* end-of-file typed on an empty line */
    uint32_t session;             /* whose controlling terminal it is; 0: nobody's */
    uint32_t sig_due;             /* signals typed, raised once the lock is let go */
} g_tty;
static vibeos_lock_t g_tty_lock;
static int g_tty_set_up;

static void tty_pump(uint64_t want);

static void tty_defaults(void) {
    static const uint8_t cc[VIBEOS_TTY_NCC] = {
        3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0,
    };
    uint32_t i;

    g_tty.m.iflag = VIBEOS_TTY_ICRNL | VIBEOS_TTY_IXON;
    g_tty.m.oflag = VIBEOS_TTY_OPOST | VIBEOS_TTY_ONLCR;
    g_tty.m.cflag = VIBEOS_TTY_CFLAG_DEFAULT;
    g_tty.m.lflag = VIBEOS_TTY_ISIG | VIBEOS_TTY_ICANON | VIBEOS_TTY_ECHO | VIBEOS_TTY_ECHOE |
                    VIBEOS_TTY_ECHOK | VIBEOS_TTY_ECHOCTL | VIBEOS_TTY_ECHOKE | VIBEOS_TTY_IEXTEN;
    g_tty.m.line = 0;
    for (i = 0; i < VIBEOS_TTY_NCC; i++) {
        g_tty.m.cc[i] = cc[i];
    }
    g_tty.size.rows = 25;
    g_tty.size.cols = 80;
    g_tty.size.xpixel = g_tty.size.ypixel = 0;
    g_tty.len = g_tty.ready = 0;
    g_tty.eof = 0;
    g_tty.session = 0;
    g_tty.sig_due = 0;
    g_tty_set_up = 1;
}

/* A terminal nobody reset still has modes: the first use gives it the ones it
 * starts with, so no caller has to be the one that remembered. */
static void tty_lock(void) {
    ks_lock(&g_tty_lock, "vibeos_tty");
    if (!g_tty_set_up) {
        tty_defaults();
    }
}

/* A live task, for what is sent to a group. */
static int tty_live(int i) {
    vibeos_task_state_t st = vibeos_task_state((uint32_t)i);

    return ks_id(i)->is_user && st != VIBEOS_TASK_FREE && st != VIBEOS_TASK_SETUP && st != VIBEOS_TASK_ZOMBIE;
}

/* To each process of the group once - its leader, or its first live thread
 * when the leader has gone: a handler runs once for a Ctrl+C, not once a
 * thread. Under the scheduler's lock. */
static void tty_send_group(uint32_t pgid, uint32_t sig) {
    int i, j, n = (int)ks_slots();

    for (i = 0; pgid != 0u && i < n; i++) {
        int first = 1;

        if (!tty_live(i) || ks_id(i)->pgid != pgid) {
            continue;
        }
        for (j = 0; j < n && ks_id(i)->pid != ks_id(i)->tgid; j++) {
            if (j != i && tty_live(j) && ks_id(j)->tgid == ks_id(i)->tgid &&
                (ks_id(j)->pid == ks_id(j)->tgid || j < i)) {
                first = 0;
                break;
            }
        }
        if (first) {
            (void)ks_signal_raise(i, sig);
        }
    }
}

/* Raising takes the scheduler's lock, and a program waiting holds it while it
 * asks about signals: never under the terminal's. */
static void tty_unlock(void) {
    uint32_t due = g_tty.sig_due, sig;

    g_tty.sig_due = 0;
    ks_unlock(&g_tty_lock);
    if (due != 0u) {
        ks_lock(ks_sched_lock(), __func__);
        for (sig = 1; sig < 32u; sig++) {
            if (due & (1u << sig)) {
                tty_send_group(ks_foreground_pgid(), sig);
            }
        }
        ks_unlock(ks_sched_lock());
    }
}

void vibeos_tty_reset(void) {
    ks_lock(&g_tty_lock, "vibeos_tty");
    tty_defaults();
    ks_unlock(&g_tty_lock);
}

void vibeos_tty_get(vibeos_tty_modes_t *out) {
    tty_lock();
    *out = g_tty.m;
    tty_unlock();
}

void vibeos_tty_set(const vibeos_tty_modes_t *in, int flush) {
    tty_lock();
    g_tty.m = *in;
    if (flush) {
        g_tty.len = g_tty.ready = 0;
        g_tty.eof = 0;
    }
    /* Leaving canonical mode with a line half typed: those bytes were typed,
     * and a program reading a byte at a time is owed them. */
    if (!(g_tty.m.lflag & VIBEOS_TTY_ICANON)) {
        g_tty.ready = g_tty.len;
    }
    tty_unlock();
}

void vibeos_tty_get_size(vibeos_tty_size_t *out) {
    tty_lock();
    *out = g_tty.size;
    tty_unlock();
}

void vibeos_tty_set_size(const vibeos_tty_size_t *in) {
    tty_lock();
    g_tty.size = *in;
    tty_unlock();
}

uint32_t vibeos_tty_pending(void) {
    uint32_t n;
    tty_lock();
    tty_pump(TTY_LINE);   /* what was typed counts whether or not a read has looked yet */
    n = g_tty.ready;
    tty_unlock();
    return n;
}

/* What typing shows. Under the console lock, like every other writer: echoing
 * without it lets a character land in the middle of another core's write() -
 * which does not merely look untidy, it splits the markers the boot gate
 * matches on, so a passing run reports a failure that never happened. */
static void tty_echo(char c) {
    ks_con_lock();
    if (c == '\n') {
        ks_con_putc('\r');
    }
    ks_con_putc(c);
    ks_con_unlock();
    ks_console_echo(c);
}

static void tty_rub_out(void) {
    ks_con_lock();
    ks_con_puts("\b \b");
    ks_con_unlock();
    ks_console_echo('\b');
}

/* The signal a typed character stands for under the present modes, or 0. A
 * character set to 0 is disabled, as POSIX's _POSIX_VDISABLE. */
static uint32_t tty_sig_of(uint8_t c) {
    if (!(g_tty.m.lflag & VIBEOS_TTY_ISIG) || c == 0u) {
        return 0;
    }
    if (c == g_tty.m.cc[VIBEOS_TTY_VINTR]) {
        return VIBEOS_SIGINT;
    }
    if (c == g_tty.m.cc[VIBEOS_TTY_VQUIT]) {
        return VIBEOS_SIGQUIT;
    }
    if (c == g_tty.m.cc[VIBEOS_TTY_VSUSP]) {
        return VIBEOS_SIGTSTP;
    }
    return 0;
}

/* Under the terminal's lock: what was typed goes unless NOFLSH, the character
 * is shown as ^C, and the signal is raised when the lock is let go. */
static void tty_take_signal(uint8_t c, uint32_t sig) {
    if (!(g_tty.m.lflag & VIBEOS_TTY_NOFLSH)) {
        g_tty.len = g_tty.ready = 0;
        g_tty.eof = 0;
    }
    if ((g_tty.m.lflag & VIBEOS_TTY_ECHO) && (g_tty.m.lflag & VIBEOS_TTY_ECHOCTL)) {
        tty_echo('^');
        tty_echo((char)((c + 64u) & 0x7Fu));
    }
    g_tty.sig_due |= 1u << sig;
}

int vibeos_tty_control(uint8_t c) {
    uint32_t sig;

    tty_lock();
    sig = tty_sig_of(c);
    if (sig != 0u) {
        tty_take_signal(c, sig);
    }
    tty_unlock();
    return sig != 0u;
}

void vibeos_tty_set_session(uint32_t sid) {
    tty_lock();
    g_tty.session = sid;
    tty_unlock();
}

long vibeos_tty_job_check(uint32_t sig) {
    int me = ks_current();
    vibeos_task_t *t;
    vibeos_procstate_t *ps;
    uint32_t session, fg;
    int held, orphan;

    if (me < 0 || !ks_id(me)->is_user) {
        return 0;
    }
    tty_lock();
    session = g_tty.session;
    tty_unlock();
    t = ks_id(me);
    ps = ks_ps(me);
    ks_lock(ks_sched_lock(), __func__);
    fg = ks_foreground_pgid();
    /* Not its controlling terminal, or its job is the foreground: nothing to
     * decide. A descriptor on a terminal another session owns is not job
     * control's business, as on Linux. */
    if (session == 0u || t->sid != session || fg == 0u || t->pgid == fg) {
        ks_unlock(ks_sched_lock());
        return 0;
    }
    held = (t->sig_blocked & (1ull << sig)) != 0u || (ps && ps->sig_handler[sig] == SIG_IGN_ADDR);
    orphan = ks_pgrp_orphaned(t->pgid);
    if (held) {
        /* Refused the signal: a write or a change goes through, as POSIX
         * says, and a read cannot, having nothing to give. */
        ks_unlock(ks_sched_lock());
        return sig == VIBEOS_SIGTTOU ? 0 : -VIBEOS_EIO;
    }
    if (orphan) {
        ks_unlock(ks_sched_lock());
        return -VIBEOS_EIO;   /* nobody could continue it */
    }
    tty_send_group(t->pgid, sig);
    ks_unlock(ks_sched_lock());
    return -VIBEOS_RESTART_CALL;   /* issued again once it is continued */
}

/* Take what the keyboard has, as far as this read needs it: in canonical mode
 * until a line is finished, otherwise until `want` bytes are there. No further
 * - what is typed ahead stays in the keyboard's queue for whoever reads next,
 * in whatever mode the terminal is in by then. Called under the terminal's
 * lock. */
static void tty_pump(uint64_t want) {
    const uint32_t lf = g_tty.m.lflag;
    const int canon = (lf & VIBEOS_TTY_ICANON) != 0u;
    const int echo = (lf & VIBEOS_TTY_ECHO) != 0u;

    for (;;) {
        int c;

        if (g_tty.eof || g_tty.len >= TTY_LINE) {
            break;
        }
        if (canon ? g_tty.ready > 0u : (uint64_t)g_tty.ready >= want) {
            break;
        }
        c = ks_console_getc();
        if (c < 0) {
            break;
        }
        if (c == '\r' && (g_tty.m.iflag & VIBEOS_TTY_ICRNL)) {
            c = '\n';
        }
        /* From a queue, not the key itself: the boot's injected script and a
         * serial line arrive this way. */
        if (tty_sig_of((uint8_t)c) != 0u) {
            /* And no further: what follows is for whoever reads after the
             * signal has acted - a killed cat must not take the shell's next
             * line with it. */
            tty_take_signal((uint8_t)c, tty_sig_of((uint8_t)c));
            break;
        }
        if (!canon) {
            g_tty.line[g_tty.len++] = (uint8_t)c;
            g_tty.ready = g_tty.len;
            if (echo) {
                tty_echo((char)c);
            }
            continue;
        }
        /* Erase: the character before, if the open line has one. Backspace is
         * taken as well as the erase character - a PC keyboard sends the one,
         * a serial console the other. */
        if (c == (int)g_tty.m.cc[VIBEOS_TTY_VERASE] || c == '\b') {
            if (g_tty.len > g_tty.ready) {
                g_tty.len--;
                if (echo && (lf & VIBEOS_TTY_ECHOE)) {
                    tty_rub_out();
                }
            }
            continue;
        }
        if (c == (int)g_tty.m.cc[VIBEOS_TTY_VKILL]) {
            while (g_tty.len > g_tty.ready) {
                g_tty.len--;
                if (echo && (lf & VIBEOS_TTY_ECHOE)) {
                    tty_rub_out();
                }
            }
            continue;
        }
        /* End of file: what is typed so far is given to the reader as it is,
         * and on an empty line that is nothing - a read of 0, which is how a
         * program is told there is no more. */
        if (c == (int)g_tty.m.cc[VIBEOS_TTY_VEOF]) {
            if (g_tty.len == g_tty.ready) {
                g_tty.eof = 1;
            }
            g_tty.ready = g_tty.len;
            continue;
        }
        g_tty.line[g_tty.len++] = (uint8_t)c;
        if (echo || (c == '\n' && (lf & VIBEOS_TTY_ECHONL))) {
            tty_echo((char)c);
        }
        if (c == '\n' || g_tty.len == TTY_LINE) {
            g_tty.ready = g_tty.len;   /* a line; or one too long to wait for its end */
        }
    }
}

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

/* A read from the terminal. In canonical mode it returns one finished line -
 * or the part of it that fits, the rest waiting for the next read - and waits
 * until there is one: it used to hand back whatever had been typed when the
 * keyboard went quiet, half a line included. Otherwise it returns as soon as
 * MIN bytes are there, which with MIN 0 is at once and possibly with nothing.
 *
 * Blocks (BLOCKED + wait_input) until the keyboard IRQ enqueues input and
 * wakes the task. The interrupts-off window makes the check-and-block
 * race-free against the IRQ; the terminal's lock is inside it and gives the
 * interrupt state back as it found it. */
static long console_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    if (len == 0u) {
        return 0;
    }
    for (;;) {
        uint32_t n = 0, min, i;
        int canon, done = 0;
        long job = vibeos_tty_job_check(VIBEOS_SIGTTIN);   /* each time: it may have been moved */

        if (job != 0) {
            return job;
        }
        ks_irq_off();
        tty_lock();
        tty_pump(len);
        canon = (g_tty.m.lflag & VIBEOS_TTY_ICANON) != 0u;
        min = g_tty.m.cc[VIBEOS_TTY_VMIN];
        if (g_tty.ready > 0u) {
            n = g_tty.ready;
            if (canon) {
                /* One line a read: up to and including its newline. */
                for (i = 0; i < g_tty.ready; i++) {
                    if (g_tty.line[i] == '\n') {
                        n = i + 1u;
                        break;
                    }
                }
            }
            if ((uint64_t)n > len) {
                n = (uint32_t)len;
            }
            if (canon || n >= min || (uint64_t)n == len) {
                /* The buffer can be unmapped while the line was being waited
                 * for (H-010); the copy is fault-safe and the bytes stay. */
                if (vibeos_uaccess_copy((void *)(uintptr_t)buf, g_tty.line, n) != 0) {
                    tty_unlock();
                    ks_irq_on();
                    return -VIBEOS_EFAULT;
                }
                for (i = n; i < g_tty.len; i++) {
                    g_tty.line[i - n] = g_tty.line[i];
                }
                g_tty.len -= n;
                g_tty.ready -= n;
                done = 1;
            }
        } else if (g_tty.eof) {
            g_tty.eof = 0;
            done = 1;                     /* end of file: a read of nothing */
        } else if (!canon && min == 0u) {
            done = 1;                     /* MIN 0: whatever there is, now */
        }
        tty_unlock();
        if (done) {
            ks_irq_on();
            return (long)n;
        }
        if (f->flags & VIBEOS_O_NONBLOCK) {
            ks_irq_on();
            return -VIBEOS_EAGAIN;
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
                return -VIBEOS_RESTART_CALL;
            }
        }
        ks_block_point();
    }
}

/* Would a read return now? In canonical mode only once a line is finished -
 * a key typed is not yet something to read - and otherwise as soon as MIN bytes
 * are there. Asking takes what the keyboard has into the terminal's line, the
 * same as a read would have. Output never waits. */
static uint32_t console_ready(vibeos_file_t *f) {
    uint32_t r = VIBEOS_READY_OUT, min;
    (void)f;

    tty_lock();
    min = g_tty.m.cc[VIBEOS_TTY_VMIN];
    tty_pump(min ? min : 1u);
    if (g_tty.eof || ((g_tty.m.lflag & VIBEOS_TTY_ICANON) ? g_tty.ready > 0u
                                                          : g_tty.ready >= (min ? min : 1u))) {
        r |= VIBEOS_READY_IN;
    }
    tty_unlock();
    return r;
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

/* The foreground process group, which is the console's to answer because the
 * kernel services hold it. The terminal's modes and size are asked through
 * vibeos/tty.h by the personality, which owns the request numbers and the
 * structures they carry. */
static long console_ioctl(vibeos_file_t *f, uint64_t req, uint64_t arg) {
    uint32_t session;

    (void)f;
    tty_lock();
    session = g_tty.session;
    tty_unlock();
    /* These are questions about the caller's controlling terminal. A terminal
     * nobody holds yet answers as it did, for what runs before a session. */
    if ((req == VIBEOS_IOCTL_GET_PGRP || req == VIBEOS_IOCTL_SET_PGRP || req == VIBEOS_IOCTL_GSID ||
         req == VIBEOS_IOCTL_NOTTY) &&
        ks_current() >= 0 && ks_id(ks_current())->is_user && session != 0u &&
        ks_id(ks_current())->sid != session) {
        return -VIBEOS_ENOTTY;
    }
    if (req == VIBEOS_IOCTL_GSID) {
        if (session == 0u) {
            return -VIBEOS_ENOTTY;
        }
        return vibeos_uaccess_copy((void *)(uintptr_t)arg, &session, sizeof(session)) != 0 ? -VIBEOS_EFAULT : 0;
    }
    if (req == VIBEOS_IOCTL_SCTTY) {
        vibeos_task_t *t = ks_current() >= 0 ? ks_id(ks_current()) : 0;
        vibeos_procstate_t *ps = ks_current() >= 0 ? ks_ps(ks_current()) : 0;
        int i, held = 0;

        if (!t || t->sid != t->tgid) {
            return t && t->sid == session ? 0 : -VIBEOS_EPERM;   /* a session leader's to take */
        }
        if (session == t->sid) {
            return 0;
        }
        ks_lock(ks_sched_lock(), __func__);
        for (i = 0; session != 0u && i < (int)ks_slots(); i++) {
            held |= tty_live(i) && ks_id(i)->sid == session;
        }
        /* Held by a live session: only the superuser, asking to steal it. */
        if (held && !((uint32_t)arg == 1u && ps && ps->cred.euid == 0u)) {
            ks_unlock(ks_sched_lock());
            return -VIBEOS_EPERM;
        }
        ks_set_foreground_pgid(t->pgid);
        ks_unlock(ks_sched_lock());
        vibeos_tty_set_session(t->sid);
        return 0;
    }
    if (req == VIBEOS_IOCTL_NOTTY) {
        vibeos_task_t *t = ks_current() >= 0 ? ks_id(ks_current()) : 0;

        if (session == 0u || !t) {
            return -VIBEOS_ENOTTY;
        }
        /* The leader giving it up ends the session's hold: the foreground
         * job is told, as when a terminal goes away. */
        if (t->sid == t->tgid) {
            ks_lock(ks_sched_lock(), __func__);
            tty_send_group(ks_foreground_pgid(), VIBEOS_SIGHUP);
            tty_send_group(ks_foreground_pgid(), VIBEOS_SIGCONT);
            ks_set_foreground_pgid(0);
            ks_unlock(ks_sched_lock());
            vibeos_tty_set_session(0);
        }
        return 0;
    }
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
        {
            long job = vibeos_tty_job_check(VIBEOS_SIGTTOU);   /* a background job asking is stopped */
            if (job != 0) {
                return job;
            }
        }
        /* Found, judged and used in one hold of the scheduler's lock: an
         * index from ks_task_by_pid is the slot's for only as long as that
         * lock is held. Released between the session check and the read of
         * the group, a reaped target's slot could be another session's task
         * by then, and Control-C would go to it (issue #191). */
        ks_lock(ks_sched_lock(), __func__);
        group = ks_task_by_pid(pgid);
        if (group < 0 || ks_id(group)->sid != ks_id(me)->sid) {
            ks_unlock(ks_sched_lock());
            return -VIBEOS_EPERM;
        }
        ks_set_foreground_pgid(ks_id(group)->pgid);
        ks_unlock(ks_sched_lock());
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
