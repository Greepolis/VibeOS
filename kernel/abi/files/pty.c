/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Pseudo-terminals (docs/shell/s1.md step 4): /dev/ptmx and /dev/pts/N.
 *
 * Opening the master makes a pair. What the master writes is the slave's
 * typing, through the slave's line discipline (kernel/abi/files/tty.c); what
 * the slave writes, and what its discipline echoes, is the master's to read.
 * It is what a terminal window, ssh and `script` are: a program that plays the
 * keyboard and the screen for another.
 *
 * What each side sees when the other goes was read off the host's Linux, not
 * remembered: with the master gone the slave reads 0 and writes EIO and its
 * name is ENOENT; with every slave closed the master reads what is left, then
 * EIO, and may still write; a slave still locked by the master is EIO.
 *
 * A pair is held by its descriptions: `masters` and `slaves` count them, and
 * the slot goes back when both are zero. The lock is the pair's own and guards
 * the two rings and those counts; it is taken inside the terminal's lock (an
 * echo is a push), so nothing here calls into the terminal holding it. */

#include "files_internal.h"
#include "vibeos/devfs.h"
#include "vibeos/tty.h"

#define PTY_MAX 8u
#define PTY_RING 4096u
#define PTY_CHUNK 128u

typedef struct {
    uint8_t b[PTY_RING];
    uint32_t head;   /* producer: free-running */
    uint32_t tail;   /* consumer */
} pty_ring_t;

typedef struct {
    vibeos_term_t term;
    vibeos_lock_t lock;
    int used;
    int locked;      /* TIOCSPTLCK: the slave cannot be opened yet */
    uint32_t masters;
    uint32_t slaves;
    int slave_seen;  /* a slave has been open: its absence is now a hangup */
    uint32_t uid;
    uint32_t gid;
    pty_ring_t in;   /* master to slave: typing */
    pty_ring_t out;  /* slave to master: the screen */
} pty_t;

#define PTY_PER_USER 4u
/* quota: PTY_PER_USER pairs for each user but root, enforced in
 * vibeos_pty_open_master: one user's programs cannot take every terminal. */
static pty_t g_pty[PTY_MAX];
static vibeos_lock_t g_pty_table_lock;

static uint32_t ring_used(const pty_ring_t *r) {
    return r->head - r->tail;
}

static int ring_put(pty_ring_t *r, uint8_t c) {
    if (ring_used(r) >= PTY_RING) {
        return 0;
    }
    r->b[r->head % PTY_RING] = c;
    r->head++;
    return 1;
}

static int ring_get(pty_ring_t *r) {
    int c;

    if (ring_used(r) == 0u) {
        return -1;
    }
    c = r->b[r->tail % PTY_RING];
    r->tail++;
    return c;
}

/* ---- the slave's terminal: its backend ---------------------------------- */

static int pty_getc(void *ctx) {
    pty_t *p = ctx;
    int c;

    ks_lock(&p->lock, "pty");
    c = ring_get(&p->in);
    ks_unlock(&p->lock);
    return c;
}

/* An echo with no room is dropped, as a terminal's is: it must not wait, being
 * made under the terminal's lock. */
static void pty_echo(void *ctx, char c) {
    pty_t *p = ctx;

    ks_lock(&p->lock, "pty");
    if (c == '\n') {
        (void)ring_put(&p->out, '\r');
    }
    (void)ring_put(&p->out, (uint8_t)c);
    ks_unlock(&p->lock);
}

static void pty_rub(void *ctx) {
    pty_t *p = ctx;

    ks_lock(&p->lock, "pty");
    (void)ring_put(&p->out, '\b');
    (void)ring_put(&p->out, ' ');
    (void)ring_put(&p->out, '\b');
    ks_unlock(&p->lock);
}

static int pty_hung(void *ctx) {
    const pty_t *p = ctx;

    return __atomic_load_n(&p->masters, __ATOMIC_ACQUIRE) == 0u;
}

static const vibeos_term_ops_t g_pty_term_ops = {
    .getc = pty_getc, .echo = pty_echo, .rub = pty_rub, .hung = pty_hung,
};

