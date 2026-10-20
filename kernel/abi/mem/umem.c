/* A process's address space as every personality changes it.
 *
 * Lifted out of the Linux personality's mm.c when W3 (docs/abi/windows.md)
 * needed the same questions a second time: NtAllocateVirtualMemory places and
 * NtFreeVirtualMemory releases as mmap and munmap do, against the same region
 * list and the same RLIMIT_AS. What each personality decides - the flags, the
 * granularity, the answers - stays in its own handler; this is what both
 * have to do the same way. Reached through vibeos/ksvc.h like a handler, and
 * held to that by check-abi-layering.py. */

#include "vibeos/umem.h"
#include "vibeos/ksvc.h"
#include "vibeos/vmspace.h"
#include "vibeos/abi_linux.h"   /* the kernel's errno values */

/* Does any region of the process overlap [base, base + len)? The first one
 * that does, or null. Under the process's mm lock. */
vibeos_vma_t *vibeos_umem_region_in(vibeos_procstate_t *ps, uint64_t base, uint64_t len) {
    vibeos_vma_t *v;

    for (v = ps->vmas.head; v; v = v->next) {
        if (v->base < base + len && base < v->base + v->len) {
            return v;
        }
    }
    return 0;
}

/* Does every page of [addr, end) lie in some region? Under the mm lock. */
int vibeos_umem_covered(vibeos_procstate_t *ps, uint64_t addr, uint64_t end) {
    uint64_t va;

    for (va = addr; va < end;) {
        vibeos_vma_t *v = vibeos_vma_find(&ps->vmas, va);
        if (!v) {
            return 0;
        }
        va = v->base + v->len;
    }
    return 1;
}

/* RLIMIT_AS: may `more` bytes be added to what the process's regions describe?
 * The limit was kept and reported and never asked (external review,
 * 2026-10-07). Under the mm lock. */
int vibeos_umem_as_room(const vibeos_procstate_t *ps, uint64_t more) {
    const vibeos_vma_t *v;
    uint64_t total = more;

    if (ps->rlim_cur[VIBEOS_RLIM_AS] == VIBEOS_RLIM_INFINITY) {
        return 1;
    }
    for (v = ps->vmas.head; v; v = v->next) {
        total += v->len;
    }
    return total <= ps->rlim_cur[VIBEOS_RLIM_AS];
}

/* Address space for `bytes` that nothing occupies, at a multiple of `align`,
 * or 0. Under the mm lock.
 *
 * The arena is a cursor that only moves up, which was the whole of it while
 * every mapping came from here. Since docs/abi/ L3 a program can put a mapping
 * at an address it names, and that address can be ahead of the cursor - so the
 * cursor steps over whatever is already there rather than assuming nothing is.
 * A range whose mapping later fails stays a hole: address space, not memory. */
uint64_t vibeos_umem_place(vibeos_procstate_t *ps, uint64_t bytes, uint64_t align) {
    const uint64_t mask = align - 1u;
    uint64_t base = __atomic_load_n(&ps->mmap_cur, __ATOMIC_ACQUIRE);
    vibeos_vma_t *v;

    for (;;) {
        /* Rounding up past the top is the same "no room" as a sum that wraps. */
        if (base + mask < base) {
            return 0u;
        }
        base = (base + mask) & ~mask;
        if (base + bytes < base || !ks_user_fixed_ok(base, bytes)) {
            return 0u;
        }
        v = vibeos_umem_region_in(ps, base, bytes);
        if (!v) {
            break;
        }
        base = v->base + v->len;
    }
    __atomic_store_n(&ps->mmap_cur, base + bytes, __ATOMIC_RELEASE);
    return base;
}

/* Take [addr, end) out of task `me`'s address space: the regions first, so
 * nothing is described after it has stopped existing, then the pages. munmap's
 * body, what a mapping at a fixed address does to whatever was there, and
 * NtFreeVirtualMemory's release. Under
 * the mm lock. */
long vibeos_umem_unmap(int me, vibeos_procstate_t *ps, uint64_t addr, uint64_t end) {
    vibeos_vmspace_t v = ks_vm(me);
    uint64_t va;

    /* The middle of a region needs a descriptor to split it with, and the
     * pool can be empty: then nothing is unmapped and the caller says ENOMEM,
     * as Linux does past its map count. Unmapping anyway left the pages gone
     * and a region still describing them. */
    if (vibeos_vma_remove(&ps->vmas, addr, end - addr) == VIBEOS_VMA_REFUSED) {
        return -VIBEOS_ENOMEM;
    }
    for (va = addr; va < end; va += 4096ull) {
        /* One call, and it is the last page-table write that lived outside
         * kernel/mm/vmspace.c.
         *
         * The two rules this used to get wrong are now properties of the
         * layer rather than of this loop. It freed the frame outright, which
         * is correct only for a page nobody else has - and after a fork that
         * is the rare case, so unmapping a copy-on-write page put it back on
         * the free list while another process was still running from it. And
         * it decided ownership by asking whether the page was present and
         * user-reachable, the same inference that freed the kernel's identity
         * map from teardown; munmap had simply never been pointed at a
         * low-window address by anything that mattered.
         *
         * That premature free was chased three times from the far end and
         * presented as something different each time: a musl program tripping
         * over its own malloc bins, init printing a pointer where a pid
         * belonged, a forked child reading back something it had not
         * written. */
        if (vibeos_vmspace_unmap(&v, va) == 1) {
            ks_log(VIBEOS_LOG_DEBUG, 45u, va, 0, "munmap released a page (a0 = address)");
        }
    }
    /* A shootdown still does not belong here, and the gap it left is closed a
     * different way.
     *
     * The need was real and this comment used to end by admitting it was
     * unmet: a thread of this process on another core still holds the old
     * translation for an address whose frame has just been handed back, so it
     * can write into memory that now belongs to somebody else. An external
     * review found the admission and was right to call it a defect rather than
     * a note.
     *
     * A synchronous barrier is still the wrong answer, for the measured reason:
     * it was tried, and two runs in twenty-four failed with `tlb_acks below
     * shootdowns`. `syscall` clears IF, so a target cannot take the IPI until
     * it returns to ring 3, and munmap runs far more often than fork.
     *
     * What changed is that asking is not the only option. The frame is parked
     * instead - vb.release_deferred - and released once every other core has
     * loaded CR3, which flushes its whole TLB. Nobody waits for anybody; the
     * timer makes every core quiescent on its own. The same shape as the dead
     * kernel stack a core parks until it is provably running on another.
     *
     * Drained here as well as on the timer so a machine doing nothing but
     * unmapping still gives frames back. */
    ks_tlb_drain();
    return 0;
}

/* The page-table half of a protection change, after the region list has made
 * it: through L1, which does the compare-exchange, preserves everything a
 * permission change does not alter (the frame, the ownership mark, the
 * copy-on-write mark), keeps a copy-on-write page read-only until its fault
 * makes the copy, updates a swapped entry's saved permissions, and shoots down
 * the other cores when it narrows. A page nobody has touched yet has no entry,
 * and takes the region's protection when the fault makes it. */
void vibeos_umem_protect_pages(int me, uint64_t addr, uint64_t end, vibeos_prot_t prot) {
    vibeos_vmspace_t v = ks_vm(me);
    uint64_t va;

    for (va = addr; va < end; va += 4096ull) {
        (void)vibeos_vmspace_protect(&v, va, prot);
        ks_tlb_flush_page(va);
    }
}
