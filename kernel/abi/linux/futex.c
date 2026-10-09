/* Linux ABI: futex - the table of waiters, waiting and waking.
 *
 * The wait lived with the process syscalls and the table and the wake in the
 * architecture's task code, because exit wakes a joiner. None of it is x86-64:
 * it is a table, a lock, and a task made BLOCKED and READY again. Together here
 * since A2 (docs/abi/), and exit calls linux_futex_wake like any caller.
 *
 * Since docs/abi/ L6 it is the whole of Linux's futex but priority inheritance:
 * the bitset forms, requeue, WAKE_OP, the futex2 calls, and robust lists. */

#include "linux_internal.h"

/* The operation in the op word's low bits, below FUTEX_PRIVATE_FLAG (128).
 * Not Linux's FUTEX_CMD_MASK, which is ~(PRIVATE | CLOCK_REALTIME): the two
 * agree on every operation this file answers. */
#define VIBEOS_FUTEX_OP_BITS 0x7F

/* futex: the primitive every thread library builds its waiting on.
 *
 * The contract is deliberately odd and the oddity is the point. WAIT says
 * "sleep, but only if this word still holds the value I last saw"; the check
 * and the sleep happen together, under a lock, so a wake that arrives between
 * a thread reading the word and deciding to sleep cannot be lost. Without
 * that, a mutex hands out a lock to a thread that will never be told, which is
 * a hang and not a slowdown.
 *
 * Uncontended locks never come here at all - a library takes those with an
 * atomic instruction - so this is the path for contention and for joins.
 *
 * Waiters are matched on a key: the process and the address for a private
 * word, and the frame and the offset for a word on a MAP_SHARED page that the
 * caller did not mark private (linux_futex_key). This said, until L2 step 6,
 * that shared futexes were not implemented because there were no shared
 * mappings - true when written, false since L3, and every LTP test that waits
 * on its checkpoint (a futex on a MAP_SHARED page, between a parent and its
 * child) waited out its timeout instead.
 *
 * A sleeper is one task waiting on one word, or on several at once
 * (futex_waitv): an entry per word, all pointing at the first, the `leader`,
 * which is where "woken" and "by which word" are kept. A wake that matches any
 * of them wakes the sleeper once and counts once.
 */
typedef struct {
    /* `used` and `addr` are not the same question, and conflating them was a
     * bug worth keeping the distinction for. A woken waiter still owns its
     * slot until it returns - it is reading `woken` out of it - so the waker
     * clears `addr`, which stops further wakes from matching, and leaves
     * `used` alone. Freeing on the waker's side let a new waiter take the slot
     * while the old one was still in it: the old one then cleared the new
     * one's registration on its way out, and that thread slept forever with
     * every wake passing it by. */
    uint8_t used;
    uint64_t addr;     /* 0 once woken: no further wake should match */
    /* Whose address. A futex word is named by a user virtual address, and a
     * virtual address means nothing without its process: every Linux program
     * here links at 0x400000, and a forked child has its parent's layout
     * exactly. The table was keyed by address alone, so a wake in one process
     * ended a wait in another - THREADS_C5_FUTEX_XPROC, red first. Null for a
     * shared word, whose `addr` is then the frame's and is nobody's address. */
    const vibeos_procstate_t *ps;
    int task;
    /* Which tenancy of `task` enqueued. The slot index alone is an ABA: a
     * blocked waiter can be reaped and its slot handed to a new task, and a wake
     * matching by address would then set the wrong tenant READY - once, a task
     * that had already exited, scheduled onto a kernel stack being freed under
     * it (ready_by=futex_wake, the four-worker crash). A wake requires this to
     * still equal ks_seq(task), the same tenancy check H-007 uses. */
    uint32_t seq;
    uint32_t bitset;   /* WAIT_BITSET's; every bit for a plain WAIT */
    uint32_t leader;   /* the slot that holds this sleeper's state */
    uint32_t index;    /* which of a waitv's words this entry is */
    volatile int woken;      /* the leader's: woken at all */
    volatile int woken_by;   /* the leader's: which entry's word woke it */
} linux_futex_waiter_t;

static linux_futex_waiter_t g_futex_waiters[LINUX_FUTEX_TABLE];

static vibeos_lock_t g_futex_lock;

/* Under g_futex_lock: a slot nobody holds - free, or held by an enqueuer
 * whose task slot has since been reused. The wake's tenancy check will never
 * match such an entry again, so it is only holding a table slot; taking it
 * back is what bounds the table against a thread reaped while blocked, which
 * never runs futex_sleep's cleanup. */
static int futex_slot_free(uint32_t i) {
    return !g_futex_waiters[i].used || ks_seq(g_futex_waiters[i].task) != g_futex_waiters[i].seq;
}

/* Under g_futex_lock: wake the sleeper `e` belongs to, unless it was woken
 * already - by another of its words, or because its tenancy is gone. 1 if this
 * call woke it. */