/* ---- the pair's life ------------------------------------------------------ */

static pty_t *pty_of(const vibeos_file_t *f) {
    return &g_pty[f->pty];
}

static void pty_free_if_done(pty_t *p) {
    int done;

    ks_lock(&g_pty_table_lock, "pty_table");
    ks_lock(&p->lock, "pty");
    done = p->masters == 0u && p->slaves == 0u;
    ks_unlock(&p->lock);
    if (done) {
        vibeos_term_unregister(&p->term);
        p->used = 0;
    }
    ks_unlock(&g_pty_table_lock);
}

static void master_release(vibeos_file_t *f) {
    pty_t *p = pty_of(f);

    ks_lock(&p->lock, "pty");
    p->masters--;
    ks_unlock(&p->lock);
    vibeos_term_hangup(&p->term);   /* the terminal has gone from under its session */
    ks_wake_waiters();
    pty_free_if_done(p);
}

static void slave_release(vibeos_file_t *f) {
    pty_t *p = pty_of(f);

    ks_lock(&p->lock, "pty");
    p->slaves--;
    ks_unlock(&p->lock);
    ks_wake_waiters();
    pty_free_if_done(p);
}

/* ---- the master ------------------------------------------------------------ */

/* Nothing to read: 1 to look again, or an answer. */
static long pty_wait(int nonblock) {
    if (nonblock) {
        return -VIBEOS_EAGAIN;
    }
    if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
        return -VIBEOS_RESTART_CALL;
    }
    ks_block_point();
    return 1;
}

static long master_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    pty_t *p = pty_of(f);
    uint8_t chunk[PTY_CHUNK];

    if (len == 0u) {
        return 0;
    }
    for (;;) {
        uint32_t n = 0;
        int hung;
        long w;

        ks_lock(&p->lock, "pty");
        while (n < PTY_CHUNK && n < len) {
            int c = ring_get(&p->out);
            if (c < 0) {
                break;
            }
            chunk[n++] = (uint8_t)c;
        }
        hung = p->slave_seen && p->slaves == 0u;
        ks_unlock(&p->lock);
        if (n > 0u) {
            ks_wake_waiters();   /* a slave waiting for room */
            return vibeos_uaccess_copy((void *)(uintptr_t)buf, chunk, n) != 0 ? -VIBEOS_EFAULT : (long)n;
        }
        if (hung) {
            return -VIBEOS_EIO;
        }
        w = pty_wait((f->flags & VIBEOS_O_NONBLOCK) != 0u);
        if (w != 1) {
            return w;
        }
    }
}

static long master_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    pty_t *p = pty_of(f);
    uint8_t chunk[PTY_CHUNK];
    uint64_t done = 0;

    while (done < len) {
        uint32_t n = len - done > PTY_CHUNK ? PTY_CHUNK : (uint32_t)(len - done), i = 0;

        if (vibeos_uaccess_copy(chunk, (const void *)(uintptr_t)(buf + done), n) != 0) {
            return done > 0u ? (long)done : -VIBEOS_EFAULT;
        }
        while (i < n) {
            int put;

            /* A signal character acts now: the program it is for may be
             * reading nothing. */
            if (vibeos_term_control(&p->term, chunk[i])) {
                i++;
                continue;
            }
            ks_lock(&p->lock, "pty");
            put = ring_put(&p->in, chunk[i]);
            ks_unlock(&p->lock);
            if (put) {
                i++;
                continue;
            }
            (void)vibeos_term_pending(&p->term);   /* take what fits into the line */
            ks_wake_waiters();
            if (done + i > 0u) {
                return (long)(done + i);
            }
            {
                long w = pty_wait((f->flags & VIBEOS_O_NONBLOCK) != 0u);
                if (w != 1) {
                    return w;
                }
            }
        }
        done += n;
    }
    /* Echoed when it is typed, whether or not anybody is reading the slave. */
    (void)vibeos_term_pending(&p->term);
    ks_wake_waiters();
    return (long)done;
}

