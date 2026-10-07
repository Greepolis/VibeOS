/* Linux ABI: epoll (docs/abi/ L4 step 5) - an interest list as a file.
 *
 * An entry is a description, the descriptor number it was added under, the
 * events asked and the program's word to hand back. A look asks each entry's
 * description what it can do (`ready`, as poll asks - poll.c), and the waiting
 * is poll's: look, give up the core, look again (linux_wait_ready). So there is
 * nothing to wake and no callback into the file types; epoll is poll with a
 * set that outlives the call.
 *
 * Three rules are Linux's and decide the shape:
 *
 *  - An entry belongs to the *description*, not the descriptor, and holds no
 *    reference to it: closing one descriptor of a duplicated file leaves the
 *    entry, and the last reference going takes it away. That is the release
 *    hook (vibeos_file_on_release), which runs before the slot can be handed
 *    out again - so a pointer here never names a slot's next tenant.
 *  - Edge-triggered means a rise: what `ready` says now that it did not say at
 *    the last look. Linux's is "something happened", which also reports a pipe
 *    that was readable and got more; here there is no event to report, only a
 *    state, and a rise is the most a state can say. The difference is written
 *    down in phases.md.
 *  - An epoll can be in another's set, or in poll's; a cycle is ELOOP. A look
 *    therefore calls `ready` on descriptions that may be epolls themselves, so
 *    it never does that under this file's lock: it takes what it needs - a
 *    reference to each description, by try-get - and lets go before asking.
 *
 * The entries are one pool for every instance, so that the release hook has one
 * table to search and an instance needs no memory of its own. */

#include "linux_internal.h"

#define EP_MAX 512u      /* entries, in every instance together */
#define EP_DEPTH 5u      /* epolls in one chain: the top and Linux's EP_MAX_NESTS (4) below it */
#define EP_WAIT_MAX (0x7fffffffu / (uint32_t)sizeof(linux_epoll_event_t))   /* Linux's EP_MAX_EVENTS */

typedef struct {
    vibeos_file_t *owner;   /* the epoll; 0 for a free entry */
    vibeos_file_t *file;    /* what it watches */
    int32_t fd;
    uint32_t events;        /* asked, with EPOLLET / EPOLLONESHOT */
    uint64_t data;
    uint32_t seen;          /* what `ready` said at the last look, for an edge */
    uint32_t gen;           /* moves on every change, so a look's update is refused */
    uint8_t armed;          /* 0 once a one-shot entry has fired, until EPOLL_CTL_MOD */
} linux_ep_entry_t;

static linux_ep_entry_t g_ep[EP_MAX];
static vibeos_lock_t g_ep_lock;

#define EP_FLAGS (LINUX_EPOLLET | LINUX_EPOLLONESHOT | LINUX_EPOLLWAKEUP | LINUX_EPOLLEXCLUSIVE)
/* What may go with EPOLLEXCLUSIVE - Linux's EPOLLEXCLUSIVE_OK_BITS; ONESHOT may not. */
#define EP_EXCLUSIVE_OK ((uint32_t)(LINUX_EPOLLEXCLUSIVE | LINUX_EPOLLET | LINUX_EPOLLWAKEUP | LINUX_POLLIN | \
                                    LINUX_POLLOUT | LINUX_POLLERR | LINUX_POLLHUP))

/* ---- the file type ------------------------------------------------------------------- */

typedef struct {
    uint32_t at;            /* index in g_ep */
    uint32_t gen;
    vibeos_file_t *file;    /* with a reference */
    uint32_t events;
    uint64_t data;
    uint32_t seen;
} linux_ep_snap_t;

/* How many entries one look takes; more stay for the next look, in pool order.
 * Bounded so that a look is a stack array, not an allocation. */
#define EP_LOOK 64u

/* Take a reference to each armed entry of `ep`, from `from` on, under the lock.
 * A description whose last reference is going is skipped: its entry is about to
 * be removed by the release hook, which is waiting for this lock. */
