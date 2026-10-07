/* Process timers. See include/vibeos/ptimer.h. */

#include "vibeos/ptimer.h"
#include "vibeos/mbz.h"

typedef struct {
    uint8_t used;
    uint8_t clock;       /* vibeos_pclock_t */
    uint8_t notify;
    uint32_t tgid;
    int32_t id;
    uint32_t notify_tid;
    uint32_t charged;    /* THREAD_CPU: the thread whose time it counts */
    uint32_t signo;
    uint64_t value;
    uint64_t expires;    /* REAL: the tick it fires at; else ticks left; 0: disarmed */
    uint64_t interval;
    uint32_t overrun;    /* the overrun count of its signal, as timer_getoverrun says */
    uint32_t missed;     /* periods gone by with no tick to fire them, not yet told */
} pt_t;

static pt_t g_pt[VIBEOS_PTIMER_MAX];
/* Armed timers on a CPU-time clock. Read without the lock by every core's tick
 * to skip the table when there are none - which is nearly always - and only
 * written under it. A stale read costs one tick of charge, never a wrong one. */
static volatile uint32_t g_charged_armed;
static uint64_t g_fired, g_overruns;
static void (*g_lock)(void);
static void (*g_unlock)(void);

/* A call with no lock registered is counted (must be zero) rather than
 * trusted, as the file-lock table counts it: this table is reached from every
 * core's tick and every core's syscalls. Tests register one. */
static void lock(void) {
    if (g_lock) {
        g_lock();
    } else {
        vibeos_mbz_hit(VIBEOS_MBZ_PTIMER_UNLOCKED, 0);
    }
}

static void unlock(void) {
    if (g_unlock) {
        g_unlock();
    }
}

void vibeos_ptimer_set_lock(void (*l)(void), void (*u)(void)) {
    g_lock = l;
    g_unlock = u;
}

void vibeos_ptimer_reset(void) {
    uint32_t i;

    for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
        g_pt[i].used = 0;
    }
    g_charged_armed = 0;
    g_fired = 0;
    g_overruns = 0;
}

/* Under the lock, after anything that arms or disarms. */
static void recount(void) {
    uint32_t i, n = 0;

    for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
        if (g_pt[i].used && g_pt[i].clock != VIBEOS_PCLOCK_REAL && g_pt[i].expires != 0u) {
            n++;
        }
    }
    g_charged_armed = n;
}

static pt_t *find(uint32_t tgid, int32_t id) {
    uint32_t i;

    for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
        if (g_pt[i].used && g_pt[i].tgid == tgid && g_pt[i].id == id) {
            return &g_pt[i];
        }
    }
    return 0;
}

static pt_t *take_free(void) {
    uint32_t i;

    for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
        if (!g_pt[i].used) {
            pt_t *e = &g_pt[i];
            uint32_t k;
            for (k = 0; k < sizeof(*e); k++) {
                ((unsigned char *)e)[k] = 0;
            }
            e->used = 1;
            return e;
        }
    }
    return 0;
}

int vibeos_ptimer_create(uint32_t tgid, uint32_t clock, uint32_t notify, uint32_t notify_tid,
                         uint32_t charged, uint32_t signo, uint64_t value, int32_t *id_out) {
    pt_t *e;
    int32_t id = 0;

    lock();
    /* The smallest id the process is not using: Linux's numbering, which
     * programs print and tests compare. */
    while (find(tgid, id)) {
        id++;
    }
    /* No process takes more than its share of the table: one that created
     * timers in a loop used to take all of it, and every other process's
     * timer_create failed (external review, 2026-10-07). Linux bounds the same
     * thing per user with RLIMIT_SIGPENDING. The interval timers and alarm
     * have ids below zero and are not counted: they are a process's own three. */
    {
        uint32_t i, mine = 0;

        for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
            mine += g_pt[i].used && g_pt[i].tgid == tgid && g_pt[i].id >= 0;
        }
        if (mine >= VIBEOS_PTIMER_PER_PROCESS) {
            unlock();
            return -1;
        }
    }
    e = take_free();
    if (!e) {
        unlock();
        return -1;
    }
    e->tgid = tgid;
    e->id = id;
    e->clock = (uint8_t)clock;
    e->notify = (uint8_t)notify;
    e->notify_tid = notify_tid;
    e->charged = charged;
    e->signo = signo;
    e->value = value == VIBEOS_PTIMER_VALUE_IS_ID ? (uint64_t)(uint32_t)id : value;
    unlock();
    *id_out = id;
    return 0;
}