static uint32_t master_ready(vibeos_file_t *f) {
    pty_t *p = pty_of(f);
    uint32_t r = 0;

    ks_lock(&p->lock, "pty");
    if (ring_used(&p->out) > 0u) {
        r |= VIBEOS_READY_IN;
    }
    if (ring_used(&p->in) < PTY_RING) {
        r |= VIBEOS_READY_OUT;
    }
    if (p->slave_seen && p->slaves == 0u) {
        r |= VIBEOS_READY_HUP;
    }
    ks_unlock(&p->lock);
    return r;
}

static int master_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    out->mode = VIBEOS_S_IFCHR | 0666u;
    out->size = 0;
    out->ino = 0x40000u + (uint64_t)(uint32_t)f->pty;
    out->rdev = VIBEOS_DEV_PTMX;
    return 0;
}

static long master_ioctl(vibeos_file_t *f, uint64_t req, uint64_t arg) {
    pty_t *p = pty_of(f);
    uint32_t v;

    /* An int to the C library: these two have the top bit set, and musl's
     * arrives sign-extended (the first boot said ENOTTY to TIOCGPTN). */
    req = (uint32_t)req;
    if (req == VIBEOS_IOCTL_GPTN) {
        v = (uint32_t)f->pty;
        return vibeos_uaccess_copy((void *)(uintptr_t)arg, &v, sizeof(v)) != 0 ? -VIBEOS_EFAULT : 0;
    }
    if (req == VIBEOS_IOCTL_SPTLCK) {
        if (vibeos_uaccess_copy(&v, (const void *)(uintptr_t)arg, sizeof(v)) != 0) {
            return -VIBEOS_EFAULT;
        }
        ks_lock(&p->lock, "pty");
        p->locked = v != 0u;
        ks_unlock(&p->lock);
        return 0;
    }
    if (req == VIBEOS_IOCTL_GET_PGRP) {
        /* The master is outside the session it serves, and may ask. */
        v = vibeos_term_foreground(&p->term);
        return vibeos_uaccess_copy((void *)(uintptr_t)arg, &v, sizeof(v)) != 0 ? -VIBEOS_EFAULT : 0;
    }
    return -VIBEOS_ENOTTY;
}

const vibeos_file_ops_t vibeos_fops_ptmx = {
    .name = "ptmx",
    .read = master_read,
    .write = master_write,
    .stat = master_stat,
    .ioctl = master_ioctl,
    .ready = master_ready,
    .release = master_release,
};

/* ---- the slave ------------------------------------------------------------- */

static long slave_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    return vibeos_term_read(&pty_of(f)->term, buf, len, (f->flags & VIBEOS_O_NONBLOCK) != 0u);
}

static long slave_write(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    pty_t *p = pty_of(f);
    vibeos_tty_modes_t m;
    uint8_t chunk[PTY_CHUNK];
    uint64_t done = 0;
    int onlcr;

    if (pty_hung(p)) {
        return -VIBEOS_EIO;
    }
    vibeos_term_get(&p->term, &m);
    onlcr = (m.oflag & VIBEOS_TTY_OPOST) && (m.oflag & VIBEOS_TTY_ONLCR);
    if (m.lflag & VIBEOS_TTY_TOSTOP) {
        long job = vibeos_term_job_check(&p->term, VIBEOS_SIGTTOU);
        if (job != 0) {
            return job;
        }
    }
    while (done < len) {
        uint32_t n = len - done > PTY_CHUNK ? PTY_CHUNK : (uint32_t)(len - done), i = 0;

        if (vibeos_uaccess_copy(chunk, (const void *)(uintptr_t)(buf + done), n) != 0) {
            return done > 0u ? (long)done : -VIBEOS_EFAULT;
        }
        while (i < n) {
            int put;

            ks_lock(&p->lock, "pty");
            /* A newline and its CR go in together or not at all, so that a
             * full ring cannot leave one of them to be sent twice. */
            if (chunk[i] == '\n' && onlcr) {
                put = PTY_RING - ring_used(&p->out) >= 2u;
                if (put) {
                    (void)ring_put(&p->out, '\r');
                    (void)ring_put(&p->out, '\n');
                }
            } else {
                put = ring_put(&p->out, chunk[i]);
            }
            ks_unlock(&p->lock);
            if (put) {
                i++;
                continue;
            }
            ks_wake_waiters();
            if (pty_hung(p)) {
                return done + i > 0u ? (long)(done + i) : -VIBEOS_EIO;
            }
            if (done + i > 0u) {
                return (long)(done + i);
            }
            {
                long w = pty_wait((f->flags & VIBEOS_O_NONBLOCK) != 0u);
                if (w != 1) {
                    return w;
                }
            }
        }
        done += n;
    }
    ks_wake_waiters();
    return (long)done;
}

