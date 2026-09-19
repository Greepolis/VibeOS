/* Linux ABI: what several of the handlers share.
 *
 * Lifted out of arch_hw.c (C4 stage 3). Nothing here is new: the handlers and the
 * helpers only they use, moved as they were. */

#include "linux_internal.h"

/* Which image the staging buffer currently holds.
 *
 * A shell runs the same binary over and over - every external command in a
 * BusyBox system is the same two megabytes - and re-reading it from the
 * filesystem each time is the single most expensive thing an exec does. The
 * buffer is already there and already holds exactly those bytes, so the read
 * can be skipped when the path has not changed.
 *
 * Correctness rests on the whole thing living under g_exec_lock, and on any
 * write to the volume dropping the cache: a program that has been rewritten
 * must not keep running as its old self. */
char g_exec_cached[128];

long g_exec_cached_len;

/* Which cache identity the bytes in the staging buffer came from, or 0 if they
 * did not come from the cache. Kept beside the path and the length because it
 * describes the same thing they do: what is currently staged. On a staging hit
 * the bytes are the previous read of this same path, so this stays valid. */
uint32_t g_exec_cached_id;

void hw_exec_cache_drop(void) {
    g_exec_cached[0] = 0;
    g_exec_cached_len = 0;
    g_exec_cached_id = 0;
}

/* One address-space mutation at a time per process. A compare-exchange flag,
 * not a spinlock: the work under it can be many pages, and a spinlock would
 * hold them all with the timer off. Every thread of a process shares the ps,
 * so this serialises brk against fork's read of the same tables and list -
 * and mmap, munmap and mprotect as each is converted to take it.
 *
 * Bounded, because the failure mode of a lock is a hang, and a hang here is a
 * silent machine. A holder that never releases - a return path that forgot to
 * unlock - becomes a named panic instead. The bound is the shootdown's, chosen
 * for the same reason: far beyond any honest wait between two cores. */
void hw_mm_lock(hw_procstate_t *ps) {
    uint64_t rflags, spins = 0;

    /* The caller's interrupt state, restored on acquire. Callers are syscalls,
     * so this is masked - but the spin below must unmask, so it is captured. */
    __asm__ __volatile__("pushfq; popq %0" : "=r"(rflags));
    for (;;) {
        uint32_t zero = 0;
        if (__atomic_compare_exchange_n(&ps->mm_busy, &zero, 1u, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            if (rflags & 0x200ull) {
                __asm__ __volatile__("sti" ::: "memory");
            }
            return;
        }
        /* Open a window for interrupts before trying again, because a core
         * spinning here must be able to acknowledge a TLB shootdown IPI. The
         * lock holder can be inside a shootdown that targets this very core: a
         * fork holds this lock across clone_cow, which revokes the parent's
         * write permission and shoots down every core running the address
         * space, and mmap/munmap/mprotect of a sibling thread contend the same
         * lock on such a core. A syscall runs with interrupts masked, so
         * without this window that core could not answer until it left the
         * spin, and it would not leave until the holder released - the
         * shootdown's timeout panic, a deadlock. The spin holds no lock, so a
         * timer taken here is a safe preemption, and g_current_task is per-CPU
         * so identity survives it. Masked again before the next attempt, so
         * acquisition and the mutation run with interrupts as the caller had
         * them. */
        __asm__ __volatile__("sti; pause; cli" ::: "memory");
        if (++spins > 200000000ull) {
            hw_panic("mm lock held too long: a mutation did not release it");
        }
    }
}

void hw_mm_unlock(hw_procstate_t *ps) {
    __atomic_store_n(&ps->mm_busy, 0u, __ATOMIC_RELEASE);
}