static uint32_t ep_snapshot(vibeos_file_t *ep, uint32_t from, linux_ep_snap_t *out, uint32_t *next) {
    uint32_t n = 0, i;

    ks_lock(&g_ep_lock, __func__);
    for (i = from; i < EP_MAX && n < EP_LOOK; i++) {
        linux_ep_entry_t *e = &g_ep[i];

        if (e->owner != ep || !e->armed || !vibeos_file_try_get(e->file)) {
            continue;
        }
        out[n].at = i;
        out[n].gen = e->gen;
        out[n].file = e->file;
        out[n].events = e->events;
        out[n].data = e->data;
        out[n].seen = e->seen;
        n++;
    }
    ks_unlock(&g_ep_lock);
    *next = i;
    return n;
}

/* What an entry reports now: its events as poll would say them, the hangup and
 * the error always - and for an edge-triggered one only what has risen. */
static uint32_t ep_judge(const linux_ep_snap_t *s, uint32_t *now) {
    uint32_t got = linux_revents(s->file, s->events & ~(uint32_t)EP_FLAGS);

    *now = got;
    return (s->events & LINUX_EPOLLET) ? (got & ~s->seen) : got;
}

/* An epoll is readable while one of its entries would be reported. */
static uint32_t ep_ready(vibeos_file_t *ep) {
    linux_ep_snap_t s[EP_LOOK];
    uint32_t from = 0, n, i, now, r = 0;

    while (from < EP_MAX && r == 0) {
        n = ep_snapshot(ep, from, s, &from);
        for (i = 0; i < n; i++) {
            if (r == 0 && ep_judge(&s[i], &now) != 0) {
                r = VIBEOS_READY_IN;
            }
            vibeos_file_put(s[i].file);
        }
    }
    return r;
}

static int ep_stat(vibeos_file_t *f, vibeos_file_stat_t *out) {
    (void)f;
    out->mode = VIBEOS_S_IFREG | 0600u;   /* Linux's anonymous inode */
    out->size = 0;
    return 0;
}

static void ep_release(vibeos_file_t *ep);

const vibeos_file_ops_t linux_fops_epoll = {
    .name = "eventpoll",
    .stat = ep_stat,
    .ready = ep_ready,
    .release = ep_release,
};

/* A description going: every entry that watches it, and if it is an epoll, every
 * entry it owns. Registered with the file layer (linux_epoll_init); runs for
 * every description, so it returns at once while no entry exists. */
static uint32_t g_ep_used;

static void ep_forget(vibeos_file_t *f) {
    uint32_t i;

    if (__atomic_load_n(&g_ep_used, __ATOMIC_ACQUIRE) == 0u) {
        return;
    }
    ks_lock(&g_ep_lock, __func__);
    for (i = 0; i < EP_MAX; i++) {
        if (g_ep[i].owner != 0 && (g_ep[i].file == f || g_ep[i].owner == f)) {
            g_ep[i].owner = 0;
            g_ep[i].file = 0;
            g_ep[i].gen++;
            __atomic_sub_fetch(&g_ep_used, 1u, __ATOMIC_ACQ_REL);
        }
    }
    ks_unlock(&g_ep_lock);
}

static void ep_release(vibeos_file_t *ep) {
    ep_forget(ep);
}

/* Once on the machine; before every test on the host, whose file table is reset
 * under the pool - so the pool is emptied here, not assumed empty. */
void linux_epoll_init(void) {
    uint32_t i;

    for (i = 0; i < EP_MAX; i++) {
        g_ep[i].owner = 0;
        g_ep[i].file = 0;
    }
    __atomic_store_n(&g_ep_used, 0u, __ATOMIC_RELEASE);
    vibeos_file_on_release(ep_forget);
}

/* ---- epoll_create, epoll_create1 ----------------------------------------------------- */

static long linux_sys_epoll_create1(uint64_t flags) {
    vibeos_file_t *f;

    if (flags & ~(uint64_t)LINUX_EPOLL_CLOEXEC) {
        return -VIBEOS_EINVAL;
    }
    if (!(f = vibeos_file_alloc(&linux_fops_epoll, 0))) {
        return -VIBEOS_ENFILE;
    }
    return linux_fd_install(f, (flags & LINUX_EPOLL_CLOEXEC) ? VIBEOS_FD_CLOEXEC : 0u, 0);
}

/* The size is a hint nobody has read since Linux 2.6.8; it must be positive. */
static long linux_sys_epoll_create(uint64_t size) {
    return VIBEOS_ARG_INT(size) <= 0 ? -VIBEOS_EINVAL : linux_sys_epoll_create1(0);
}

/* ---- epoll_ctl ----------------------------------------------------------------------- */