static int futex_wake_entry(uint32_t e) {
    linux_futex_waiter_t *w = &g_futex_waiters[e];
    linux_futex_waiter_t *l = &g_futex_waiters[w->leader];

    /* The slot must still hold the very task that enqueued: a reaped-and-
     * reused slot is a different tenant, and waking it by a stale entry is
     * the ABA that scheduled an exited thread onto a stack being freed
     * (H-007, here in the futex table). alloc_seq is stable for the life of
     * a tenancy and changes on every reuse. */
    if (ks_seq(w->task) != w->seq || l->woken) {
        return 0;
    }
    w->addr = 0;   /* no second wake for this waiter */
    l->woken_by = (int)w->index;
    l->woken = 1;
    ks_lock(ks_sched_lock(), __func__);
    if (vibeos_task_state((uint32_t)w->task) == VIBEOS_TASK_BLOCKED &&
        ks_seq(w->task) == w->seq) {
        (void)ks_set_state(w->task, VIBEOS_TASK_READY, __func__);
        ks_mark_ready(w->task, "futex_wake");
    }
    ks_unlock(ks_sched_lock());
    return 1;
}

/* Under g_futex_lock: wake up to `count` sleepers on the word, whose bitset
 * shares a bit with `mask`. */
static long futex_wake_locked(const vibeos_procstate_t *ps, uint64_t key, uint32_t count,
                              uint32_t mask) {
    long woke = 0;
    uint32_t i;

    for (i = 0; i < LINUX_FUTEX_TABLE && (uint32_t)woke < count; i++) {
        linux_futex_waiter_t *w = &g_futex_waiters[i];

        if (!w->used || w->addr != key || w->ps != ps || (w->bitset & mask) == 0u) {
            continue;
        }
        woke += futex_wake_entry(i);
    }
    return woke;
}

/* Wake up to `count` waiters on `addr`. Returns how many were woken, which is
 * what the caller is told: a library uses it to decide whether it needs to
 * wake anybody else. */
static long futex_wake_mask(const vibeos_procstate_t *ps, uint64_t addr, uint32_t count,
                            uint32_t mask) {
    long woke;

    if (addr == 0u) {
        return 0;
    }
    ks_lock(&g_futex_lock, __func__);
    woke = futex_wake_locked(ps, addr, count, mask);
    ks_unlock(&g_futex_lock);
    ks_log(VIBEOS_LOG_DEBUG, 20u, addr, (uint64_t)woke, "futex wake");
    return woke;
}

long linux_futex_wake(const vibeos_procstate_t *ps, uint64_t addr, uint32_t count) {
    return futex_wake_mask(ps, addr, count, LINUX_FUTEX_BITSET_MATCH_ANY);
}

/* Which word a futex names: its process and its address, or - for a word the
 * caller did not mark private, on a page mapped MAP_SHARED - the page's frame
 * and the offset in it, with no process. The same word in every process that
 * maps it, wherever each maps it: how a parent and a child wait for each other
 * through shared memory, as LTP's checkpoints do. A word on a private page is
 * its process's whatever the flag says, as in Linux. The word is read first,
 * so that its page is in: a frame that is not there has no identity yet.
 * Linux refuses a word that is not four-byte aligned with EINVAL, before
 * anything else. */
static long linux_futex_key(uint64_t addr, int private_word, const vibeos_procstate_t **ps,
                            uint64_t *key) {
    int me = ks_current();
    uint32_t word;

    if (addr & 3u) {
        return -VIBEOS_EINVAL;
    }
    *ps = me >= 0 ? ks_ps(me) : 0;
    *key = addr;
    if (me < 0 || private_word) {
        return 0;
    }
    if (!linux_user_ok(addr, 4u, 0) ||
        vibeos_uaccess_copy(&word, (const void *)(uintptr_t)addr, 4u) != 0) {
        return -VIBEOS_EFAULT;
    }
    {
        vibeos_pageinfo_t pi;

        ks_pageinfo(me, addr & ~0xFFFull, &pi);
        if ((pi.flags & VIBEOS_PAGE_PRESENT) && (pi.flags & VIBEOS_PAGE_SHARED)) {
            *ps = 0;
            *key = (pi.frame << 12) | (addr & 0xFFFull);
        }
    }
    return 0;
}

/* One word of a wait: where it is, what it must still hold, how it is named. */
typedef struct {
    uint64_t addr;
    uint64_t key;
    const vibeos_procstate_t *ps;
    uint32_t expected;
} futex_word_t;

/* Compare `n` words and enqueue a sleeper on all of them, under one hold of the
 * lock, so a wake on any of them between the compare and the sleep is not
 * lost; a word that has already moved is EAGAIN, as for one. 0 with the
 * sleeper's leader slot, or a negated errno. `words` is not needed after. */