int vibeos_ptimer_clock(uint32_t tgid, int32_t id) {
    pt_t *e;
    int r;

    lock();
    e = find(tgid, id);
    r = e ? (int)e->clock : -1;
    unlock();
    return r;
}

/* What is left before `e` fires, in ticks; 0 only when it is disarmed. A REAL
 * timer was armed one tick late on purpose (it never fires early, see set), and
 * that tick is not reported: a timer just armed for T reads back as T. */
static uint64_t left_of(const pt_t *e, uint64_t now) {
    if (e->expires == 0u) {
        return 0;
    }
    if (e->clock != VIBEOS_PCLOCK_REAL) {
        return e->expires;
    }
    return e->expires > now + 1u ? e->expires - now - 1u : 1u;
}

int vibeos_ptimer_set(uint32_t tgid, int32_t id, uint64_t ticks, uint64_t interval,
                      int absolute, uint64_t now, uint64_t *old_left, uint64_t *old_interval) {
    pt_t *e;

    lock();
    e = find(tgid, id);
    if (!e && id < 0 && id >= VIBEOS_PTIMER_FIXED_LAST) {
        if (ticks == 0u) {
            /* Disarming an interval timer that was never armed. */
            unlock();
            *old_left = 0;
            *old_interval = 0;
            return 0;
        }
        e = take_free();
        if (e) {
            e->tgid = tgid;
            e->id = id;
            e->clock = (uint8_t)(id == VIBEOS_PTIMER_ITIMER(0) ? VIBEOS_PCLOCK_REAL
                               : id == VIBEOS_PTIMER_ITIMER(1) ? VIBEOS_PCLOCK_VIRT
                               : id == VIBEOS_PTIMER_ITIMER(2) ? VIBEOS_PCLOCK_PROF
                               : VIBEOS_PCLOCK_PROCESS_CPU);
            e->notify = VIBEOS_PTIMER_SIGNAL;
            e->signo = id == VIBEOS_PTIMER_ITIMER(0) ? 14u      /* SIGALRM   */
                     : id == VIBEOS_PTIMER_ITIMER(1) ? 26u      /* SIGVTALRM */
                     : id == VIBEOS_PTIMER_ITIMER(2) ? 27u      /* SIGPROF   */
                     : id == VIBEOS_PTIMER_RLIMIT_SOFT ? 24u    /* SIGXCPU   */
                     : 9u;                                      /* SIGKILL   */
        }
    }
    if (!e) {
        unlock();
        return -1;
    }
    *old_left = left_of(e, now);
    *old_interval = e->interval;
    e->interval = interval;
    e->missed = 0;
    e->overrun = 0;
    if (ticks == 0u) {
        e->expires = 0;
    } else if (e->clock != VIBEOS_PCLOCK_REAL) {
        e->expires = ticks;
    } else if (absolute) {
        /* A time already past fires on the next tick. */
        e->expires = ticks > now ? ticks : now + 1u;
    } else {
        /* One more than asked: the tick in progress has partly gone, and a
         * timer that fired early would be a timer that lied. */
        e->expires = now + ticks + 1u;
    }
    recount();
    unlock();
    return 0;
}

int vibeos_ptimer_get(uint32_t tgid, int32_t id, uint64_t now, uint64_t *left, uint64_t *interval) {
    pt_t *e;

    lock();
    e = find(tgid, id);
    if (!e) {
        unlock();
        if (id < 0 && id >= VIBEOS_PTIMER_FIXED_LAST) {
            *left = 0;   /* an interval timer never armed reads as disarmed */
            *interval = 0;
            return 0;
        }
        return -1;
    }
    *left = left_of(e, now);
    *interval = e->interval;
    unlock();
    return 0;
}

int vibeos_ptimer_overrun(uint32_t tgid, int32_t id) {
    pt_t *e;
    int r;

    lock();
    e = find(tgid, id);
    r = e ? (int)e->overrun : -1;
    unlock();
    return r;
}

int vibeos_ptimer_delete(uint32_t tgid, int32_t id) {
    pt_t *e;

    lock();
    e = find(tgid, id);
    if (e) {
        e->used = 0;
        recount();
    }
    unlock();
    return e ? 0 : -1;
}

static void drop(uint32_t tgid, int keep_itimers) {
    uint32_t i;

    lock();
    for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
        if (g_pt[i].used && g_pt[i].tgid == tgid && !(keep_itimers && g_pt[i].id < 0)) {
            g_pt[i].used = 0;
        }
    }
    recount();
    unlock();
}