static uint32_t slave_ready(vibeos_file_t *f) {
    pty_t *p = pty_of(f);
    uint32_t r;

    if (pty_hung(p)) {
        /* What Linux's poll says of a hung-up terminal. */
        return VIBEOS_READY_IN | VIBEOS_READY_OUT | VIBEOS_READY_ERR | VIBEOS_READY_HUP;
    }
    r = vibeos_term_ready(&p->term) & ~VIBEOS_READY_OUT;
    ks_lock(&p->lock, "pty");
    if (PTY_RING - ring_used(&p->out) >= 2u) {
        r |= VIBEOS_READY_OUT;
    }
    ks_unlock(&p->lock);
    return r;
}

static int slave_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    const pty_t *p = pty_of(f);

    out->mode = VIBEOS_S_IFCHR | 0620u;
    out->size = 0;
    out->ino = f->node ? f->node : 0x41000u + (uint64_t)(uint32_t)f->pty;
    out->rdev = f->rdev;
    out->uid = p->uid;
    out->gid = p->gid;
    return 0;
}

static long slave_ioctl(vibeos_file_t *f, uint64_t req, uint64_t arg) {
    return vibeos_term_ioctl(&pty_of(f)->term, req, arg);
}

const vibeos_file_ops_t vibeos_fops_pts = {
    .name = "pts",
    .read = slave_read,
    .write = slave_write,
    .stat = slave_stat,
    .ioctl = slave_ioctl,
    .ready = slave_ready,
    .release = slave_release,
};

/* ---- opening ---------------------------------------------------------------- */

vibeos_file_t *vibeos_pty_open_master(uint32_t flags, long *err) {
    vibeos_procstate_t *ps = ks_current() >= 0 ? ks_ps(ks_current()) : 0;
    vibeos_file_t *f = vibeos_file_alloc(&vibeos_fops_ptmx, flags & ~(VIBEOS_O_CREAT | VIBEOS_O_EXCL | VIBEOS_O_TRUNC |
                                                                      VIBEOS_O_DIRECTORY | VIBEOS_O_NOFOLLOW));
    uint32_t i;

    if (!f) {
        *err = -VIBEOS_ENFILE;
        return 0;
    }
    ks_lock(&g_pty_table_lock, "pty_table");
    {
        uint32_t k, mine = 0, euid = ps ? ps->cred.euid : 0u;

        for (k = 0; k < PTY_MAX; k++) {
            mine += g_pty[k].used && g_pty[k].uid == euid;
        }
        i = euid != 0u && mine >= PTY_PER_USER ? PTY_MAX : 0u;
    }
    for (; i < PTY_MAX && g_pty[i].used; i++) {
    }
    if (i == PTY_MAX) {
        ks_unlock(&g_pty_table_lock);
        f->ops = &vibeos_fops_chrdev;   /* it holds no pair, so its release must give none back */
        f->rdev = VIBEOS_DEV_NULL;
        vibeos_file_put(f);
        *err = -VIBEOS_ENOSPC;   /* Linux's answer when the instance is at its limit */
        return 0;
    }
    g_pty[i].used = 1;
    g_pty[i].locked = 1;
    g_pty[i].masters = 1;
    g_pty[i].slaves = 0;
    g_pty[i].slave_seen = 0;
    g_pty[i].in.head = g_pty[i].in.tail = 0;
    g_pty[i].out.head = g_pty[i].out.tail = 0;
    g_pty[i].uid = ps ? ps->cred.euid : 0u;
    g_pty[i].gid = ps ? ps->cred.egid : 0u;
    g_pty[i].term.ops = &g_pty_term_ops;
    g_pty[i].term.ctx = &g_pty[i];
    ks_unlock(&g_pty_table_lock);
    vibeos_term_reset(&g_pty[i].term);
    vibeos_term_register(&g_pty[i].term);
    f->pty = (int)i;
    f->rdev = VIBEOS_DEV_PTMX;
    *err = 0;
    return f;
}