static long futex_enqueue(const futex_word_t *words, uint32_t n, uint32_t bitset,
                          uint64_t deadline, uint32_t *out_leader) {
    uint32_t i, k, leader = 0;
    int me = ks_current();

    if (me < 0 || n == 0u || n > LINUX_FUTEX_WAITV_MAX) {
        return -VIBEOS_EINVAL;
    }
    ks_lock(&g_futex_lock, __func__);
    /* The compare and the enqueue are one step. Reading the word first and
     * enqueuing after would leave a window in which a waker sees no waiter and
     * the waiter then sleeps on a value that has already changed - the lost
     * wakeup, which presents as a program that stops for no reason. Through the
     * fault-tolerant copy: g_futex_lock can spin between a check and this read
     * (H-003). */
    for (i = 0; i < n; i++) {
        uint32_t cur = 0;

        /* Judged before it is read. The copy survives a page that went away;
         * it does not ask whose the page is, and a Linux program's null page
         * is the kernel's, present - so a word at 0 was read, and a vector
         * naming it waited out its timeout where Linux says EFAULT (LTP's
         * futex_waitv01). */
        if (!linux_user_ok(words[i].addr, 4u, 0) ||
            vibeos_uaccess_copy(&cur, (const void *)(uintptr_t)words[i].addr, 4u) != 0) {
            ks_unlock(&g_futex_lock);
            return -VIBEOS_EFAULT;
        }
        if (cur != words[i].expected) {
            ks_unlock(&g_futex_lock);
            ks_log(VIBEOS_LOG_DEBUG, 21u, words[i].addr, (uint64_t)words[i].expected,
                   "futex wait: value already moved");
            return -VIBEOS_EAGAIN;
        }
    }
    /* Counted first, so that a vector that does not fit takes nothing. */
    for (i = 0, k = 0; i < LINUX_FUTEX_TABLE && k < n; i++) {
        k += futex_slot_free(i) ? 1u : 0u;
    }
    if (k < n) {
        ks_unlock(&g_futex_lock);
        return -VIBEOS_ENOMEM;
    }
    for (i = 0, k = 0; i < LINUX_FUTEX_TABLE && k < n; i++) {
        linux_futex_waiter_t *w = &g_futex_waiters[i];

        if (!futex_slot_free(i)) {
            continue;
        }
        if (k == 0u) {
            leader = i;
        }
        w->used = 1;
        w->addr = words[k].key;
        w->ps = words[k].ps;
        w->task = me;
        w->seq = ks_seq(me);
        w->bitset = bitset;
        w->leader = leader;
        w->index = k;
        w->woken = 0;
        w->woken_by = -1;
        k++;
    }

    /* A wait with a timeout stays runnable and looks at the clock each tick,
     * as a pipe's wait does: BLOCKED is left only by a wake or a signal, and
     * nothing would come to say the time was up. */
    if (deadline == 0u) {
        ks_lock(ks_sched_lock(), __func__);
        (void)ks_set_state(me, VIBEOS_TASK_BLOCKED, __func__);
        ks_unlock(ks_sched_lock());
    }
    ks_unlock(&g_futex_lock);
    ks_log(VIBEOS_LOG_DEBUG, 22u, words[0].addr,
           (uint64_t)words[0].expected | ((uint64_t)ks_id(me)->pid << 32),
           "futex wait: sleeping (a1 = value | tid<<32)");
    *out_leader = leader;
    return 0;
}

/* Sleep until the sleeper `leader` heads is woken, a signal, or - with a
 * deadline - until `deadline` ticks, 0 for none. The index of the word that
 * woke it, or a negated errno. */
static long futex_sleep(uint32_t leader, uint64_t deadline, uint64_t addr) {
    int me = ks_current();
    uint32_t seq = ks_seq(me), i;
    long r;

    /* Yield until somebody wakes us. The scheduler runs from the timer, so
     * this is a wait and not a spin: the core is given away on the first
     * interrupt and this task is not runnable again until a wake says so.
     *
     * Or until a signal that must be acted on is pending. The task was made
     * BLOCKED before this loop, so a signal raised after that finds it BLOCKED
     * and makes it runnable, and a signal raised before it is seen by the check
     * on the first pass. Checking before blocking would leave a window in which
     * neither happens. */
    while (!g_futex_waiters[leader].woken) {
        if (ks_signal_interrupts(me)) {
            break;
        }
        if (deadline != 0u) {
            if (ks_ticks() >= deadline) {
                break;
            }
            ks_wait_tick();   /* a wait with an end: the clock has to move */
        } else {
            ks_block_point();
        }
    }

    /* Woken or interrupted is decided under the lock a waker takes. Read
     * outside it, a FUTEX_WAKE landing between the loop and here would count
     * this waiter as woken while it returned EINTR - and the thread that wake
     * was meant for would never be told. A wake that got in first wins: the
     * call returns and the signal is delivered on the way out all the same. */
    ks_lock(&g_futex_lock, __func__);
    r = g_futex_waiters[leader].woken ? (long)g_futex_waiters[leader].woken_by : -1;
    for (i = 0; i < LINUX_FUTEX_TABLE; i++) {
        linux_futex_waiter_t *w = &g_futex_waiters[i];

        if (w->used && w->leader == leader && w->task == me && w->seq == seq) {
            w->addr = 0;
            w->used = 0;   /* released by its owner, and only here */
        }
    }
    ks_unlock(&g_futex_lock);
    if (r < 0) {
        /* The signal may have been raised before this task was BLOCKED, in
         * which case nothing made it runnable again. It is running now; say
         * so, the same transition raising a signal uses. */
        ks_lock(ks_sched_lock(), __func__);
        if (vibeos_task_state((uint32_t)me) == VIBEOS_TASK_BLOCKED) {
            (void)ks_set_state(me, VIBEOS_TASK_READY, __func__);
            ks_mark_ready(me, "futex_wait_interrupted");
        }
        ks_unlock(ks_sched_lock());
        if (!ks_signal_interrupts(me) && deadline != 0u && ks_ticks() >= deadline) {
            return -VIBEOS_ETIMEDOUT;
        }
        ks_log(VIBEOS_LOG_DEBUG, 23u, addr, (uint64_t)ks_id(me)->pid,
               "futex wait: interrupted by a signal");
        return -VIBEOS_EINTR;
    }
    ks_log(VIBEOS_LOG_DEBUG, 23u, addr, (uint64_t)ks_id(me)->pid, "futex wait: woken");
    return r;
}