static int ep_find(vibeos_file_t *ep, vibeos_file_t *f, int32_t fd) {
    uint32_t i;

    for (i = 0; i < EP_MAX; i++) {
        if (g_ep[i].owner == ep && g_ep[i].file == f && g_ep[i].fd == fd) {
            return (int)i;
        }
    }
    return -1;
}

/* The longest chain of epolls from `ep` down through what it watches, `ep`
 * counted; more than EP_DEPTH if it reaches `outer`, which is a cycle. */
static uint32_t ep_down(vibeos_file_t *ep, vibeos_file_t *outer, uint32_t depth) {
    uint32_t i, best = 1, d;

    if (ep == outer || depth > EP_DEPTH) {
        return EP_DEPTH + 1u;
    }
    for (i = 0; i < EP_MAX; i++) {
        if (g_ep[i].owner == ep && g_ep[i].file->ops == &linux_fops_epoll &&
            (d = 1u + ep_down(g_ep[i].file, outer, depth + 1u)) > best) {
            best = d;
        }
    }
    return best;
}

/* The longest chain of epolls from `ep` up through those that watch it. */
static uint32_t ep_up(vibeos_file_t *ep, uint32_t depth) {
    uint32_t i, best = 1, d;

    if (depth > EP_DEPTH) {
        return EP_DEPTH + 1u;
    }
    for (i = 0; i < EP_MAX; i++) {
        if (g_ep[i].owner != 0 && g_ep[i].file == ep && (d = 1u + ep_up(g_ep[i].owner, depth + 1u)) > best) {
            best = d;
        }
    }
    return best;
}

/* Would `inner` in `outer`'s set make a cycle, or a chain longer than Linux
 * allows? The chain is the whole of it once joined: what `inner` reaches below
 * and what reaches `outer` above. Five epolls is the most - EP_MAX_NESTS is four
 * levels below the top - and LTP's epoll_ctl04 builds exactly five and asks for
 * a sixth. It counted only the chain below at first, and let the sixth in.
 * Under the lock. */
static int ep_loops(vibeos_file_t *outer, vibeos_file_t *inner) {
    return ep_down(inner, outer, 0) + ep_up(outer, 0) > EP_DEPTH;
}

static long ep_ctl_locked(vibeos_file_t *ep, uint64_t op, vibeos_file_t *f, int32_t fd,
                          const linux_epoll_event_t *ev) {
    int at = ep_find(ep, f, fd);
    uint32_t i;

    switch (op) {
        case LINUX_EPOLL_CTL_ADD:
            if (at >= 0) {
                return -VIBEOS_EEXIST;
            }
            if (f->ops == &linux_fops_epoll && ep_loops(ep, f)) {
                return -VIBEOS_ELOOP;
            }
            for (i = 0; i < EP_MAX && g_ep[i].owner != 0; i++) {
            }
            if (i == EP_MAX) {
                return -VIBEOS_ENOSPC;
            }
            g_ep[i].file = f;
            g_ep[i].fd = fd;
            g_ep[i].events = ev->events;
            g_ep[i].data = ev->data;
            g_ep[i].seen = 0;
            g_ep[i].armed = 1;
            g_ep[i].gen++;
            g_ep[i].owner = ep;
            __atomic_add_fetch(&g_ep_used, 1u, __ATOMIC_ACQ_REL);
            return 0;
        case LINUX_EPOLL_CTL_MOD:
            if (at < 0) {
                return -VIBEOS_ENOENT;
            }
            /* An entry added with EPOLLEXCLUSIVE stays as it is (Linux's EINVAL). */
            if (g_ep[at].events & LINUX_EPOLLEXCLUSIVE) {
                return -VIBEOS_EINVAL;
            }
            g_ep[at].events = ev->events;
            g_ep[at].data = ev->data;
            g_ep[at].seen = 0;      /* a modified entry reports what is ready now */
            g_ep[at].armed = 1;
            g_ep[at].gen++;
            return 0;
        case LINUX_EPOLL_CTL_DEL:
            if (at < 0) {
                return -VIBEOS_ENOENT;
            }
            g_ep[at].owner = 0;
            g_ep[at].file = 0;
            g_ep[at].gen++;
            __atomic_sub_fetch(&g_ep_used, 1u, __ATOMIC_ACQ_REL);
            return 0;
        default:
            return -VIBEOS_EINVAL;
    }
}