vibeos_file_t *vibeos_pty_open_slave(uint32_t n, uint32_t flags, uint32_t rdev, long *err) {
    vibeos_file_t *f;
    pty_t *p;
    long why = 0;

    if (n >= PTY_MAX) {
        *err = -VIBEOS_ENXIO;
        return 0;
    }
    p = &g_pty[n];
    f = vibeos_file_alloc(&vibeos_fops_pts, flags & ~(VIBEOS_O_CREAT | VIBEOS_O_EXCL | VIBEOS_O_TRUNC |
                                                      VIBEOS_O_DIRECTORY | VIBEOS_O_NOFOLLOW));
    if (!f) {
        *err = -VIBEOS_ENFILE;
        return 0;
    }
    ks_lock(&g_pty_table_lock, "pty_table");
    ks_lock(&p->lock, "pty");
    if (!p->used || p->masters == 0u) {
        why = -VIBEOS_ENOENT;
    } else if (p->locked) {
        why = -VIBEOS_EIO;
    } else {
        p->slaves++;
        p->slave_seen = 1;
    }
    ks_unlock(&p->lock);
    ks_unlock(&g_pty_table_lock);
    if (why != 0) {
        f->ops = &vibeos_fops_chrdev;   /* as above: no pair to give back */
        f->rdev = VIBEOS_DEV_NULL;
        vibeos_file_put(f);
        *err = why;
        return 0;
    }
    f->pty = (int)n;
    f->rdev = rdev;
    /* A session leader with no terminal that opens one gets it - the way
     * every getty and terminal emulator hands a session its terminal. */
    if (!(flags & VIBEOS_O_NOCTTY)) {
        vibeos_term_acquire(&p->term);
    }
    *err = 0;
    return f;
}

vibeos_term_t *vibeos_pty_term(const vibeos_file_t *f) {
    return &g_pty[f->pty].term;
}

int vibeos_pty_slave_of(const vibeos_term_t *t) {
    uint32_t i;

    for (i = 0; i < PTY_MAX; i++) {
        if (t == &g_pty[i].term) {
            return (int)i;
        }
    }
    return -1;
}

uint32_t vibeos_pty_master_pending(const vibeos_file_t *f) {
    pty_t *p = &g_pty[f->pty];
    uint32_t n;

    ks_lock(&p->lock, "pty");
    n = ring_used(&p->out);
    ks_unlock(&p->lock);
    return n;
}

/* What /dev/pts lists: a pair whose master is open. */
static int pty_exists(uint32_t n, uint32_t *uid, uint32_t *gid) {
    int yes;

    if (n >= PTY_MAX) {
        return 0;
    }
    ks_lock(&g_pty_table_lock, "pty_table");
    yes = g_pty[n].used && __atomic_load_n(&g_pty[n].masters, __ATOMIC_ACQUIRE) != 0u;
    if (yes && uid) {
        *uid = g_pty[n].uid;
    }
    if (yes && gid) {
        *gid = g_pty[n].gid;
    }
    ks_unlock(&g_pty_table_lock);
    return yes;
}

void vibeos_pty_reset(void) {
    uint32_t i;

    ks_lock(&g_pty_table_lock, "pty_table");
    for (i = 0; i < PTY_MAX; i++) {
        g_pty[i].used = 0;
        g_pty[i].masters = g_pty[i].slaves = 0;
    }
    ks_unlock(&g_pty_table_lock);
    vibeos_devfs_set_pts(pty_exists, PTY_MAX);
}