/* A timeout as a deadline in ticks: 0 for none, 1 for a time already past
 * (still a deadline, so the wait ends at once), or a negated errno. Relative
 * for FUTEX_WAIT, absolute for WAIT_BITSET and the futex2 calls - on any clock,
 * since every clock here counts the ticks since boot (timer.c). */
static long futex_deadline(uint64_t utimeout, int absolute, uint64_t *deadline) {
    linux_timespec_t ts;
    int64_t t;

    *deadline = 0;
    if (utimeout == 0u) {
        return 0;
    }
    if (!linux_user_ok(utimeout, sizeof(ts), 0) ||
        vibeos_uaccess_copy(&ts, (const void *)(uintptr_t)utimeout, sizeof(ts)) != 0) {
        return -VIBEOS_EFAULT;
    }
    /* Through the one conversion that saturates: tv_sec * hz in 64 bits
     * wrapped for a huge timeout, which then ended almost at once instead of
     * all but never (external review, 2026-10-07). */
    if ((t = linux_ticks_of(&ts)) < 0) {
        return -VIBEOS_EINVAL;
    }
    if (absolute) {
        *deadline = (uint64_t)t > ks_ticks() ? (uint64_t)t : 1u;
    } else {
        *deadline = ks_ticks() + (t ? (uint64_t)t : 1u);
    }
    return 0;
}

/* Wake `nwake` on one word and move up to `nmove` of the rest onto another, so
 * they are woken by the second word's wakes: how a condition variable hands
 * its waiters to the mutex without waking them all to fight over it. With
 * `check`, only if the first word still holds `expected` - CMP_REQUEUE's
 * guard against a waiter that arrived after the caller looked. Linux counts the
 * moved with the woken in what it returns, for both forms. */
static long futex_requeue(uint64_t a1, int priv1, uint64_t a2, int priv2, uint32_t nwake,
                          uint32_t nmove, int check, uint32_t expected) {
    const vibeos_procstate_t *ps1, *ps2;
    uint64_t k1, k2;
    long r, done;
    uint32_t i;

    if ((int32_t)nwake < 0 || (int32_t)nmove < 0) {
        return -VIBEOS_EINVAL;
    }
    if ((r = linux_futex_key(a1, priv1, &ps1, &k1)) != 0 ||
        (r = linux_futex_key(a2, priv2, &ps2, &k2)) != 0) {
        return r;
    }
    ks_lock(&g_futex_lock, __func__);
    if (check) {
        uint32_t cur = 0;

        if (!linux_user_ok(a1, 4u, 0) ||
            vibeos_uaccess_copy(&cur, (const void *)(uintptr_t)a1, 4u) != 0) {
            ks_unlock(&g_futex_lock);
            return -VIBEOS_EFAULT;
        }
        if (cur != expected) {
            ks_unlock(&g_futex_lock);
            return -VIBEOS_EAGAIN;
        }
    }
    done = futex_wake_locked(ps1, k1, nwake, LINUX_FUTEX_BITSET_MATCH_ANY);
    for (i = 0; i < LINUX_FUTEX_TABLE && nmove > 0u; i++) {
        linux_futex_waiter_t *w = &g_futex_waiters[i];

        if (!w->used || w->addr != k1 || w->ps != ps1 || ks_seq(w->task) != w->seq ||
            g_futex_waiters[w->leader].woken) {
            continue;
        }
        w->addr = k2;
        w->ps = ps2;
        nmove--;
        done++;
    }
    ks_unlock(&g_futex_lock);
    return done;
}

/* WAKE_OP: change a second word atomically, wake waiters on the first, and on
 * the second as well if its old value passes a comparison - all in one call,
 * so the change and the wakes cannot be told apart from outside. The change is
 * a compare-exchange on the user's word: other threads change it with their
 * own atomics, and a read followed by a write would lose theirs. */