/* epoll_ctl(epfd, op, fd, event): the event is read for ADD and MOD, and not for
 * DEL, which old programs call with a null one. */
static long linux_sys_epoll_ctl(uint64_t epfd, uint64_t op, uint64_t fd, uint64_t ev_uptr) {
    linux_epoll_event_t ev;
    vibeos_file_t *ep, *f;
    long r;

    ev.events = 0;
    ev.data = 0;
    if (op != LINUX_EPOLL_CTL_DEL) {
        if (!linux_user_ok(ev_uptr, sizeof(ev), 0) ||
            vibeos_uaccess_copy(&ev, (const void *)(uintptr_t)ev_uptr, sizeof(ev)) != 0) {
            return -VIBEOS_EFAULT;
        }
    }
    if (!(ep = linux_file_get(epfd))) {
        return -VIBEOS_EBADF;
    }
    if (!(f = linux_file_get(fd))) {
        vibeos_file_put(ep);
        return -VIBEOS_EBADF;
    }
    if (f->ops->ready == 0) {
        r = -VIBEOS_EPERM;          /* a regular file or a directory: always ready */
    } else if (ep->ops != &linux_fops_epoll || f == ep) {
        r = -VIBEOS_EINVAL;
    } else if ((ev.events & LINUX_EPOLLEXCLUSIVE) &&
               (op == LINUX_EPOLL_CTL_MOD || (ev.events & ~EP_EXCLUSIVE_OK) || f->ops == &linux_fops_epoll)) {
        r = -VIBEOS_EINVAL;         /* EXCLUSIVE to MOD, with ONESHOT, or on an epoll */
    } else {
        ks_lock(&g_ep_lock, __func__);
        r = ep_ctl_locked(ep, op, f, VIBEOS_ARG_INT(fd), &ev);
        ks_unlock(&g_ep_lock);
    }
    vibeos_file_put(f);
    vibeos_file_put(ep);
    return r;
}

/* ---- epoll_wait, epoll_pwait, epoll_pwait2 -------------------------------------------- */

typedef struct {
    vibeos_file_t *ep;
    uint64_t out;
    uint32_t max;
} linux_ep_wait_t;

/* One look: entries reported into the program's array, up to max. An
 * edge-triggered entry remembers what it saw; a one-shot one that was reported
 * is disarmed. Both are written back only to the entry as it was looked at - if
 * a ctl changed it meanwhile, the change stands. */
static long ep_look(void *ctx) {
    const linux_ep_wait_t *w = (const linux_ep_wait_t *)ctx;
    linux_ep_snap_t s[EP_LOOK];
    uint32_t from = 0, n, i, got = 0;
    long fault = 0;

    while (from < EP_MAX && got < w->max) {
        n = ep_snapshot(w->ep, from, s, &from);
        for (i = 0; i < n; i++) {
            uint32_t now = 0, rep = 0;
            int fired = 0, judged = got < w->max;

            if (judged) {
                rep = ep_judge(&s[i], &now);
            }

            if (rep != 0u && fault == 0) {
                linux_epoll_event_t o;

                o.events = rep;
                o.data = s[i].data;
                if (vibeos_uaccess_copy((void *)(uintptr_t)(w->out + got * sizeof(o)), &o, sizeof(o)) != 0) {
                    fault = -VIBEOS_EFAULT;
                } else {
                    got++;
                    fired = 1;
                }
            }
            if (judged && (rep != 0u || (s[i].events & LINUX_EPOLLET))) {
                ks_lock(&g_ep_lock, __func__);
                if (g_ep[s[i].at].gen == s[i].gen) {
                    if (s[i].events & LINUX_EPOLLET) {
                        g_ep[s[i].at].seen = now;
                    }
                    if (fired && (s[i].events & LINUX_EPOLLONESHOT)) {
                        g_ep[s[i].at].armed = 0;
                    }
                }
                ks_unlock(&g_ep_lock);
            }
            vibeos_file_put(s[i].file);
        }
    }
    return got != 0u ? (long)got : fault;
}

