/* Linux ABI: the event loops (docs/abi/ L4) - poll, ppoll, select and pselect6,
 * on one engine.
 *
 * Every wait in this kernel is the same: look, give up the core, look again,
 * and stop for a signal that needs acting on. What a description can do now is
 * its type's `ready` (vibeos/file.h), so an event loop is a set of descriptors
 * and the events asked of each, looked at until one of them can, the time is
 * up, or a signal comes. There is no wait queue: a second model of waiting for
 * one phase would be one more thing to get right twice. The four calls differ
 * only in how they say what they want and what they hand back.
 *
 * poll used to live in fs.c, written ahead of this phase for a shell on a
 * terminal; it moved here unchanged in what it answers for files, pipes and the
 * console. A socket says what it can do now (socket.c, from the stack), where it
 * used to say "always ready". */

#include "linux_internal.h"

/* What a description can do now, in Linux's poll bits, for the events asked:
 * the hangup and the error always, whether asked or not, as Linux reports them.
 * A type with no `ready` is always ready for both, which is what Linux assumes
 * of a file that cannot say. */
uint32_t linux_revents(vibeos_file_t *f, uint32_t events) {
    uint32_t r = f->ops->ready ? f->ops->ready(f) : (VIBEOS_READY_IN | VIBEOS_READY_OUT);
    uint32_t got = 0;

    if (r & VIBEOS_READY_IN) {
        got |= events & (LINUX_POLLIN | LINUX_POLLRDNORM);
    }
    if (r & VIBEOS_READY_OUT) {
        got |= events & (LINUX_POLLOUT | LINUX_POLLWRNORM);
    }
    if (r & VIBEOS_READY_RDHUP) {
        got |= events & LINUX_POLLRDHUP;
    }
    if (r & VIBEOS_READY_HUP) {
        got |= LINUX_POLLHUP;
    }
    if (r & VIBEOS_READY_ERR) {
        got |= LINUX_POLLERR;
    }
    return got;
}

/* ---- the engine ------------------------------------------------------------------- */

/* Look until something is ready, the time is up or a signal needs acting on;
 * what was ready wins over a signal that came meanwhile, as on Linux. `ticks` < 0
 * waits for ever and 0 looks once. The tick in progress is not counted, so the
 * wait is never shorter than asked, as a sleep's is not (misc.c); *left is what
 * was left of the time, never more than was asked. */
long linux_wait_ready(linux_look_t look, void *ctx, int64_t ticks, uint64_t *left) {
    uint64_t deadline = ticks > 0 ? ks_ticks() + (uint64_t)ticks + 1u : 0;
    long n;

    for (;;) {
        n = look(ctx);
        if (n != 0 || ticks == 0 || (ticks > 0 && ks_ticks() >= deadline)) {
            break;
        }
        if (ks_current() >= 0 && ks_signal_interrupts(ks_current())) {
            n = -VIBEOS_EINTR;
            break;
        }
        if (ticks > 0) {
            ks_wait_tick();     /* a wait with an end: the clock has to move (futex.c) */
        } else {
            ks_block_point();
        }
    }
    if (left) {
        uint64_t now = ks_ticks();

        *left = (ticks > 0 && deadline > now) ? deadline - now : 0;
        if (ticks > 0 && *left > (uint64_t)ticks) {
            *left = (uint64_t)ticks;
        }
    }
    return n;
}

/* ppoll and pselect6 wait under a mask of their own, swapped in as rt_sigsuspend
 * swaps it (sig.c): the program's own goes aside for a handler's frame, so a
 * signal the call's mask lets through is delivered under it and the program
 * resumes under its own. When the call does not end in EINTR the program's mask
 * comes back before it returns - otherwise a signal it blocks, let through only
 * for the wait, would be delivered on the way out. */
void linux_mask_swap(uint64_t raw) {
    vibeos_task_t *t = ks_id(ks_current());

    t->sig_saved = t->sig_blocked;
    t->sig_saved_valid = 1;
    t->sig_blocked = linux_sigset_from_user(raw) & ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
}

void linux_mask_back(long r) {
    vibeos_task_t *t = ks_id(ks_current());

    if (r != -VIBEOS_EINTR && t->sig_saved_valid) {
        t->sig_blocked = t->sig_saved;
        t->sig_saved_valid = 0;
    }
}

/* A time a call hands back, written over the one it was given: what was left.
 * Linux does that for ppoll, select and pselect6, and goes on without it when
 * the memory cannot be written - the "sticky" timeout - so a failure here is
 * not the call's. */
static void linux_time_left_ts(uint64_t uptr, uint64_t left) {
    linux_timespec_t ts;

    ts.tv_sec = (int64_t)(left / ks_hz());
    ts.tv_nsec = (int64_t)((left % ks_hz()) * (1000000000ull / ks_hz()));
    (void)vibeos_uaccess_copy((void *)(uintptr_t)uptr, &ts, sizeof(ts));
}