static long futex_wake_op(uint64_t a1, int priv, uint64_t a2, uint32_t nwake1, uint32_t nwake2,
                          uint32_t encoded) {
    uint32_t op = (encoded >> 28) & 0xFu, cmp = (encoded >> 24) & 0xFu;
    int32_t oparg = (int32_t)(encoded << 8) >> 20, cmparg = (int32_t)(encoded << 20) >> 20;
    const vibeos_procstate_t *ps1, *ps2;
    uint64_t k1, k2;
    uint32_t old, want;
    long r, done;
    int pass;

    if (op & LINUX_FUTEX_OP_OPARG_SHIFT) {
        oparg = (int32_t)(1u << ((uint32_t)oparg & 31u));   /* Linux masks a shift past 31 */
        op &= ~(uint32_t)LINUX_FUTEX_OP_OPARG_SHIFT;
    }
    if (op > LINUX_FUTEX_OP_XOR || cmp > LINUX_FUTEX_OP_CMP_GE) {
        return -VIBEOS_ENOSYS;
    }
    if ((r = linux_futex_key(a1, priv, &ps1, &k1)) != 0 ||
        (r = linux_futex_key(a2, priv, &ps2, &k2)) != 0) {
        return r;
    }
    if (!linux_user_ok(a2, 4u, 1)) {
        return -VIBEOS_EFAULT;
    }
    ks_lock(&g_futex_lock, __func__);
    if (vibeos_uaccess_copy(&old, (const void *)(uintptr_t)a2, 4u) != 0) {
        ks_unlock(&g_futex_lock);
        return -VIBEOS_EFAULT;
    }
    for (;;) {
        uint32_t seen = old;

        switch (op) {
            case LINUX_FUTEX_OP_SET:  want = (uint32_t)oparg; break;
            case LINUX_FUTEX_OP_ADD:  want = old + (uint32_t)oparg; break;
            case LINUX_FUTEX_OP_OR:   want = old | (uint32_t)oparg; break;
            case LINUX_FUTEX_OP_ANDN: want = old & ~(uint32_t)oparg; break;
            default:                  want = old ^ (uint32_t)oparg; break;
        }
        if (ks_user_cmpxchg32(a2, &seen, want) != 0) {
            ks_unlock(&g_futex_lock);
            return -VIBEOS_EFAULT;
        }
        if (seen == old) {
            break;
        }
        old = seen;   /* another thread changed it: again, from what it holds now */
    }
    switch (cmp) {
        case LINUX_FUTEX_OP_CMP_EQ: pass = (int32_t)old == cmparg; break;
        case LINUX_FUTEX_OP_CMP_NE: pass = (int32_t)old != cmparg; break;
        case LINUX_FUTEX_OP_CMP_LT: pass = (int32_t)old < cmparg; break;
        case LINUX_FUTEX_OP_CMP_LE: pass = (int32_t)old <= cmparg; break;
        case LINUX_FUTEX_OP_CMP_GT: pass = (int32_t)old > cmparg; break;
        default:                    pass = (int32_t)old >= cmparg; break;
    }
    done = futex_wake_locked(ps1, k1, nwake1, LINUX_FUTEX_BITSET_MATCH_ANY);
    if (pass) {
        done += futex_wake_locked(ps2, k2, nwake2, LINUX_FUTEX_BITSET_MATCH_ANY);
    }
    ks_unlock(&g_futex_lock);
    return done;
}

/* One word, waited on: FUTEX_WAIT and WAIT_BITSET, and futex_wait. */
static long futex_wait_one(uint64_t addr, int priv, uint32_t expected, uint32_t bitset,
                           uint64_t deadline) {
    futex_word_t w;
    uint32_t leader;
    long r;

    if (addr == 0u) {
        return -VIBEOS_EINVAL;   /* the address is the row's: IN_IFM_ERR, EINVAL */
    }
    if ((r = linux_futex_key(addr, priv, &w.ps, &w.key)) != 0) {
        return r;
    }
    w.addr = addr;
    w.expected = expected;
    if ((r = futex_enqueue(&w, 1u, bitset, deadline, &leader)) != 0) {
        return r;
    }
    r = futex_sleep(leader, deadline, addr);
    return r < 0 ? r : 0;
}

