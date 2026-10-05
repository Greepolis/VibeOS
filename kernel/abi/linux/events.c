/* Linux ABI: the descriptors that are events (docs/abi/ L4 steps 2 to 4) -
 * eventfd, timerfd and signalfd.
 *
 * The counter and the timer are file types of their own (kernel/abi/files/
 * eventfd.c, timerfd.c): what they are is no personality's. A signalfd's records
 * are Linux's layout, so its type is here, beside the handlers that make one. All
 * three are ready the way every description here is - a `ready` that poll,
 * select and epoll ask (poll.c) - and wait the way a pipe does. */

#include "linux_internal.h"

static void linux_ts_of_ticks(linux_timespec_t *ts, uint64_t ticks) {
    ts->tv_sec = (int64_t)(ticks / ks_hz());
    ts->tv_nsec = (int64_t)((ticks % ks_hz()) * (1000000000ull / ks_hz()));
}

/* ---- eventfd ------------------------------------------------------------------------ */

/* eventfd2(initval, flags), and eventfd(initval) with no flags. */
static long linux_sys_eventfd2(uint64_t initval, uint64_t flags) {
    vibeos_file_t *f;

    if (flags & ~(uint64_t)(LINUX_EFD_SEMAPHORE | LINUX_EFD_NONBLOCK | LINUX_EFD_CLOEXEC)) {
        return -VIBEOS_EINVAL;
    }
    f = vibeos_open_eventfd((uint64_t)(uint32_t)initval,
                            (flags & LINUX_EFD_SEMAPHORE) ? VIBEOS_EVENT_SEMAPHORE : 0u,
                            (flags & LINUX_EFD_NONBLOCK) ? VIBEOS_O_NONBLOCK : 0u);
    if (!f) {
        return -VIBEOS_ENFILE;
    }
    return linux_fd_install(f, (flags & LINUX_EFD_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
}

/* ---- timerfd ------------------------------------------------------------------------ */

/* timerfd_create(clockid, flags): the clocks that count time passing - one clock
 * here - and the alarm clocks, which are the superuser's (CAP_WAKE_ALARM). A
 * CPU-time clock is not one a timerfd may use, on Linux either. */
static long linux_sys_timerfd_create(uint64_t clockid, uint64_t flags) {
    int32_t clk = VIBEOS_ARG_INT(clockid);
    vibeos_file_t *f;

    if (flags & ~(uint64_t)(LINUX_TFD_NONBLOCK | LINUX_TFD_CLOEXEC)) {
        return -VIBEOS_EINVAL;
    }
    switch (clk) {
        case LINUX_CLOCK_REALTIME:
        case LINUX_CLOCK_MONOTONIC:
        case LINUX_CLOCK_BOOTTIME:
            break;
        case LINUX_CLOCK_REALTIME_ALARM:
        case LINUX_CLOCK_BOOTTIME_ALARM: {
            vibeos_cred_t me;

            linux_cred(&me);
            if (me.euid != 0u) {
                return -VIBEOS_EPERM;
            }
            break;
        }
        default:
            return -VIBEOS_EINVAL;
    }
    f = vibeos_open_timerfd(clk, (flags & LINUX_TFD_NONBLOCK) ? VIBEOS_O_NONBLOCK : 0u);
    if (!f) {
        return -VIBEOS_ENFILE;
    }
    return linux_fd_install(f, (flags & LINUX_TFD_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
}

/* The timer a descriptor names: EBADF for none, EINVAL for one that is not a
 * timer, as Linux answers. The caller puts the reference. */
static long linux_timerfd_get(uint64_t fd, vibeos_file_t **out) {
    vibeos_file_t *f = linux_file_get(fd);

    if (!f) {
        return -VIBEOS_EBADF;
    }
    if (f->ops != &vibeos_fops_timerfd) {
        vibeos_file_put(f);
        return -VIBEOS_EINVAL;
    }
    *out = f;
    return 0;
}

/* A setting copied out, last: Linux looks at the descriptor before it writes, so
 * a closed one and a bad pointer together are EBADF (LTP's timerfd_gettime01),
 * and settime's timer is set even when its old value cannot be written back. */
static long linux_itimerspec_out(uint64_t uptr, uint64_t left, uint64_t interval) {
    linux_itimerspec_t its;

    linux_ts_of_ticks(&its.it_value, left);
    linux_ts_of_ticks(&its.it_interval, interval);
    if (!linux_user_ok(uptr, sizeof(its), 1) ||
        vibeos_uaccess_copy((void *)(uintptr_t)uptr, &its, sizeof(its)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* timerfd_settime(fd, flags, new, old): arm, relative or absolute, with a
 * period; a zero value disarms. A relative time does not count the tick in
 * progress, so the timer never expires early, as a sleep never ends early. A
 * clock that can be set would make TFD_TIMER_CANCEL_ON_SET mean something; none
 * here can be, so it is accepted and never cancels. */
static long linux_sys_timerfd_settime(uint64_t fd, uint64_t flags, uint64_t new_uptr, uint64_t old_uptr) {
    linux_itimerspec_t its;
    vibeos_file_t *f;
    int64_t v, iv;
    uint64_t next = 0, old_left = 0, old_interval = 0;
    long r;

    if (flags & ~(uint64_t)(LINUX_TFD_TIMER_ABSTIME | LINUX_TFD_TIMER_CANCEL_ON_SET)) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&its, (const void *)(uintptr_t)new_uptr, sizeof(its)) != 0) {
        return -VIBEOS_EFAULT;
    }
    if ((v = linux_ticks_of(&its.it_value)) < 0 || (iv = linux_ticks_of(&its.it_interval)) < 0) {
        return -VIBEOS_EINVAL;
    }
    if ((r = linux_timerfd_get(fd, &f)) != 0) {
        return r;
    }
    if (v != 0) {
        next = (flags & LINUX_TFD_TIMER_ABSTIME) ? (uint64_t)v : ks_ticks() + (uint64_t)v + 1u;
    }
    vibeos_timerfd_set(f, next, (uint64_t)iv, &old_left, &old_interval);
    vibeos_file_put(f);
    return old_uptr != 0u ? linux_itimerspec_out(old_uptr, old_left, old_interval) : 0;
}

static long linux_sys_timerfd_gettime(uint64_t fd, uint64_t cur_uptr) {
    vibeos_file_t *f;
    uint64_t left, interval;
    long r;

    if ((r = linux_timerfd_get(fd, &f)) != 0) {
        return r;
    }
    vibeos_timerfd_get(f, &left, &interval);
    vibeos_file_put(f);
    return linux_itimerspec_out(cur_uptr, left, interval);
}

/* ---- signalfd ----------------------------------------------------------------------- */

/* One record of a signal taken, Linux's layout, from why it came - the same
 * reading of the reason a handler's siginfo gets (linux_siginfo_from). */
static void linux_ssi_of(linux_signalfd_siginfo_t *o, uint32_t sig, const vibeos_siginfo_t *why) {
    linux_siginfo_t li;
    uint32_t i;

    for (i = 0; i < sizeof(*o); i++) {
        ((unsigned char *)o)[i] = 0;
    }
    linux_siginfo_from(&li, sig, why);
    o->ssi_signo = sig;
    o->ssi_errno = li.si_errno;
    o->ssi_code = li.si_code;
    o->ssi_pid = (uint32_t)li.pid;
    o->ssi_uid = li.uid;
    o->ssi_int = (int32_t)li.value;
    o->ssi_ptr = li.value;
    o->ssi_utime = (uint64_t)li.utime;
    o->ssi_stime = (uint64_t)li.stime;
    if (why->from == VIBEOS_SIG_FROM_TIMER) {
        o->ssi_tid = (uint32_t)why->code;      /* the timer's id */
        o->ssi_overrun = (uint32_t)why->status;
        o->ssi_pid = 0;
        o->ssi_uid = 0;
    } else if (why->from == VIBEOS_SIG_FROM_CHILD) {
        o->ssi_status = why->status;
    } else if (why->from == VIBEOS_SIG_FROM_FAULT) {
        o->ssi_addr = why->addr;
        o->ssi_trapno = why->trapno;
        o->ssi_pid = 0;
        o->ssi_uid = 0;
    }
}

/* A read takes, of the signals in the mask, those pending for the reading thread,
 * lowest first, as many records as fit; it waits while none is - for one of them,
 * or for another signal that needs acting on. Taking is ks_signal_take, the take
 * sigtimedwait uses: a signal read here is not delivered as well. */
static long sfd_read(vibeos_file_t *f, uint64_t buf, uint64_t len) {
    int me = ks_current();
    uint64_t n = 0;

    if (me < 0) {
        return -VIBEOS_EINVAL;
    }
    if (len < sizeof(linux_signalfd_siginfo_t)) {
        return -VIBEOS_EINVAL;
    }
    for (;;) {
        uint32_t sig;

        for (sig = 1; sig <= VIBEOS_SIG_MAX && (n + 1u) * sizeof(linux_signalfd_siginfo_t) <= len; sig++) {
            vibeos_siginfo_t why;
            linux_signalfd_siginfo_t rec;

            if (!(f->sig_mask & (1ull << sig)) || !ks_signal_take(me, sig, &why)) {
                continue;
            }
            linux_ssi_of(&rec, sig, &why);
            if (vibeos_uaccess_copy((void *)(uintptr_t)(buf + n * sizeof(rec)), &rec, sizeof(rec)) != 0) {
                (void)ks_signal_send(me, sig, &why);   /* given back, not lost */
                return n != 0u ? (long)(n * sizeof(rec)) : -VIBEOS_EFAULT;
            }
            n++;
        }
        if (n != 0u) {
            return (long)(n * sizeof(linux_signalfd_siginfo_t));
        }
        if (f->flags & VIBEOS_O_NONBLOCK) {
            return -VIBEOS_EAGAIN;
        }
        if (ks_signal_interrupts(me)) {
            return -VIBEOS_RESTART_CALL;
        }
        ks_block_point();
    }
}

/* Readable while one of the mask is pending for whoever asks - the polling
 * thread, as Linux's signalfd_poll looks at current. */
static uint32_t sfd_ready(vibeos_file_t *f) {
    int me = ks_current();

    return (me >= 0 && (ks_id(me)->sig_pending & f->sig_mask) != 0u) ? VIBEOS_READY_IN : 0u;
}

static int sfd_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    (void)f;
    out->mode = VIBEOS_S_IFREG | 0600u;   /* Linux's anonymous inode */
    out->size = 0;
    return 0;
}

const vibeos_file_ops_t linux_fops_signalfd = {
    .name = "signalfd",
    .read = sfd_read,
    .stat = sfd_stat,
    .ready = sfd_ready,
};

/* signalfd4(fd, mask, sizemask, flags), and signalfd with no flags: a new
 * descriptor for fd -1, or a new mask for the signalfd fd names. SIGKILL and
 * SIGSTOP are left out of a mask, as Linux leaves them. */
static long linux_sys_signalfd4(uint64_t fd, uint64_t mask_uptr, uint64_t size, uint64_t flags) {
    uint64_t raw, mask;
    vibeos_file_t *f;

    if (size != 8u || (flags & ~(uint64_t)(LINUX_SFD_NONBLOCK | LINUX_SFD_CLOEXEC))) {
        return -VIBEOS_EINVAL;
    }
    if (vibeos_uaccess_copy(&raw, (const void *)(uintptr_t)mask_uptr, sizeof(raw)) != 0) {
        return -VIBEOS_EFAULT;
    }
    mask = linux_sigset_from_user(raw) & ~((1ull << VIBEOS_SIGKILL) | (1ull << VIBEOS_SIGSTOP));
    if (VIBEOS_ARG_INT(fd) == -1) {
        f = vibeos_file_alloc(&linux_fops_signalfd, (flags & LINUX_SFD_NONBLOCK) ? VIBEOS_O_NONBLOCK : 0u);
        if (!f) {
            return -VIBEOS_ENFILE;
        }
        f->sig_mask = mask;
        return linux_fd_install(f, (flags & LINUX_SFD_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
    }
    if (!(f = linux_file_get(fd))) {
        return -VIBEOS_EBADF;
    }
    if (f->ops != &linux_fops_signalfd) {
        vibeos_file_put(f);
        return -VIBEOS_EINVAL;
    }
    __atomic_store_n(&f->sig_mask, mask, __ATOMIC_RELEASE);
    vibeos_file_put(f);
    return (long)(uint32_t)VIBEOS_ARG_INT(fd);
}

/* ---- the syscalls this file implements ---------------------------------------- */
#define LINUX_EVENTS_SYSCALLS(X) \
    X(284, eventfd,         EVENTFD,  NOPTR, linux_sys_eventfd2(ARG(0), 0)) \
    X(290, eventfd2,        EVENTFD,  NOPTR, linux_sys_eventfd2(ARG(0), ARG(1))) \
    X(283, timerfd_create,  TIMERFD,  NOPTR, linux_sys_timerfd_create(ARG(0), ARG(1))) \
    X(286, timerfd_settime, TIMERFD,  PTRS(IN(2, sizeof(linux_itimerspec_t))), linux_sys_timerfd_settime(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(287, timerfd_gettime, TIMERFD,  NOPTR, linux_sys_timerfd_gettime(ARG(0), ARG(1))) \
    X(282, signalfd,        SIGNALFD, PTRS(IN(1, 8)), linux_sys_signalfd4(ARG(0), ARG(1), ARG(2), 0)) \
    X(289, signalfd4,       SIGNALFD, PTRS(IN(1, 8)), linux_sys_signalfd4(ARG(0), ARG(1), ARG(2), ARG(3)))

LINUX_DEFINE_SYSCALLS(events, LINUX_EVENTS_SYSCALLS)
