/* A timer as a file (docs/abi/ L4 step 3): Linux's timerfd, read for how many
 * times it expired since it was last read.
 *
 * Nothing fires it. A timer here is a deadline and a period in clock ticks, and
 * whoever looks - a read, a poll - counts the expiries that have happened since,
 * under this file's lock: the clocks a timerfd may use all count time passing,
 * which is one clock on this machine (ks_ticks), so "how many periods have gone
 * by" is arithmetic, not an interrupt. That is why it needs no slot in the
 * process-timer table (kernel/sched/ptimer.c) and why a timer whose descriptor
 * nobody reads costs nothing.
 *
 * A wait for an expiry is a pipe's wait: look, give up the core, look again,
 * stop for a signal. */

#include "files_internal.h"

static vibeos_lock_t g_tfd_lock;   /* the fields of every timer description */

/* Count the expiries up to now into tfd_count and move the deadline past now.
 * Under g_tfd_lock. */
static void tfd_catch_up(vibeos_file_t *f, uint64_t now) {
    uint64_t n;

    if (f->tfd_next == 0u || now < f->tfd_next) {
        return;
    }
    n = 1u + (f->tfd_interval != 0u ? (now - f->tfd_next) / f->tfd_interval : 0u);
    f->tfd_count += n;
    f->tfd_next = f->tfd_interval != 0u ? f->tfd_next + n * f->tfd_interval : 0u;
}

static long tfd_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    if (len < 8u) {
        return -VIBEOS_EINVAL;
    }
    for (;;) {
        uint64_t got;

        ks_lock(&g_tfd_lock, __func__);
        tfd_catch_up(f, ks_ticks());
        got = f->tfd_count;
        f->tfd_count = 0;
        ks_unlock(&g_tfd_lock);
        if (got != 0u) {
            if (vibeos_uaccess_copy((void *)(uintptr_t)buf, &got, sizeof(got)) != 0) {
                ks_lock(&g_tfd_lock, __func__);
                f->tfd_count += got;   /* not lost to a buffer that could not take it */
                ks_unlock(&g_tfd_lock);
                return -VIBEOS_EFAULT;
            }
            return 8;
        }
        if (f->flags & VIBEOS_O_NONBLOCK) {
            return -VIBEOS_EAGAIN;
        }
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            return -VIBEOS_RESTART_CALL;
        }
        ks_wait_tick();   /* a wait for the clock: it has to move */
    }
}

static uint32_t tfd_ready(vibeos_file_t *f) {
    uint32_t r;

    ks_lock(&g_tfd_lock, __func__);
    tfd_catch_up(f, ks_ticks());
    r = f->tfd_count != 0u ? VIBEOS_READY_IN : 0u;
    ks_unlock(&g_tfd_lock);
    return r;
}

/* Linux's anonymous inode, as a pidfd's (pidfd.c). */
static int tfd_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    (void)f;
    out->mode = VIBEOS_S_IFREG | 0600u;
    out->size = 0;
    return 0;
}

const vibeos_file_ops_t vibeos_fops_timerfd = {
    .name = "timerfd",
    .read = tfd_read,
    .stat = tfd_stat,
    .ready = tfd_ready,
};

vibeos_file_t *vibeos_open_timerfd(int32_t clock, uint32_t flags) {
    vibeos_file_t *f = vibeos_file_alloc(&vibeos_fops_timerfd, flags);

    if (f) {
        f->tfd_clock = clock;
    }
    return f;
}

void vibeos_timerfd_set(vibeos_file_t *f, uint64_t next, uint64_t interval, uint64_t *old_left,
                        uint64_t *old_interval) {
    uint64_t now = ks_ticks();

    ks_lock(&g_tfd_lock, __func__);
    tfd_catch_up(f, now);
    if (old_left) {
        *old_left = f->tfd_next > now ? f->tfd_next - now : 0u;
    }
    if (old_interval) {
        *old_interval = f->tfd_interval;
    }
    /* A timer set again starts counting again, as Linux's does. */
    f->tfd_next = next;
    f->tfd_interval = next != 0u ? interval : 0u;
    f->tfd_count = 0;
    ks_unlock(&g_tfd_lock);
    ks_wake_waiters();
}

void vibeos_timerfd_get(vibeos_file_t *f, uint64_t *left, uint64_t *interval) {
    uint64_t now = ks_ticks();

    ks_lock(&g_tfd_lock, __func__);
    tfd_catch_up(f, now);
    *left = f->tfd_next > now ? f->tfd_next - now : 0u;
    *interval = f->tfd_interval;
    ks_unlock(&g_tfd_lock);
}