static long linux_sys_futex(uint64_t addr, uint64_t op, uint64_t val, uint64_t utimeout,
                            uint64_t addr2, uint64_t val3) {
    int priv = (op & LINUX_FUTEX_PRIVATE_FLAG) != 0u;
    uint32_t cmd = (uint32_t)op & VIBEOS_FUTEX_OP_BITS;
    uint64_t deadline = 0;
    const vibeos_procstate_t *kps;
    uint64_t key;
    long r;

    /* CLOCK_REALTIME is accepted only where a timeout can be absolute - and on
     * FUTEX_WAIT, since Linux 4.5. Every clock here is the same count, so it
     * changes nothing but what is refused. */
    if ((op & LINUX_FUTEX_CLOCK_REALTIME) &&
        cmd != LINUX_FUTEX_WAIT && cmd != LINUX_FUTEX_WAIT_BITSET) {
        return -VIBEOS_ENOSYS;
    }
    switch (cmd) {
        case LINUX_FUTEX_WAKE:
            if ((r = linux_futex_key(addr, priv, &kps, &key)) != 0) {
                return r;
            }
            return futex_wake_mask(kps, key, (uint32_t)val, LINUX_FUTEX_BITSET_MATCH_ANY);
        case LINUX_FUTEX_WAKE_BITSET:
            if ((uint32_t)val3 == 0u) {
                return -VIBEOS_EINVAL;
            }
            if ((r = linux_futex_key(addr, priv, &kps, &key)) != 0) {
                return r;
            }
            return futex_wake_mask(kps, key, (uint32_t)val, (uint32_t)val3);
        case LINUX_FUTEX_WAIT:
            /* A relative timeout, which every C library's timed wait passes
             * and which used to be ignored: a timed wait that nobody ended
             * waited for ever. */
            if ((r = futex_deadline(utimeout, 0, &deadline)) != 0) {
                return r;
            }
            return futex_wait_one(addr, priv, (uint32_t)val, LINUX_FUTEX_BITSET_MATCH_ANY, deadline);
        case LINUX_FUTEX_WAIT_BITSET:
            /* Absolute, which is what lets a condition variable wait until a
             * time rather than for a length it would have to recompute. */
            if ((uint32_t)val3 == 0u) {
                return -VIBEOS_EINVAL;
            }
            if ((r = futex_deadline(utimeout, 1, &deadline)) != 0) {
                return r;
            }
            return futex_wait_one(addr, priv, (uint32_t)val, (uint32_t)val3, deadline);
        case LINUX_FUTEX_REQUEUE:
            return futex_requeue(addr, priv, addr2, priv, (uint32_t)val, (uint32_t)utimeout, 0, 0);
        case LINUX_FUTEX_CMP_REQUEUE:
            return futex_requeue(addr, priv, addr2, priv, (uint32_t)val, (uint32_t)utimeout, 1,
                                 (uint32_t)val3);
        case LINUX_FUTEX_WAKE_OP:
            return futex_wake_op(addr, priv, addr2, (uint32_t)val, (uint32_t)utimeout, (uint32_t)val3);
        default:
            /* Loudly, because this is how a thread library silently stops
             * working: it asks for an operation, is told it does not exist,
             * and carries on believing the wakeup it requested happened. The
             * priority-inheritance operations land here: no program this
             * kernel runs has asked for them. */
            ks_log(VIBEOS_LOG_WARN, 25u, addr, op, "futex: unsupported operation");
            return -VIBEOS_ENOSYS;
    }
}

/* ---- futex2 (Linux 6.7): the same operations, a flags word per futex ---------
 *
 * The flags say the word's size and whether it is private. Only 32-bit words
 * exist here, as on Linux today; any other size, NUMA, or a bit Linux does not
 * define is EINVAL. */
static int futex2_flags_ok(uint64_t flags) {
    return (flags & ~(uint64_t)(LINUX_FUTEX2_SIZE_MASK | LINUX_FUTEX2_PRIVATE)) == 0u &&
           (flags & LINUX_FUTEX2_SIZE_MASK) == LINUX_FUTEX2_SIZE_U32;
}

/* A futex2 timeout: absolute, on CLOCK_MONOTONIC or CLOCK_REALTIME only. */
static long futex2_deadline(uint64_t utimeout, uint64_t clk, uint64_t *deadline) {
    if (utimeout != 0u && VIBEOS_ARG_INT(clk) != LINUX_CLOCK_MONOTONIC &&
        VIBEOS_ARG_INT(clk) != LINUX_CLOCK_REALTIME) {
        return -VIBEOS_EINVAL;
    }
    return futex_deadline(utimeout, 1, deadline);
}

static long linux_sys_futex_wake(uint64_t addr, uint64_t mask, uint64_t nr, uint64_t flags) {
    const vibeos_procstate_t *ps;
    uint64_t key;
    long r;

    if (!futex2_flags_ok(flags) || (uint32_t)mask == 0u) {
        return -VIBEOS_EINVAL;
    }
    if ((r = linux_futex_key(addr, (flags & LINUX_FUTEX2_PRIVATE) != 0u, &ps, &key)) != 0) {
        return r;
    }
    return futex_wake_mask(ps, key, (uint32_t)(nr > 0x7FFFFFFFu ? 0x7FFFFFFFu : nr), (uint32_t)mask);
}

static long linux_sys_futex_wait(uint64_t addr, uint64_t val, uint64_t mask, uint64_t flags,
                                 uint64_t utimeout, uint64_t clk) {
    uint64_t deadline;
    long r;

    if (!futex2_flags_ok(flags) || (uint32_t)mask == 0u || val > 0xFFFFFFFFull) {
        return -VIBEOS_EINVAL;
    }
    if ((r = futex2_deadline(utimeout, clk, &deadline)) != 0) {
        return r;
    }
    return futex_wait_one(addr, (flags & LINUX_FUTEX2_PRIVATE) != 0u, (uint32_t)val,
                          (uint32_t)mask, deadline);
}

/* Read and judge a vector of futex2 words: each one's flags, no reserved bits,
 * a value that fits its size. */
