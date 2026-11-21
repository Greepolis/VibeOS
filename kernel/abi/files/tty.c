/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The line discipline (docs/shell/s1.md step 3): what makes a stream of typed
 * bytes a terminal - modes, the line being typed and its editing, the signal
 * characters, the foreground group and the session.
 *
 * Moved out of the console, whose one static it was, so that a terminal is an
 * instance fed by whatever is behind it: the console, behind the keyboard and
 * the framebuffer, and a pseudo-terminal's master (step 4). Here and not in
 * kernel/core because it stops and signals process groups, which is the
 * scheduler's, reached through vibeos/ksvc.h as every file type reaches it.
 *
 * `len` bytes have been taken from the backend; the first `ready` of them may
 * be read - in canonical mode that is whole lines, the rest being a line still
 * open to erasing. The lock is the instance's own: the modes are set by one
 * program while another reads. */

#include "files_internal.h"
#include "vibeos/fileops.h"
#include "vibeos/tty.h"

/* The foreground group: the backend's when it keeps it elsewhere - the
 * console's is read by /proc and set at boot - else the instance's. */
static uint32_t tty_fg(const vibeos_term_t *t) {
    return t->ops->get_fg ? t->ops->get_fg(t->ctx) : t->fg;
}

static void tty_set_fg(vibeos_term_t *t, uint32_t pgid) {
    if (t->ops->set_fg) {
        t->ops->set_fg(t->ctx, pgid);
    } else {
        t->fg = pgid;
    }
}

static void tty_pump(vibeos_term_t *t, uint64_t want);

static void tty_defaults(vibeos_term_t *t) {
    static const uint8_t cc[VIBEOS_TTY_NCC] = {
        3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22, 0, 0, 0,
    };
    uint32_t i;

    t->m.iflag = VIBEOS_TTY_ICRNL | VIBEOS_TTY_IXON;
    t->m.oflag = VIBEOS_TTY_OPOST | VIBEOS_TTY_ONLCR;
    t->m.cflag = VIBEOS_TTY_CFLAG_DEFAULT;
    t->m.lflag = VIBEOS_TTY_ISIG | VIBEOS_TTY_ICANON | VIBEOS_TTY_ECHO | VIBEOS_TTY_ECHOE |
                    VIBEOS_TTY_ECHOK | VIBEOS_TTY_ECHOCTL | VIBEOS_TTY_ECHOKE | VIBEOS_TTY_IEXTEN;
    t->m.line = 0;
    for (i = 0; i < VIBEOS_TTY_NCC; i++) {
        t->m.cc[i] = cc[i];
    }
    t->size.rows = 25;
    t->size.cols = 80;
    t->size.xpixel = t->size.ypixel = 0;
    t->len = t->ready = 0;
    t->eof = 0;
    t->session = 0;
    t->sig_due = 0;
    t->fg = 0;
    t->set_up = 1;
}

/* A terminal nobody reset still has modes: the first use gives it the ones it
 * starts with, so no caller has to be the one that remembered. */
