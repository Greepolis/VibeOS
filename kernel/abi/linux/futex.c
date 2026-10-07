/* Linux ABI: futex - the table of waiters, waiting and waking.
 *
 * The wait lived with the process syscalls and the table and the wake in the
 * architecture's task code, because exit wakes a joiner. None of it is x86-64:
 * it is a table, a lock, and a task made BLOCKED and READY again. Together here
 * since A2 (docs/abi/), and exit calls linux_futex_wake like any caller. */

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
    volatile int woken;
} linux_futex_waiter_t;

static linux_futex_waiter_t g_futex_waiters[LINUX_FUTEX_WAITERS];

static vibeos_lock_t g_futex_lock;

/* Wake up to `count` waiters on `addr`. Returns how many were woken, which is
 * what the caller is told: a library uses it to decide whether it needs to
 * wake anybody else. */
long linux_futex_wake(const vibeos_procstate_t *ps, uint64_t addr, uint32_t count) {
    long woke = 0;
    uint32_t i;

    if (addr == 0u) {
        return 0;
    }
    ks_lock(&g_futex_lock, __func__);
    for (i = 0; i < LINUX_FUTEX_WAITERS && (uint32_t)woke < count; i++) {
        if (!g_futex_waiters[i].used || g_futex_waiters[i].addr != addr ||
            g_futex_waiters[i].ps != ps) {
            continue;
        }
        /* The slot must still hold the very task that enqueued: a reaped-and-
         * reused slot is a different tenant, and waking it by a stale entry is
         * the ABA that scheduled an exited thread onto a stack being freed
         * (H-007, here in the futex table). alloc_seq is stable for the life of
         * a tenancy and changes on every reuse. */
        if (ks_seq(g_futex_waiters[i].task) != g_futex_waiters[i].seq) {
            continue;   /* stale entry; the enqueuer is long gone */
        }
        g_futex_waiters[i].addr = 0;   /* no second wake for this waiter */
        g_futex_waiters[i].woken = 1;
        ks_lock(ks_sched_lock(), __func__);
        if (vibeos_task_state((uint32_t)g_futex_waiters[i].task) == VIBEOS_TASK_BLOCKED &&
            ks_seq(g_futex_waiters[i].task) == g_futex_waiters[i].seq) {
            (void)ks_set_state(g_futex_waiters[i].task, VIBEOS_TASK_READY, __func__);
            ks_mark_ready(g_futex_waiters[i].task, "futex_wake");
        }
        ks_unlock(ks_sched_lock());
        woke++;
    }
    ks_unlock(&g_futex_lock);
    ks_log(VIBEOS_LOG_DEBUG, 20u, addr, (uint64_t)woke, "futex wake");
    return woke;
}

/* Which word a futex names: its process and its address, or - for a word the
 * caller did not mark private, on a page mapped MAP_SHARED - the page's frame
 * and the offset in it, with no process. The same word in every process that
 * maps it, wherever each maps it: how a parent and a child wait for each other
 * through shared memory, as LTP's checkpoints do. A word on a private page is
 * its process's whatever the flag says, as in Linux. The word is read first,
 * so that its page is in: a frame that is not there has no identity yet. */