static long futex2_read_vector(uint64_t uptr, uint32_t n, linux_futex_waitv_t *v) {
    uint32_t i;

    if (!linux_user_ok(uptr, (uint64_t)n * sizeof(*v), 0) ||
        vibeos_uaccess_copy(v, (const void *)(uintptr_t)uptr, (uint64_t)n * sizeof(*v)) != 0) {
        return -VIBEOS_EFAULT;
    }
    for (i = 0; i < n; i++) {
        if (!futex2_flags_ok(v[i].flags) || v[i].__reserved != 0u || v[i].val > 0xFFFFFFFFull) {
            return -VIBEOS_EINVAL;
        }
    }
    return 0;
}

static long linux_sys_futex_requeue(uint64_t uwaiters, uint64_t flags, uint64_t nwake,
                                    uint64_t nmove) {
    linux_futex_waitv_t v[2];
    long r;

    if (flags != 0u) {
        return -VIBEOS_EINVAL;
    }
    if ((r = futex2_read_vector(uwaiters, 2u, v)) != 0) {
        return r;
    }
    return futex_requeue(v[0].uaddr, (v[0].flags & LINUX_FUTEX2_PRIVATE) != 0u,
                         v[1].uaddr, (v[1].flags & LINUX_FUTEX2_PRIVATE) != 0u,
                         (uint32_t)nwake, (uint32_t)nmove, 1, (uint32_t)v[0].val);
}

/* futex_waitv: sleep on up to 128 words at once, and say which one woke us. */
static long linux_sys_futex_waitv(uint64_t uwaiters, uint64_t n, uint64_t flags,
                                  uint64_t utimeout, uint64_t clk) {
    static linux_futex_waitv_t v[LINUX_FUTEX_WAITV_MAX];     /* under g_waitv_lock */
    static futex_word_t words[LINUX_FUTEX_WAITV_MAX];
    static vibeos_lock_t g_waitv_lock;
    uint64_t deadline, first;
    uint32_t i, leader = 0;
    long r;

    if (flags != 0u || n == 0u || n > LINUX_FUTEX_WAITV_MAX || uwaiters == 0u) {
        return -VIBEOS_EINVAL;
    }
    if ((r = futex2_deadline(utimeout, clk, &deadline)) != 0) {
        return r;
    }
    /* The vector is read and keyed into static arrays - 128 words of each
     * would not fit a kernel stack - under a lock of their own, held only
     * until the sleeper is enqueued: what the sleep needs is in the table. */
    ks_lock_preemptible(&g_waitv_lock);
    if ((r = futex2_read_vector(uwaiters, (uint32_t)n, v)) == 0) {
        for (i = 0; i < (uint32_t)n && r == 0; i++) {
            words[i].addr = v[i].uaddr;
            words[i].expected = (uint32_t)v[i].val;
            r = linux_futex_key(v[i].uaddr, (v[i].flags & LINUX_FUTEX2_PRIVATE) != 0u,
                                &words[i].ps, &words[i].key);
        }
    }
    if (r == 0) {
        r = futex_enqueue(words, (uint32_t)n, LINUX_FUTEX_BITSET_MATCH_ANY, deadline, &leader);
    }
    first = words[0].addr;
    ks_unlock_preemptible(&g_waitv_lock);
    return r != 0 ? r : futex_sleep(leader, deadline, first);
}

/* ---- robust lists -------------------------------------------------------------
 *
 * A thread that dies holding a lock leaves every other thread waiting on it for
 * ever, unless the kernel knows which locks it held. A C library keeps a list of
 * them in user memory and tells the kernel where (set_robust_list); at exit the
 * kernel walks it and, for each lock word still naming the thread, marks it
 * OWNER_DIED and wakes one waiter - who takes the lock and is told by EOWNERDEAD
 * that what it protects may be half-changed. Bounded by Linux's limit, so a
 * list a program corrupted into a cycle ends. */

static long linux_sys_set_robust_list(uint64_t head, uint64_t len) {
    int me = ks_current();

    if (len != sizeof(linux_robust_list_head_t)) {
        return -VIBEOS_EINVAL;
    }
    if (me >= 0) {
        ks_id(me)->robust_head = head;
    }
    return 0;
}

/* Another thread's list only for whoever could read its memory: the same
 * question /proc asks (Linux: ptrace read access). */