/* The common call: `ticks` < 0 waits for ever, 0 looks once. */
static long ep_wait(uint64_t epfd, uint64_t events, uint64_t maxevents, int64_t ticks, int has_mask,
                    uint64_t mask) {
    linux_ep_wait_t w;
    long r;
    int32_t max = VIBEOS_ARG_INT(maxevents);

    /* The array was judged by the row (OUT_VEC) when max is in range. */
    if (max <= 0 || (uint64_t)max > EP_WAIT_MAX) {
        return -VIBEOS_EINVAL;
    }
    if (!(w.ep = linux_file_get(epfd))) {
        return -VIBEOS_EBADF;
    }
    if (w.ep->ops != &linux_fops_epoll) {
        vibeos_file_put(w.ep);
        return -VIBEOS_EINVAL;
    }
    w.out = events;
    w.max = (uint32_t)max;
    if (has_mask) {
        linux_mask_swap(mask);
    }
    r = linux_wait_ready(ep_look, &w, ticks, 0);
    if (has_mask) {
        linux_mask_back(r);
    }
    vibeos_file_put(w.ep);
    return r;
}

/* A timeout in milliseconds as ticks, rounded up; negative is for ever. */
static int64_t ep_ms(uint64_t timeout) {
    int32_t ms = VIBEOS_ARG_INT(timeout);

    return ms < 0 ? -1 : (int64_t)(((uint64_t)ms * ks_hz() + 999u) / 1000u);
}

/* The mask, if there is one: Linux's size must be its own (8), else EINVAL. */
static long ep_mask(uint64_t uptr, uint64_t size, int *has, uint64_t *mask) {
    *has = 0;
    if (uptr == 0u) {
        return 0;
    }
    if (size != 8u) {
        return -VIBEOS_EINVAL;
    }
    if (!linux_user_ok(uptr, 8, 0) || vibeos_uaccess_copy(mask, (const void *)(uintptr_t)uptr, 8) != 0) {
        return -VIBEOS_EFAULT;
    }
    *has = 1;
    return 0;
}

static long linux_sys_epoll_pwait(uint64_t epfd, uint64_t events, uint64_t max, uint64_t timeout,
                                  uint64_t mask_uptr, uint64_t size) {
    uint64_t mask = 0;
    int has;
    long r = ep_mask(mask_uptr, size, &has, &mask);

    return r != 0 ? r : ep_wait(epfd, events, max, ep_ms(timeout), has, mask);
}

static long linux_sys_epoll_pwait2(uint64_t epfd, uint64_t events, uint64_t max, uint64_t ts_uptr,
                                   uint64_t mask_uptr, uint64_t size) {
    uint64_t mask = 0;
    int64_t ticks = -1;
    int has;
    long r = ep_mask(mask_uptr, size, &has, &mask);

    if (r != 0) {
        return r;
    }
    if (ts_uptr != 0u) {
        linux_timespec_t ts;

        if (vibeos_uaccess_copy(&ts, (const void *)(uintptr_t)ts_uptr, sizeof(ts)) != 0) {
            return -VIBEOS_EFAULT;
        }
        if ((ticks = linux_ticks_of(&ts)) < 0) {
            return -VIBEOS_EINVAL;
        }
    }
    return ep_wait(epfd, events, max, ticks, has, mask);
}

#define LINUX_EPOLL_SYSCALLS(X) \
    X(213, epoll_create,  EPOLL, NOPTR, linux_sys_epoll_create(ARG(0))) \
    X(291, epoll_create1, EPOLL, NOPTR, linux_sys_epoll_create1(ARG(0))) \
    X(233, epoll_ctl,     EPOLL, NOPTR, linux_sys_epoll_ctl(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(232, epoll_wait,    EPOLL, PTRS(OUT_VEC32(1, 2, sizeof(linux_epoll_event_t), EP_WAIT_MAX)), ep_wait(ARG(0), ARG(1), ARG(2), ep_ms(ARG(3)), 0, 0)) \
    X(281, epoll_pwait,   EPOLL, PTRS(OUT_VEC32(1, 2, sizeof(linux_epoll_event_t), EP_WAIT_MAX)), linux_sys_epoll_pwait(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(441, epoll_pwait2,  EPOLL, PTRS(IN_OPT(3, sizeof(linux_timespec_t)), OUT_VEC32(1, 2, sizeof(linux_epoll_event_t), EP_WAIT_MAX)), linux_sys_epoll_pwait2(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5)))

LINUX_DEFINE_SYSCALLS(epoll, LINUX_EPOLL_SYSCALLS)