void vibeos_ptimer_exec(uint32_t tgid) { drop(tgid, 1); }
void vibeos_ptimer_exit(uint32_t tgid) { drop(tgid, 0); }

/* At most this many expiries per pass. Fired from an interrupt, so the batch
 * is on a kernel stack that is not large; what does not fit fires on the next
 * tick, a tick late. */
#define PT_BATCH 16u

/* Describe an expiry, and hand over the periods missed: they are told with it. */
static void describe(pt_t *e, vibeos_ptimer_fire_t *f) {
    f->tgid = e->tgid;
    f->tid = e->notify_tid;
    f->to_thread = e->notify == VIBEOS_PTIMER_THREAD;
    f->signo = e->signo;
    f->id = e->id;
    f->value = e->value;
    f->overrun = e->missed;
    e->missed = 0;
}

/* Call back for each expiry, outside the lock, and then record the overrun
 * count of the timer's signal: what was missed between ticks, for a signal
 * just raised; the pending signal's whole count, for one that was not taken
 * yet - the callback kept that count in the signal, where sigtimedwait and the
 * handler read it. */
static void deliver(vibeos_ptimer_fire_t *f, uint32_t n, vibeos_ptimer_fire_fn fire) {
    uint32_t i;

    for (i = 0; i < n; i++) {
        int r = fire ? fire(&f[i]) : 0;
        pt_t *e;

        lock();
        g_fired++;
        if (r > 0) {
            g_overruns++;
        }
        e = find(f[i].tgid, f[i].id);
        if (e) {
            e->overrun = r > 0 ? (uint32_t)(r - 1) : f[i].overrun;
        }
        unlock();
    }
}

void vibeos_ptimer_tick(uint64_t now, vibeos_ptimer_fire_fn fire) {
    vibeos_ptimer_fire_t f[PT_BATCH];
    uint32_t i, n = 0;

    lock();
    for (i = 0; i < VIBEOS_PTIMER_MAX && n < PT_BATCH; i++) {
        pt_t *e = &g_pt[i];

        if (!e->used || e->clock != VIBEOS_PCLOCK_REAL || e->expires == 0u || e->expires > now) {
            continue;
        }
        if (e->interval == 0u) {
            e->expires = 0;
        } else {
            /* Every period that went by without a tick to fire it - a machine
             * too busy, or a timer shorter than a tick - is an overrun, and is
             * counted before this expiry is described, so that it is reported
             * with it. */
            e->expires += e->interval;
            while (e->expires <= now) {
                e->expires += e->interval;
                e->missed++;
            }
        }
        if (e->notify != VIBEOS_PTIMER_NONE) {
            describe(e, &f[n++]);
        }
    }
    unlock();
    deliver(f, n, fire);
}

void vibeos_ptimer_charge(uint32_t tgid, uint32_t tid, int user, vibeos_ptimer_fire_fn fire) {
    vibeos_ptimer_fire_t f[PT_BATCH];
    uint32_t i, n = 0;

    if (g_charged_armed == 0u) {
        return;
    }
    lock();
    for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
        pt_t *e = &g_pt[i];
        int counts;

        if (!e->used || e->expires == 0u) {
            continue;
        }
        switch (e->clock) {
            case VIBEOS_PCLOCK_VIRT:        counts = e->tgid == tgid && user; break;
            case VIBEOS_PCLOCK_PROF:
            case VIBEOS_PCLOCK_PROCESS_CPU: counts = e->tgid == tgid; break;
            case VIBEOS_PCLOCK_THREAD_CPU:  counts = e->tgid == tgid && e->charged == tid; break;
            default:                        counts = 0; break;
        }
        if (!counts || --e->expires != 0u) {
            continue;
        }
        if (e->notify != VIBEOS_PTIMER_NONE) {
            if (n < PT_BATCH) {
                describe(e, &f[n++]);
            } else {
                e->missed++;   /* no room this tick: counted, as Linux counts it */
            }
        }
        e->expires = e->interval;
    }
    recount();
    unlock();
    deliver(f, n, fire);
}

uint32_t vibeos_ptimer_used(void) {
    uint32_t i, n = 0;

    lock();
    for (i = 0; i < VIBEOS_PTIMER_MAX; i++) {
        n += g_pt[i].used;
    }
    unlock();
    return n;
}

uint64_t vibeos_ptimer_fired(void) { return g_fired; }
uint64_t vibeos_ptimer_overruns(void) { return g_overruns; }