static long linux_sys_get_robust_list(uint64_t tid, uint64_t uhead, uint64_t ulen) {
    uint64_t head, len = sizeof(linux_robust_list_head_t);
    int t;

    if (VIBEOS_ARG_INT(tid) == 0) {
        t = ks_current();
    } else {
        ks_lock(ks_sched_lock(), __func__);
        t = ks_task_by_tid((uint32_t)tid);
        ks_unlock(ks_sched_lock());
    }
    if (t < 0 || !ks_id(t)->is_user) {
        return -VIBEOS_ESRCH;
    }
    if (t != ks_current()) {
        vibeos_cred_t me, them;
        const vibeos_procstate_t *ps = ks_ps(t);

        linux_cred(&me);
        if (!ps) {
            return -VIBEOS_ESRCH;
        }
        them = ps->cred;
        if (me.euid != 0u && (me.uid != them.euid || me.uid != them.suid || me.uid != them.uid)) {
            return -VIBEOS_EPERM;
        }
    }
    head = ks_id(t)->robust_head;
    if (vibeos_uaccess_copy((void *)(uintptr_t)ulen, &len, sizeof(len)) != 0 ||
        vibeos_uaccess_copy((void *)(uintptr_t)uhead, &head, sizeof(head)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* One lock word of a dying thread: if it still names the thread, mark it
 * OWNER_DIED, keep the waiters bit, and wake one waiter if there is one. A
 * word changed under us is read again; a word that cannot be reached ends the
 * walk, as Linux's does. */
static int futex_owner_died(uint64_t addr, uint32_t tid, int pending) {
    uint32_t seen, want;
    int guard;

    if ((addr & 3u) != 0u || !linux_user_ok(addr, 4u, 1) ||
        vibeos_uaccess_copy(&seen, (const void *)(uintptr_t)addr, 4u) != 0) {
        return -1;
    }
    for (guard = 0; guard < 64; guard++) {
        uint32_t old = seen;

        /* A pending entry whose word is 0: the thread died after taking the
         * lock's slot and before naming itself in it. Linux wakes a waiter so
         * the lock is not left with sleepers and no owner. */
        if (pending && seen == 0u) {
            (void)linux_futex_wake(ks_current() >= 0 ? ks_ps(ks_current()) : 0, addr, 1u);
            return 0;
        }
        if ((seen & LINUX_FUTEX_TID_MASK) != tid) {
            return 0;   /* not this thread's: leave it alone */
        }
        want = (seen & LINUX_FUTEX_WAITERS) | LINUX_FUTEX_OWNER_DIED;
        if (ks_user_cmpxchg32(addr, &seen, want) != 0) {
            return -1;
        }
        if (seen == old) {
            if (old & LINUX_FUTEX_WAITERS) {
                const vibeos_procstate_t *ps;
                uint64_t key;

                /* Shared or private: a robust mutex is often process-shared,
                 * and its waiters are in other processes. */
                if (linux_futex_key(addr, 0, &ps, &key) == 0) {
                    (void)linux_futex_wake(ps, key, 1u);
                }
            }
            return 0;
        }
    }
    return 0;
}

void linux_futex_exit_robust(int slot) {
    linux_robust_list_head_t h;
    uint64_t entry, head;
    uint32_t tid, n;

    if (slot < 0 || (head = ks_id(slot)->robust_head) == 0u) {
        return;
    }
    ks_id(slot)->robust_head = 0;
    tid = ks_id(slot)->pid;
    if (!linux_user_ok(head, sizeof(h), 0) ||
        vibeos_uaccess_copy(&h, (const void *)(uintptr_t)head, sizeof(h)) != 0) {
        return;
    }
    /* The low bit of an entry pointer says the lock is a PI one; the word is
     * found the same way. */
    entry = h.next;
    for (n = 0; n < LINUX_ROBUST_LIST_LIMIT && entry != head && entry != 0u; n++) {
        uint64_t next;
        uint64_t at = entry & ~1ull;

        if (!linux_user_ok(at, 8u, 0) ||
            vibeos_uaccess_copy(&next, (const void *)(uintptr_t)at, 8u) != 0) {
            return;
        }
        /* The entry being added or removed when the thread died is handled
         * after the walk, not twice. */
        if (at != (h.list_op_pending & ~1ull) &&
            futex_owner_died(at + (uint64_t)h.futex_offset, tid, 0) != 0) {
            return;
        }
        entry = next;
    }
    if (h.list_op_pending != 0u) {
        (void)futex_owner_died((h.list_op_pending & ~1ull) + (uint64_t)h.futex_offset, tid, 1);
    }
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_FUTEX_SYSCALLS(X) \
    X(202, futex,            FUTEX,           PTRS(IN_IFM_ERR(1, VIBEOS_FUTEX_OP_BITS, LINUX_FUTEX_WAIT, 0, 4, VIBEOS_EINVAL)), linux_sys_futex(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(273, set_robust_list,  SET_ROBUST_LIST, NOPTR, linux_sys_set_robust_list(ARG(0), ARG(1))) \
    X(274, get_robust_list,  GET_ROBUST_LIST, PTRS(OUT(1, 8), OUT(2, 8)), linux_sys_get_robust_list(ARG(0), ARG(1), ARG(2))) \
    X(454, futex_wake,       FUTEX,           NOPTR, linux_sys_futex_wake(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(455, futex_wait,       FUTEX,           NOPTR, linux_sys_futex_wait(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(456, futex_requeue,    FUTEX,           NOPTR, linux_sys_futex_requeue(ARG(0), ARG(1), ARG(2), ARG(3))) \
    X(449, futex_waitv,      FUTEX,           NOPTR, linux_sys_futex_waitv(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)))

LINUX_DEFINE_SYSCALLS(futex, LINUX_FUTEX_SYSCALLS)