/* ---- poll, ppoll ------------------------------------------------------------------- */

typedef struct {
    uint64_t fds;
    uint64_t nfds;
} linux_pollset_t;

/* One look over a pollfd array: each element copied in, judged and copied out
 * on its own - the array was checked as a whole before the call ran, and that
 * check and these copies are two instants (H-020). */
static long linux_poll_look(void *ctx) {
    const linux_pollset_t *ps = (const linux_pollset_t *)ctx;
    long count = 0;
    uint64_t i;

    for (i = 0; i < ps->nfds; i++) {
        linux_pollfd_t p;
        uint64_t at = ps->fds + i * sizeof(p);
        int16_t got = 0;

        if (vibeos_uaccess_copy(&p, (const void *)(uintptr_t)at, sizeof(p)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (p.fd >= 0) {
            vibeos_file_t *f = linux_file_get((uint64_t)(uint32_t)p.fd);

            if (!f) {
                got = LINUX_POLLNVAL;
            } else {
                got = (int16_t)linux_revents(f, (uint32_t)(uint16_t)p.events);
                vibeos_file_put(f);
            }
        }
        if (got != p.revents) {
            p.revents = got;
            if (vibeos_uaccess_copy((void *)(uintptr_t)at, &p, sizeof(p)) != 0) {
                return -VIBEOS_EFAULT;
            }
        }
        if (got != 0) {
            count++;
        }
    }
    return count;
}

/* poll(fds, nfds, timeout): which of these descriptors can be read or written
 * now, waiting up to `timeout` milliseconds for one that can - for ever when it
 * is negative, not at all when it is 0. More descriptors than the process may
 * have open is EINVAL, as Linux's RLIMIT_NOFILE check. */
static long linux_sys_poll(uint64_t fds_uptr, uint64_t nfds, uint64_t timeout) {
    linux_pollset_t ps = {fds_uptr, nfds};
    int ms = VIBEOS_ARG_INT(timeout);
    int64_t ticks = ms < 0 ? -1 : (int64_t)vibeos_ceil_div_u64((uint64_t)ms * ks_hz(), 1000ull);

    if (nfds > (uint64_t)LINUX_MAX_FDS) {
        return -VIBEOS_EINVAL;
    }
    return linux_wait_ready(linux_poll_look, &ps, ticks, 0);
}

/* ppoll(fds, nfds, tsp, sigmask, sigsetsize): poll with a timespec - a null one
 * waits for ever - and a mask to wait under. What was left of the time is
 * written back. */
static long linux_sys_ppoll(uint64_t fds_uptr, uint64_t nfds, uint64_t ts_uptr,
                            uint64_t mask_uptr, uint64_t setsize) {
    linux_pollset_t ps = {fds_uptr, nfds};
    linux_timespec_t ts;
    int64_t ticks = -1;
    uint64_t raw = 0, left = 0;
    long r;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    if (nfds > (uint64_t)LINUX_MAX_FDS) {
        return -VIBEOS_EINVAL;
    }
    if (ts_uptr != 0u) {
        if (vibeos_uaccess_copy(&ts, (const void *)(uintptr_t)ts_uptr, sizeof(ts)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if ((ticks = linux_ticks_of(&ts)) < 0) {
            return -VIBEOS_EINVAL;
        }
    }
    if (mask_uptr != 0u) {
        if (setsize != 8u) {
            return -VIBEOS_EINVAL;
        }
        if (vibeos_uaccess_copy(&raw, (const void *)(uintptr_t)mask_uptr, sizeof(raw)) != 0) {
            return -VIBEOS_EFAULT;
        }
        linux_mask_swap(raw);
    }
    r = linux_wait_ready(linux_poll_look, &ps, ticks, &left);
    if (mask_uptr != 0u) {
        linux_mask_back(r);
    }
    if (ts_uptr != 0u) {
        linux_time_left_ts(ts_uptr, left);
    }
    return r;
}

/* ---- select, pselect6 -------------------------------------------------------------- */

/* What select counts as each kind of readiness: Linux's POLLIN_SET, POLLOUT_SET
 * and POLLEX_SET - a hangup or an error is readable, an error writable. */
#define LINUX_SELECT_IN  (LINUX_POLLIN | LINUX_POLLRDNORM | LINUX_POLLRDBAND | LINUX_POLLHUP | LINUX_POLLERR)
#define LINUX_SELECT_OUT (LINUX_POLLOUT | LINUX_POLLWRNORM | LINUX_POLLWRBAND | LINUX_POLLERR)
#define LINUX_SELECT_EX  (LINUX_POLLPRI)

typedef struct {
    uint32_t n;                          /* descriptors 0 .. n-1           */
    linux_fd_set_t in, out, ex;          /* what was asked                  */
    linux_fd_set_t rin, rout, rex;       /* what is ready, at the last look */
} linux_selset_t;

static int linux_fd_isset(const linux_fd_set_t *s, uint32_t fd) {
    return (s->fds_bits[fd / 64u] >> (fd % 64u)) & 1u;
}

static void linux_fd_set(linux_fd_set_t *s, uint32_t fd) {
    s->fds_bits[fd / 64u] |= 1ull << (fd % 64u);
}

/* One look over select's sets: a descriptor named in any of them that is not
 * open makes the whole call EBADF, as Linux checks every one it is asked about. */
static long linux_select_look(void *ctx) {
    linux_selset_t *ss = (linux_selset_t *)ctx;
    long count = 0;
    uint32_t fd, w;

    for (w = 0; w < LINUX_FD_SETSIZE / 64u; w++) {
        ss->rin.fds_bits[w] = ss->rout.fds_bits[w] = ss->rex.fds_bits[w] = 0;
    }
    for (fd = 0; fd < ss->n; fd++) {
        vibeos_file_t *f;
        uint32_t got;
        int in = linux_fd_isset(&ss->in, fd), out = linux_fd_isset(&ss->out, fd);
        int ex = linux_fd_isset(&ss->ex, fd);

        if (!in && !out && !ex) {
            continue;
        }
        if (!(f = linux_file_get(fd))) {
            return -VIBEOS_EBADF;
        }
        got = linux_revents(f, LINUX_SELECT_IN | LINUX_SELECT_OUT | LINUX_SELECT_EX);
        vibeos_file_put(f);
        if (in && (got & LINUX_SELECT_IN)) {
            linux_fd_set(&ss->rin, fd);
            count++;
        }
        if (out && (got & LINUX_SELECT_OUT)) {
            linux_fd_set(&ss->rout, fd);
            count++;
        }
        if (ex && (got & LINUX_SELECT_EX)) {
            linux_fd_set(&ss->rex, fd);
            count++;
        }
    }
    return count;
}

/* The bytes of a set select reads and writes for `n` descriptors: whole 64-bit
 * words, as Linux copies them, so a set smaller than FD_SETSIZE in user memory
 * is all that is touched. */
static uint64_t linux_fdset_bytes(uint32_t n) {
    return (uint64_t)((n + 63u) / 64u) * 8u;
}

/* A set in from user memory, judged first: its size follows from nfds, which no
 * row can say, and the machine's copy reads any page that is mapped (M-082). */
static long linux_fdset_in(uint64_t uptr, uint32_t n, linux_fd_set_t *out) {
    uint32_t w;

    for (w = 0; w < LINUX_FD_SETSIZE / 64u; w++) {
        out->fds_bits[w] = 0;
    }
    if (uptr == 0u || n == 0u) {
        return 0;
    }
    if (!linux_user_ok(uptr, linux_fdset_bytes(n), 1) ||
        vibeos_uaccess_copy(out->fds_bits, (const void *)(uintptr_t)uptr, linux_fdset_bytes(n)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (n % 64u != 0u) {
        out->fds_bits[n / 64u] &= (1ull << (n % 64u)) - 1u;   /* bits past nfds are not asked */
    }
    return 0;
}

static long linux_fdset_out(uint64_t uptr, uint32_t n, const linux_fd_set_t *s) {
    if (uptr == 0u || n == 0u) {
        return 0;
    }
    return vibeos_uaccess_copy((void *)(uintptr_t)uptr, s->fds_bits, linux_fdset_bytes(n)) != 0
               ? -VIBEOS_EFAULT : 0;
}

/* select and pselect6 after their arguments are read: the sets in, the wait, the
 * sets out - all three, cleared, when the time ran out; untouched on an error,
 * as Linux leaves them. */
static long linux_select_common(uint64_t nfds, uint64_t rp, uint64_t wp, uint64_t ep,
                                int64_t ticks, uint64_t *left) {
    linux_selset_t ss;
    int32_t n = VIBEOS_ARG_INT(nfds);
    long r;

    if (n < 0) {
        return -VIBEOS_EINVAL;
    }
    /* Past the descriptors a process can have, Linux looks no further. */
    ss.n = (uint32_t)n > (uint32_t)LINUX_MAX_FDS ? (uint32_t)LINUX_MAX_FDS : (uint32_t)n;
    if (linux_fdset_in(rp, ss.n, &ss.in) != 0 || linux_fdset_in(wp, ss.n, &ss.out) != 0 ||
        linux_fdset_in(ep, ss.n, &ss.ex) != 0) {
        return -VIBEOS_EFAULT;
    }
    r = linux_wait_ready(linux_select_look, &ss, ticks, left);
    if (r < 0) {
        return r;
    }
    if (linux_fdset_out(rp, ss.n, &ss.rin) != 0 || linux_fdset_out(wp, ss.n, &ss.rout) != 0 ||
        linux_fdset_out(ep, ss.n, &ss.rex) != 0) {
        return -VIBEOS_EFAULT;
    }
    return r;
}

/* select(nfds, readfds, writefds, exceptfds, timeout): the three sets, and a
 * timeval - null waits for ever - which is written back with what was left. */
static long linux_sys_select(uint64_t nfds, uint64_t rp, uint64_t wp, uint64_t ep, uint64_t tv_uptr) {
    linux_timeval_t tv;
    linux_timespec_t ts;
    int64_t ticks = -1;
    uint64_t left = 0;
    long r;

    if (tv_uptr != 0u) {
        if (vibeos_uaccess_copy(&tv, (const void *)(uintptr_t)tv_uptr, sizeof(tv)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if (tv.tv_usec < 0 || tv.tv_usec >= 1000000) {
            return -VIBEOS_EINVAL;
        }
        ts.tv_sec = tv.tv_sec;
        ts.tv_nsec = tv.tv_usec * 1000;
        if ((ticks = linux_ticks_of(&ts)) < 0) {
            return -VIBEOS_EINVAL;
        }
    }
    r = linux_select_common(nfds, rp, wp, ep, ticks, &left);
    if (tv_uptr != 0u) {
        tv.tv_sec = (int64_t)(left / ks_hz());
        tv.tv_usec = (int64_t)((left % ks_hz()) * (1000000ull / ks_hz()));
        (void)vibeos_uaccess_copy((void *)(uintptr_t)tv_uptr, &tv, sizeof(tv));   /* sticky: see above */
    }
    return r;
}

/* pselect6(nfds, readfds, writefds, exceptfds, timeout, sigarg): select with a
 * timespec and a mask to wait under. The sixth argument is not a mask but a
 * pair - where the mask is, and its size - since a syscall has six registers;
 * the mask's own pointer is judged here, as nothing in a row can name it. */
static long linux_sys_pselect6(uint64_t nfds, uint64_t rp, uint64_t wp, uint64_t ep,
                               uint64_t ts_uptr, uint64_t sig_uptr) {
    linux_timespec_t ts;
    int64_t ticks = -1;
    uint64_t pack[2] = {0, 0}, raw = 0, left = 0;
    long r;

    if (ks_current() < 0) {
        return -VIBEOS_EINVAL;
    }
    if (ts_uptr != 0u) {
        if (vibeos_uaccess_copy(&ts, (const void *)(uintptr_t)ts_uptr, sizeof(ts)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if ((ticks = linux_ticks_of(&ts)) < 0) {
            return -VIBEOS_EINVAL;
        }
    }
    if (sig_uptr != 0u && vibeos_uaccess_copy(pack, (const void *)(uintptr_t)sig_uptr, sizeof(pack)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if (pack[0] != 0u) {
        if (pack[1] != 8u) {
            return -VIBEOS_EINVAL;
        }
        if (!linux_user_ok(pack[0], 8u, 0) ||
            vibeos_uaccess_copy(&raw, (const void *)(uintptr_t)pack[0], sizeof(raw)) != 0) {
            return -VIBEOS_EFAULT;
        }
        linux_mask_swap(raw);
    }
    r = linux_select_common(nfds, rp, wp, ep, ticks, &left);
    if (pack[0] != 0u) {
        linux_mask_back(r);
    }
    if (ts_uptr != 0u) {
        linux_time_left_ts(ts_uptr, left);
    }
    return r;
}

/* ---- the syscalls this file implements ---------------------------------------- */
#define LINUX_POLL_SYSCALLS(X) \
    X(7,   poll,     POLL, PTRS(OUT_VEC(0, 1, sizeof(linux_pollfd_t), 1024)), linux_sys_poll(ARG(0), ARG(1), ARG(2))) \
    X(271, ppoll,    POLL, PTRS(OUT_VEC(0, 1, sizeof(linux_pollfd_t), 1024), IN_OPT(2, sizeof(linux_timespec_t)), IN_OPT(3, 8)), linux_sys_ppoll(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(23,  select,   POLL, PTRS(IN_OPT(4, sizeof(linux_timeval_t))), linux_sys_select(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(270, pselect6, POLL, PTRS(IN_OPT(4, sizeof(linux_timespec_t)), IN_OPT(5, 16)), linux_sys_pselect6(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)))

LINUX_DEFINE_SYSCALLS(poll, LINUX_POLL_SYSCALLS)