static void tty_lock(vibeos_term_t *t) {
    ks_lock(&t->lock, "vibeos_tty");
    if (!t->set_up) {
        tty_defaults(t);
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
static void tty_unlock(vibeos_term_t *t) {
    uint32_t due = t->sig_due, sig;

    t->sig_due = 0;
    ks_unlock(&t->lock);
    if (due != 0u) {
        ks_lock(ks_sched_lock(), __func__);
        for (sig = 1; sig < 32u; sig++) {
            if (due & (1u << sig)) {
                tty_send_group(tty_fg(t), sig);
            }
        }
        ks_unlock(ks_sched_lock());
    }
}

void vibeos_term_reset(vibeos_term_t *t) {
    ks_lock(&t->lock, "vibeos_tty");
    tty_defaults(t);
    ks_unlock(&t->lock);
}

void vibeos_term_get(vibeos_term_t *t, vibeos_tty_modes_t *out) {
    tty_lock(t);
    *out = t->m;
    tty_unlock(t);
}

void vibeos_term_set(vibeos_term_t *t, const vibeos_tty_modes_t *in, int flush) {
    tty_lock(t);
    t->m = *in;
    if (flush) {
        t->len = t->ready = 0;
        t->eof = 0;
    }
    /* Leaving canonical mode with a line half typed: those bytes were typed,
     * and a program reading a byte at a time is owed them. */
    if (!(t->m.lflag & VIBEOS_TTY_ICANON)) {
        t->ready = t->len;
    }
    tty_unlock(t);
}

void vibeos_term_get_size(vibeos_term_t *t, vibeos_tty_size_t *out) {
    tty_lock(t);
    *out = t->size;
    tty_unlock(t);
}

void vibeos_term_set_size(vibeos_term_t *t, const vibeos_tty_size_t *in) {
    tty_lock(t);
    t->size = *in;
    tty_unlock(t);
}

uint32_t vibeos_term_pending(vibeos_term_t *t) {
    uint32_t n;
    tty_lock(t);
    tty_pump(t, VIBEOS_TERM_LINE);   /* what was typed counts whether or not a read has looked yet */
    n = t->ready;
    tty_unlock(t);
    return n;
}

/* What typing shows and an erase rubs out: the backend's, because where it
 * appears is what differs between a console and a pseudo-terminal. */
static void tty_echo(vibeos_term_t *t, char c) {
    t->ops->echo(t->ctx, c);
}

static void tty_rub_out(vibeos_term_t *t) {
    t->ops->rub(t->ctx);
}

/* The signal a typed character stands for under the present modes, or 0. A
 * character set to 0 is disabled, as POSIX's _POSIX_VDISABLE. */
static uint32_t tty_sig_of(vibeos_term_t *t, uint8_t c) {
    if (!(t->m.lflag & VIBEOS_TTY_ISIG) || c == 0u) {
        return 0;
    }
    if (c == t->m.cc[VIBEOS_TTY_VINTR]) {
        return VIBEOS_SIGINT;
    }
    if (c == t->m.cc[VIBEOS_TTY_VQUIT]) {
        return VIBEOS_SIGQUIT;
    }
    if (c == t->m.cc[VIBEOS_TTY_VSUSP]) {
        return VIBEOS_SIGTSTP;
    }
    return 0;
}

/* Under the terminal's lock: what was typed goes unless NOFLSH, the character
 * is shown as ^C, and the signal is raised when the lock is let go. */
static void tty_take_signal(vibeos_term_t *t, uint8_t c, uint32_t sig) {
    if (!(t->m.lflag & VIBEOS_TTY_NOFLSH)) {
        t->len = t->ready = 0;
        t->eof = 0;
    }
    if ((t->m.lflag & VIBEOS_TTY_ECHO) && (t->m.lflag & VIBEOS_TTY_ECHOCTL)) {
        tty_echo(t, '^');
        tty_echo(t, (char)((c + 64u) & 0x7Fu));
    }
    t->sig_due |= 1u << sig;
}

int vibeos_term_control(vibeos_term_t *t, uint8_t c) {
    uint32_t sig;

    tty_lock(t);
    sig = tty_sig_of(t, c);
    if (sig != 0u) {
        tty_take_signal(t, c, sig);
    }
    tty_unlock(t);
    return sig != 0u;
}

void vibeos_term_set_session(vibeos_term_t *t, uint32_t sid) {
    tty_lock(t);
    t->session = sid;
    tty_unlock(t);
}

long vibeos_term_job_check(vibeos_term_t *t, uint32_t sig) {
    int me = ks_current();
    vibeos_task_t *who;
    vibeos_procstate_t *ps;
    uint32_t session, fg;
    int held, orphan;

    if (me < 0 || !ks_id(me)->is_user) {
        return 0;
    }
    tty_lock(t);
    session = t->session;
    tty_unlock(t);
    who = ks_id(me);
    ps = ks_ps(me);
    ks_lock(ks_sched_lock(), __func__);
    fg = tty_fg(t);
    /* Not its controlling terminal, or its job is the foreground: nothing to
     * decide. A descriptor on a terminal another session owns is not job
     * control's business, as on Linux. */
    if (session == 0u || who->sid != session || fg == 0u || who->pgid == fg) {
        ks_unlock(ks_sched_lock());
        return 0;
    }
    held = (who->sig_blocked & (1ull << sig)) != 0u || (ps && ps->sig_handler[sig] == SIG_IGN_ADDR);
    orphan = ks_pgrp_orphaned(who->pgid);
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
    tty_send_group(who->pgid, sig);
    ks_unlock(ks_sched_lock());
    return -VIBEOS_RESTART_CALL;   /* issued again once it is continued */
}

/* Take what the keyboard has, as far as this read needs it: in canonical mode
 * until a line is finished, otherwise until `want` bytes are there. No further
 * - what is typed ahead stays in the keyboard's queue for whoever reads next,
 * in whatever mode the terminal is in by then. Called under the terminal's
 * lock. */
static void tty_pump(vibeos_term_t *t, uint64_t want) {
    const uint32_t lf = t->m.lflag;
    const int canon = (lf & VIBEOS_TTY_ICANON) != 0u;
    const int echo = (lf & VIBEOS_TTY_ECHO) != 0u;

    for (;;) {
        int c;

        if (t->eof || t->len >= VIBEOS_TERM_LINE) {
            break;
        }
        if (canon ? t->ready > 0u : (uint64_t)t->ready >= want) {
            break;
        }
        c = t->ops->getc(t->ctx);
        if (c < 0) {
            break;
        }
        if (c == '\r' && (t->m.iflag & VIBEOS_TTY_ICRNL)) {
            c = '\n';
        }
        /* From a queue, not the key itself: the boot's injected script and a
         * serial line arrive this way. */
        if (tty_sig_of(t, (uint8_t)c) != 0u) {
            /* And no further: what follows is for whoever reads after the
             * signal has acted - a killed cat must not take the shell's next
             * line with it. */
            tty_take_signal(t, (uint8_t)c, tty_sig_of(t, (uint8_t)c));
            break;
        }
        if (!canon) {
            t->line[t->len++] = (uint8_t)c;
            t->ready = t->len;
            if (echo) {
                tty_echo(t, (char)c);
            }
            continue;
        }
        /* Erase: the character before, if the open line has one. Backspace is
         * taken as well as the erase character - a PC keyboard sends the one,
         * a serial console the other. */
        if (c == (int)t->m.cc[VIBEOS_TTY_VERASE] || c == '\b') {
            if (t->len > t->ready) {
                t->len--;
                if (echo && (lf & VIBEOS_TTY_ECHOE)) {
                    tty_rub_out(t);
                }
            }
            continue;
        }
        if (c == (int)t->m.cc[VIBEOS_TTY_VKILL]) {
            while (t->len > t->ready) {
                t->len--;
                if (echo && (lf & VIBEOS_TTY_ECHOE)) {
                    tty_rub_out(t);
                }
            }
            continue;
        }
        /* End of file: what is typed so far is given to the reader as it is,
         * and on an empty line that is nothing - a read of 0, which is how a
         * program is told there is no more. */
        if (c == (int)t->m.cc[VIBEOS_TTY_VEOF]) {
            if (t->len == t->ready) {
                t->eof = 1;
            }
            t->ready = t->len;
            continue;
        }
        t->line[t->len++] = (uint8_t)c;
        if (echo || (c == '\n' && (lf & VIBEOS_TTY_ECHONL))) {
            tty_echo(t, (char)c);
        }
        if (c == '\n' || t->len == VIBEOS_TERM_LINE) {
            t->ready = t->len;   /* a line; or one too long to wait for its end */
        }
    }
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
long vibeos_term_read(vibeos_term_t *t, uint64_t buf, uint64_t len, int nonblock) {
    if (len == 0u) {
        return 0;
    }
    for (;;) {
        uint32_t n = 0, min, i;
        int canon, done = 0;
        long job = vibeos_term_job_check(t, VIBEOS_SIGTTIN);   /* each time: it may have been moved */

        if (job != 0) {
            return job;
        }
        ks_irq_off();
        tty_lock(t);
        tty_pump(t, len);
        canon = (t->m.lflag & VIBEOS_TTY_ICANON) != 0u;
        min = t->m.cc[VIBEOS_TTY_VMIN];
        if (t->ready > 0u) {
            n = t->ready;
            if (canon) {
                /* One line a read: up to and including its newline. */
                for (i = 0; i < t->ready; i++) {
                    if (t->line[i] == '\n') {
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
                if (vibeos_uaccess_copy((void *)(uintptr_t)buf, t->line, n) != 0) {
                    tty_unlock(t);
                    ks_irq_on();
                    return -VIBEOS_EFAULT;
                }
                for (i = n; i < t->len; i++) {
                    t->line[i - n] = t->line[i];
                }
                t->len -= n;
                t->ready -= n;
                done = 1;
            }
        } else if (t->eof) {
            t->eof = 0;
            done = 1;                     /* end of file: a read of nothing */
        } else if (!canon && min == 0u) {
            done = 1;                     /* MIN 0: whatever there is, now */
        }
        tty_unlock(t);
        if (done) {
            ks_irq_on();
            return (long)n;
        }
        if (nonblock) {
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
uint32_t vibeos_term_ready(vibeos_term_t *t) {
    uint32_t r = VIBEOS_READY_OUT, min;

    tty_lock(t);
    min = t->m.cc[VIBEOS_TTY_VMIN];
    tty_pump(t, min ? min : 1u);
    if (t->eof || ((t->m.lflag & VIBEOS_TTY_ICANON) ? t->ready > 0u
                                                          : t->ready >= (min ? min : 1u))) {
        r |= VIBEOS_READY_IN;
    }
    tty_unlock(t);
    return r;
}

/* The foreground process group, which is the console's to answer because the
 * kernel services hold it. The terminal's modes and size are asked through
 * vibeos/tty.h by the personality, which owns the request numbers and the
 * structures they carry. */
long vibeos_term_ioctl(vibeos_term_t *t, uint64_t req, uint64_t arg) {
    uint32_t session;

    tty_lock(t);
    session = t->session;
    tty_unlock(t);
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
        vibeos_task_t *me = ks_current() >= 0 ? ks_id(ks_current()) : 0;
        vibeos_procstate_t *ps = ks_current() >= 0 ? ks_ps(ks_current()) : 0;
        int i, held = 0;

        if (!me || me->sid != me->tgid) {
            return me && me->sid == session ? 0 : -VIBEOS_EPERM;   /* a session leader's to take */
        }
        if (session == me->sid) {
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
        tty_set_fg(t, me->pgid);
        ks_unlock(ks_sched_lock());
        vibeos_term_set_session(t, me->sid);
        return 0;
    }
    if (req == VIBEOS_IOCTL_NOTTY) {
        vibeos_task_t *me = ks_current() >= 0 ? ks_id(ks_current()) : 0;

        if (session == 0u || !me) {
            return -VIBEOS_ENOTTY;
        }
        /* The leader giving it up ends the session's hold: the foreground
         * job is told, as when a terminal goes away. */
        if (me->sid == me->tgid) {
            ks_lock(ks_sched_lock(), __func__);
            tty_send_group(tty_fg(t), VIBEOS_SIGHUP);
            tty_send_group(tty_fg(t), VIBEOS_SIGCONT);
            tty_set_fg(t, 0);
            ks_unlock(ks_sched_lock());
            vibeos_term_set_session(t, 0);
        }
        return 0;
    }
    if (req == VIBEOS_IOCTL_GET_PGRP) {
        uint32_t v = tty_fg(t);
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
            long job = vibeos_term_job_check(t, VIBEOS_SIGTTOU);   /* a background job asking is stopped */
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
        tty_set_fg(t, ks_id(group)->pgid);
        ks_unlock(ks_sched_lock());
        return 0;
    }
    return -VIBEOS_ENOTTY;
}