static long linux_futex_key(uint64_t addr, uint64_t op, const vibeos_procstate_t **ps, uint64_t *key) {
    int me = ks_current();
    uint32_t word;

    *ps = me >= 0 ? ks_ps(me) : 0;
    *key = addr;
    if (me < 0 || (op & LINUX_FUTEX_PRIVATE_FLAG)) {
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

/* Wait until woken, a signal, or - with a timeout - until `deadline` ticks,
 * 0 for none. */
static long linux_futex_wait(const vibeos_procstate_t *kps, uint64_t key, uint64_t addr,
                             uint32_t expected, uint64_t deadline) {
    uint32_t slot;
    uint32_t cur = 0;
    int me = ks_current();

    if (me < 0 || addr == 0u) {
        return -VIBEOS_EINVAL;   /* the address is the row's: IN_IFM_ERR, EINVAL */
    }

    ks_lock(&g_futex_lock, __func__);
    /* The compare and the enqueue are one step. Reading the word first and
     * enqueuing after would leave a window in which a waker sees no waiter and
     * the waiter then sleeps on a value that has already changed - the lost
     * wakeup, which presents as a program that stops for no reason. */
    /* Through the fault-tolerant copy: the check above and this read are two
     * instants, and g_futex_lock can spin between them (H-003). */
    if (vibeos_uaccess_copy(&cur, (const void *)(uintptr_t)addr, 4u) != 0) {
        ks_unlock(&g_futex_lock);
        return -VIBEOS_EFAULT;
    }
    if (cur != expected) {
        ks_unlock(&g_futex_lock);
        ks_log(VIBEOS_LOG_DEBUG, 21u, addr, (uint64_t)expected,
               "futex wait: value already moved");
        return -VIBEOS_EAGAIN;
    }
    for (slot = 0; slot < LINUX_FUTEX_WAITERS; slot++) {
        if (!g_futex_waiters[slot].used) {
            break;
        }
        /* Reclaim an entry whose enqueuer's slot has since been reused: the
         * wake's tenancy check will never match it again, so it is only holding
         * a table slot. This is what bounds the table against a thread reaped
         * while blocked, which never runs the cleanup below. */
        if (ks_seq(g_futex_waiters[slot].task) != g_futex_waiters[slot].seq) {
            break;
        }
    }
    if (slot == LINUX_FUTEX_WAITERS) {
        ks_unlock(&g_futex_lock);
        return -VIBEOS_ENOMEM;
    }
    g_futex_waiters[slot].used = 1;
    g_futex_waiters[slot].addr = key;
    g_futex_waiters[slot].ps = kps;
    g_futex_waiters[slot].task = me;
    g_futex_waiters[slot].seq = ks_seq(me);
    g_futex_waiters[slot].woken = 0;

    /* A wait with a timeout stays runnable and looks at the clock each tick,
     * as a pipe's wait does: BLOCKED is left only by a wake or a signal, and
     * nothing would come to say the time was up. */
    if (deadline == 0u) {
        ks_lock(ks_sched_lock(), __func__);
        (void)ks_set_state(me, VIBEOS_TASK_BLOCKED, __func__);
        ks_unlock(ks_sched_lock());
    }
    ks_unlock(&g_futex_lock);
    ks_log(VIBEOS_LOG_DEBUG, 22u, addr,
           (uint64_t)cur |
           ((uint64_t)ks_id(me)->pid << 32),
           "futex wait: sleeping (a1 = value | tid<<32)");

    /* Yield until somebody wakes us. The scheduler runs from the timer, so
     * this is a wait and not a spin: the core is given away on the first
     * interrupt and this task is not runnable again until a wake says so.
     *
     * Or until a signal that must be acted on is pending. The task was made
     * BLOCKED before this loop, so a signal raised after that finds it BLOCKED
     * and makes it runnable, and a signal raised before it is seen by the check
     * on the first pass. Checking before blocking would leave a window in which
     * neither happens. */
    while (!g_futex_waiters[slot].woken) {
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

    {
        int interrupted;

        /* Woken or interrupted is decided under the lock a waker takes. Read
         * outside it, a FUTEX_WAKE landing between the loop and here would count
         * this waiter as woken while it returned EINTR - and the thread that
         * wake was meant for would never be told. A wake that got in first
         * wins: the call returns 0 and the signal is delivered on the way out
         * all the same. */
        ks_lock(&g_futex_lock, __func__);
        interrupted = !g_futex_waiters[slot].woken;
        g_futex_waiters[slot].addr = 0;
        g_futex_waiters[slot].used = 0;   /* released by its owner, and only here */
        ks_unlock(&g_futex_lock);
        if (interrupted) {
            /* The signal may have been raised before this task was BLOCKED, in
             * which case nothing made it runnable again. It is running now;
             * say so, the same transition raising a signal uses. */
            ks_lock(ks_sched_lock(), __func__);
            if (vibeos_task_state((uint32_t)(me)) == VIBEOS_TASK_BLOCKED) {
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
    }
    ks_log(VIBEOS_LOG_DEBUG, 23u, addr, (uint64_t)ks_id(me)->pid,
           "futex wait: woken");
    return 0;
}

static long linux_sys_futex(uint64_t addr, uint64_t op, uint64_t val, uint64_t utimeout) {
    const vibeos_procstate_t *kps;
    uint64_t key, deadline = 0;
    long r;

    switch (op & VIBEOS_FUTEX_OP_BITS) {
        case LINUX_FUTEX_WAKE:
            if ((r = linux_futex_key(addr, op, &kps, &key)) != 0) {
                return r;
            }
            return linux_futex_wake(kps, key, (uint32_t)val);
        case LINUX_FUTEX_WAIT:
            /* A relative timeout, which every C library's timed wait passes
             * and which used to be ignored: a timed wait that nobody ended
             * waited for ever. */
            if (utimeout != 0u) {
                linux_timespec_t ts;
                uint64_t ticks;

                if (!linux_user_ok(utimeout, sizeof(ts), 0) ||
                    vibeos_uaccess_copy(&ts, (const void *)(uintptr_t)utimeout, sizeof(ts)) != 0) {
                    return -VIBEOS_EFAULT;
                }
                /* Through the one conversion that saturates: tv_sec * hz in
                 * 64 bits wrapped for a huge timeout, which then ended almost
                 * at once instead of all but never (external review,
                 * 2026-10-07). */
                {
                    int64_t t = linux_ticks_of(&ts);

                    if (t < 0) {
                        return -VIBEOS_EINVAL;
                    }
                    ticks = (uint64_t)t;
                }
                deadline = ks_ticks() + (ticks ? ticks : 1u);
            }
            if ((r = linux_futex_key(addr, op, &kps, &key)) != 0) {
                return r;
            }
            return linux_futex_wait(kps, key, addr, (uint32_t)val, deadline);
        default:
            /* Loudly, because this is how a thread library silently stops
             * working: it asks for an operation, is told it does not exist,
             * and carries on believing the wakeup it requested happened. */
            ks_log(VIBEOS_LOG_WARN, 25u, addr, op,
                   "futex: unsupported operation");
            return -VIBEOS_ENOSYS;
    }
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_FUTEX_SYSCALLS(X) \
    X(202, futex,            FUTEX,           PTRS(IN_IFM_ERR(1, VIBEOS_FUTEX_OP_BITS, LINUX_FUTEX_WAIT, 0, 4, VIBEOS_EINVAL)), linux_sys_futex(ARG(0), ARG(1), ARG(2), ARG(3)))

LINUX_DEFINE_SYSCALLS(futex, LINUX_FUTEX_SYSCALLS)
