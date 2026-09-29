/* Linux ABI: futex - the table of waiters, waiting and waking.
 *
 * The wait lived with the process syscalls and the table and the wake in the
 * architecture's task code, because exit wakes a joiner. None of it is x86-64:
 * it is a table, a lock, and a task made BLOCKED and READY again. Together here
 * since A2 (docs/abi/), and exit calls linux_futex_wake like any caller. */

#include "linux_internal.h"

/* futex operations we can answer honestly. */
#define FUTEX_WAIT 0

#define FUTEX_WAKE 1

#define FUTEX_CMD_MASK 0x7F

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
 * Waiters are matched on the address alone. Every thread that can share a
 * futex shares an address space, so the same virtual address is the same word;
 * two processes waiting on the same address in their own spaces would be
 * confused with each other, and that is a real limitation, written down rather
 * than papered over. Shared futexes across processes are not implemented.
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
     * ended a wait in another - THREADS_C5_FUTEX_XPROC, red first. There are no
     * shared mappings in this kernel, so the process is the whole key. */
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

static long linux_futex_wait(uint64_t addr, uint32_t expected) {
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
    g_futex_waiters[slot].addr = addr;
    g_futex_waiters[slot].ps = ks_ps(me);
    g_futex_waiters[slot].task = me;
    g_futex_waiters[slot].seq = ks_seq(me);
    g_futex_waiters[slot].woken = 0;

    ks_lock(ks_sched_lock(), __func__);
    (void)ks_set_state(me, VIBEOS_TASK_BLOCKED, __func__);
    ks_unlock(ks_sched_lock());
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
        ks_block_point();
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
            ks_log(VIBEOS_LOG_DEBUG, 23u, addr, (uint64_t)ks_id(me)->pid,
                   "futex wait: interrupted by a signal");
            return -VIBEOS_EINTR;
        }
    }
    ks_log(VIBEOS_LOG_DEBUG, 23u, addr, (uint64_t)ks_id(me)->pid,
           "futex wait: woken");
    return 0;
}

static long linux_sys_futex(uint64_t addr, uint64_t op, uint64_t val) {
    switch (op & FUTEX_CMD_MASK) {
        case FUTEX_WAKE:
            return linux_futex_wake(ks_current() >= 0 ? ks_ps(ks_current()) : 0,
                                 addr, (uint32_t)val);
        case FUTEX_WAIT:
            return linux_futex_wait(addr, (uint32_t)val);
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
    X(202, futex,            FUTEX,           PTRS(IN_IFM_ERR(1, FUTEX_CMD_MASK, FUTEX_WAIT, 0, 4, VIBEOS_EINVAL)), linux_sys_futex(ARG(0), ARG(1), ARG(2)))

LINUX_DEFINE_SYSCALLS(futex, LINUX_FUTEX_SYSCALLS)
