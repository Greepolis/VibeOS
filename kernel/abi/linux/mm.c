/* Linux ABI: brk, mmap, mprotect, munmap and pageinfo.
 *
 * Lifted out of arch_hw.c (C4 stage 3) as they were. Since docs/abi/ L3 mmap
 * places a mapping where the program says and maps a file's contents. */

#include "linux_internal.h"

/* Does any region of the process overlap [base, base + len)? The first one
 * that does, or null. Under the process's mm lock. */
static vibeos_vma_t *linux_region_in(vibeos_procstate_t *ps, uint64_t base, uint64_t len) {
    vibeos_vma_t *v;

    for (v = ps->vmas.head; v; v = v->next) {
        if (v->base < base + len && base < v->base + v->len) {
            return v;
        }
    }
    return 0;
}

/* Address space for `pages` that nothing occupies, or 0. Under the mm lock.
 *
 * The arena is a cursor that only moves up, which was the whole of it while
 * every mapping came from here. Since docs/abi/ L3 a program can put a mapping
 * at an address it names, and that address can be ahead of the cursor - so the
 * cursor steps over whatever is already there rather than assuming nothing is.
 * A range whose mapping later fails stays a hole: address space, not memory. */
static uint64_t linux_mmap_place(vibeos_procstate_t *ps, uint64_t pages) {
    uint64_t base = __atomic_load_n(&ps->mmap_cur, __ATOMIC_ACQUIRE);
    const uint64_t bytes = pages * 4096ull;
    vibeos_vma_t *v;

    for (;;) {
        if (base + bytes < base || !ks_user_fixed_ok(base, bytes)) {
            return 0u;
        }
        v = linux_region_in(ps, base, bytes);
        if (!v) {
            break;
        }
        base = v->base + v->len;
    }
    __atomic_store_n(&ps->mmap_cur, base + bytes, __ATOMIC_RELEASE);
    return base;
}

/* brk(0) reports the break; brk(addr) moves it, mapping fresh pages or giving
 * them back. Runs with the process's mm_busy claimed; see linux_sys_brk. */
static long linux_sys_brk_locked(int me, vibeos_procstate_t *ps, uint64_t addr) {
    uint64_t new_brk, pages;

    if (addr == 0u) {
        return (long)ps->brk_cur;
    }
    if (addr < ks_heap_base() || addr >= ks_mmap_base()) {
        return (long)ps->brk_cur; /* out of the heap arena: unchanged */
    }
    new_brk = (addr + 0xFFFull) & ~0xFFFull;
    if (new_brk > ps->brk_cur) {
        pages = (new_brk - ps->brk_cur) / 4096ull;
        if (ks_map_user_pages(me, ps->brk_cur, pages) != 0) {
            return -VIBEOS_ENOMEM;
        }
        (void)vibeos_vma_insert(&ps->vmas, ps->brk_cur,
                                new_brk - ps->brk_cur,
                                (vibeos_prot_t)(VIBEOS_PROT_READ |
                                                VIBEOS_PROT_WRITE |
                                                VIBEOS_PROT_USER),
                                VIBEOS_BACKING_ANON, 0, 0);
    } else if (new_brk < ps->brk_cur) {
        /* Shrinking used to remove the region and stop there.
         *
         * The page-table entries stayed present and the frames stayed
         * allocated, so memory the program had handed back to the kernel was
         * still mapped and still readable through the pages it had just
         * released. Found by an external review; the region list and the page
         * tables are the two sources of truth here and this left them
         * disagreeing, which is the same shape as the defect that let munmap
         * free frames belonging to somebody else.
         *
         * Removed from the list first, then unmapped - a region must never be
         * described after it has stopped existing, the same publish-last rule
         * munmap follows a few lines down. */
        uint64_t va;
        vibeos_vmspace_t v = ks_vm(me);

        (void)vibeos_vma_remove(&ps->vmas, new_brk, ps->brk_cur - new_brk);
        for (va = new_brk; va < ps->brk_cur; va += 4096ull) {
            (void)vibeos_vmspace_unmap(&v, va);
        }
        /* And the frames go through the same quarantine munmap uses: the
         * unmap path releases through vb.release_deferred, so a sibling
         * thread holding a stale translation cannot be handed the frame
         * underneath it. Draining here for the same reason munmap does. */
        ks_tlb_drain();
    }
    ps->brk_cur = new_brk;
    return (long)ps->brk_cur;
}

static long linux_sys_brk(uint64_t addr) {
    vibeos_procstate_t *ps;
    long r;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user ||
        (ps = ks_ps(ks_current())) == 0) {
        return -VIBEOS_EINVAL;
    }
    /* One at a time per process, and now against fork too: two threads moving
     * the break at once, or a fork reading the tables while one moves it, are
     * the same hazard. See ks_mm_lock. */
    ks_mm_lock(ps);
    r = linux_sys_brk_locked(ks_current(), ps, addr);
    ks_mm_unlock(ps);
    return r;
}

/* The syscall's protection bits as this kernel's own type. One place, because
 * mmap and mprotect must agree about what a region's protection means or the
 * list and the tables drift apart - which is the drift this layer exists to
 * end. */
static vibeos_prot_t linux_prot_of(uint64_t prot) {
    vibeos_prot_t p = VIBEOS_PROT_NONE;

    if (prot == LINUX_PROT_NONE) {
        return p;   /* a guard: mapped, owned, and reachable by nobody */
    }
    p = (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_USER);
    if (prot & LINUX_PROT_WRITE) {
        p = (vibeos_prot_t)(p | VIBEOS_PROT_WRITE);
    }
    if (prot & LINUX_PROT_EXEC) {
        p = (vibeos_prot_t)(p | VIBEOS_PROT_EXEC);
    }
    return p;
}

/* Take [addr, end) out of the caller's address space: the regions first, so
 * nothing is described after it has stopped existing, then the pages. munmap's
 * body, and what a mapping at a fixed address does to whatever was there. Under
 * the mm lock. */
static void linux_unmap_range(int me, vibeos_procstate_t *ps, uint64_t addr, uint64_t end) {
    vibeos_vmspace_t v = ks_vm(me);
    uint64_t va;

    (void)vibeos_vma_remove(&ps->vmas, addr, end - addr);
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
}

/* mmap(addr, len, prot, flags, fd, offset).
 *
 * **Where.** At the address given when the program insists (MAP_FIXED), taking
 * the place of whatever was mapped there - which is how a dynamic loader lays a
 * library's segments over the range it reserved for it - or refusing if anything
 * was (MAP_FIXED_NOREPLACE, EEXIST). Otherwise at the address hinted when that
 * range is free, and failing that wherever the arena has room. A fixed address
 * the architecture does not allow a mapping at is ENOMEM, as an address past
 * the end of user memory is on Linux.
 *
 * **What.** Zeroed pages, or the bytes of a file from `offset`: a private
 * mapping of a file is the file's contents in pages of the process's own, read
 * when the mapping is made. That is what MAP_PRIVATE promises - writes stay in
 * the process and never reach the file - and what it leaves open (whether a
 * later change to the file shows through) is answered "no". The pages past the
 * end of the file read as zeros, where Linux raises SIGBUS beyond the last
 * page that holds any of it.
 *
 * A shared mapping of a file is still refused (step 2), and a shared anonymous
 * one is made but is private after a fork - the gap the registry names.
 *
 * PROT_NONE is a mapping with no access, which is how a thread stack is made: a
 * C library asks for stack plus guard as one PROT_NONE region and then
 * mprotects the usable part readable and writable, so a thread that overruns its
 * stack lands on the guard instead of on another thread's memory. Refusing this
 * - which this did, calling it "nothing sensible to map" - is why pthread_create
 * failed before it ever reached clone().
 *
 * The guard pages are allocated, not merely promised. Handing back address
 * space and populating it later in mprotect was the first attempt, and it could
 * not tell a reservation from a range that munmap had just freed: both are
 * "unmapped inside the arena", so mprotect started accepting an address the ABI
 * self-test requires it to refuse. One bit in the page table answers the
 * question that two ranges could not, at the cost of a frame per guard page -
 * ks_map_anon maps VIBEOS_PROT_NONE with no user access, so ring 3 faults on it
 * exactly as a guard should. Any other protection is executable only when asked
 * for (M-036).
 *
 * Runs under the process's mm lock, so a fork cloning the tables and the region
 * list, or a sibling's brk or munmap, cannot see it half-built. */
static long linux_mmap_locked(int me, vibeos_procstate_t *ps, uint64_t addr, uint64_t len,
                              uint64_t prot, uint64_t flags, vibeos_file_t *f, uint64_t off) {
    const vibeos_prot_t want = linux_prot_of(prot);
    const vibeos_prot_t fill = (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_WRITE | VIBEOS_PROT_USER);
    const uint64_t pages = (len + 0xFFFull) / 4096ull, bytes = pages * 4096ull;
    vibeos_vmspace_t v = ks_vm(me);
    uint64_t base, i;
    long r = 0;

    if (flags & (LINUX_MAP_FIXED | LINUX_MAP_FIXED_NOREPLACE)) {
        if ((addr & 0xFFFull) != 0u) {
            return -VIBEOS_EINVAL;
        }
        if (!ks_user_fixed_ok(addr, bytes)) {
            return -VIBEOS_ENOMEM;
        }
        if (flags & LINUX_MAP_FIXED_NOREPLACE) {
            if (linux_region_in(ps, addr, bytes)) {
                return -VIBEOS_EEXIST;
            }
        } else {
            linux_unmap_range(me, ps, addr, addr + bytes);
        }
        base = addr;
    } else {
        base = addr & ~0xFFFull;
        if (base == 0u || !ks_user_fixed_ok(base, bytes) || linux_region_in(ps, base, bytes)) {
            base = linux_mmap_place(ps, pages);   /* a hint is a hint */
        }
        if (base == 0u) {
            return -VIBEOS_ENOMEM;
        }
    }
    for (i = 0; i < pages && r == 0; i++) {
        /* A file's page is mapped writable to be filled, whatever was asked
         * for, and given its real protection after. */
        if (ks_map_anon(me, base + i * 4096ull, f ? fill : want) != 0) {
            r = -VIBEOS_ENOMEM;
            break;
        }
    }
    for (i = 0; f && i < pages && r == 0; i++) {
        /* Into the mapping itself: the file's read copies to a user address,
         * and this is one now. A short read is the end of the file, and the
         * rest stays zeros. */
        long n = f->ops->pread(f, base + i * 4096ull, 4096u, off + i * 4096ull);
        if (n < 0) {
            r = n;
        } else if (n < 4096) {
            break;
        }
    }
    if (r != 0) {
        /* Roll back what was mapped. Two leaks lived here, and in the
         * ordinary-malloc path there was no rollback at all (M-002's fix went
         * into the reservation loop and missed the other one): the pages
         * already mapped stayed mapped with no region describing them, and the
         * address space is claimed before anything is mapped, so nothing would
         * ever map over them again. ks_map_anon gives back the page it could
         * not map itself. */
        for (i = 0; i < pages; i++) {
            (void)vibeos_vmspace_unmap(&v, base + i * 4096ull);
        }
        ks_tlb_drain();
        return r;
    }
    (void)vibeos_vma_insert(&ps->vmas, base, bytes, want, VIBEOS_BACKING_ANON, 0, 0);
    if (f && want != fill) {
        for (i = 0; i < pages; i++) {
            (void)vibeos_vmspace_protect(&v, base + i * 4096ull, want);
            ks_tlb_flush_page(base + i * 4096ull);
        }
    }
    return (long)base;
}

static long linux_sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd,
                           uint64_t off) {
    vibeos_procstate_t *ps;
    vibeos_file_t *f = 0;
    long r;

    ks_log(VIBEOS_LOG_DEBUG, 12u, len, prot | (flags << 32), "mmap");
    if (ks_current() < 0 || !ks_id(ks_current())->is_user || len == 0u) {
        return -VIBEOS_EINVAL;
    }
    /* The page count is (len + 0xFFF) / 4096, and for a length within a page
     * of 2^64 that sum wraps to a page count of zero: nothing was claimed,
     * nothing mapped, and the call returned success with the base the next
     * caller would also get (M-006, verified). Refused before the sum, with
     * the answer Linux gives when the aligned length is zero. */
    if (len > ~0ull - 0xFFFull) {
        return -VIBEOS_ENOMEM;
    }
    /* Private or shared, and one of them: Linux refuses a mapping that says
     * neither, because the two mean different things for every later write. */
    if ((flags & LINUX_MAP_TYPE) != LINUX_MAP_PRIVATE && (flags & LINUX_MAP_TYPE) != LINUX_MAP_SHARED &&
        (flags & LINUX_MAP_TYPE) != LINUX_MAP_SHARED_VALIDATE) {
        return -VIBEOS_EINVAL;
    }
    ps = ks_ps(ks_current());
    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    if (!(flags & LINUX_MAP_ANONYMOUS)) {
        /* The file, held for as long as it is being read from. The mapping
         * does not keep it: the bytes are the process's own once copied, which
         * is why closing the descriptor afterwards changes nothing. */
        if ((off & 0xFFFull) != 0u || (int64_t)off < 0) {
            return -VIBEOS_EINVAL;
        }
        if (!(f = linux_file_get(fd))) {
            return -VIBEOS_EBADF;
        }
        if (f->ops != &vibeos_fops_regular) {
            /* A directory, a pipe, a socket, the console: nothing to map. */
            vibeos_file_put(f);
            return -VIBEOS_ENODEV;
        }
        if ((f->flags & VIBEOS_O_ACCMODE) == VIBEOS_O_WRONLY) {
            vibeos_file_put(f);
            return -VIBEOS_EACCES;
        }
        if ((flags & LINUX_MAP_TYPE) != LINUX_MAP_PRIVATE) {
            /* Shared with the file: a store has to reach it, and every other
             * mapping of it. Step 2. Said rather than answered with a private
             * copy, which would look like a file nobody else's writes reach. */
            vibeos_file_put(f);
            ks_log(VIBEOS_LOG_WARN, 11u, flags, fd, "mmap refused: shared file mapping");
            return -VIBEOS_ENOSYS;
        }
    }
    ks_mm_lock(ps);
    r = linux_mmap_locked(ks_current(), ps, addr, len, prot, flags, f, off);
    ks_mm_unlock(ps);
    if (f) {
        vibeos_file_put(f);
    }
    return r;
}

/* mprotect(): change permissions on pages that are already mapped.
 *
 * Applied to the real page-table entries rather than recorded and ignored. A
 * libc uses this for RELRO - it maps its relocated data writable, then takes
 * write away - and a kernel that returns success without revoking anything
 * leaves the program less protected than it believes itself to be. */
static long linux_sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    ks_log(VIBEOS_LOG_DEBUG, 14u, addr, len, "mprotect");
    uint64_t va, end;
    int me = ks_current();

    if (me < 0 || !ks_id(me)->is_user) {
        return -VIBEOS_EINVAL;
    }
    /* `addr + len < addr` was the whole check, and it is one page short: the
     * aligned end below adds 0xFFF more, so a range ending in the last page of
     * the address space wrapped to end = 0. mprotect then did nothing and
     * reported success; munmap handed the region list 2^64 - addr as a length
     * and dropped every region above addr with the pages still mapped (found
     * beside M-006). The bound covers both sums. */
    if ((addr & 0xFFFull) != 0u || len == 0u || len > ~0ull - 0xFFFull - addr) {
        return -VIBEOS_EINVAL;
    }
    end = (addr + len + 0xFFFull) & ~0xFFFull;

    /* The whole call runs under the address-space lock: the region list and
     * the page tables, which is what a fork's clone_cow reads. It used to take
     * the lock for the list only and narrow the page tables after releasing
     * it, because the narrowing does a TLB shootdown and a sibling spinning in
     * the mm lock could not acknowledge the IPI - the shootdown's timeout panic.
     * That is no longer true: the spin opens an interrupt window every turn,
     * and a waiting core answers flush requests itself (M-065). So a fork can
     * no longer read a page-table entry this call is halfway through
     * narrowing. Single exit below, so one release. */
    ks_mm_lock(ks_ps(me));
    {
        vibeos_vmspace_t v = ks_vm(me);
        long rc = 0;

        /* Check the whole range first: a partial application would leave the
         * address space in a state the caller never asked for.
         *
         * "Mapped" is the question, not "mapped and reachable from ring 3": a
         * PROT_NONE region is mapped with no user access, and mprotect turning
         * that into a usable stack is the entire point of the pattern. A page
         * in swap is mapped too - its permissions travel with it (M-063) - and
         * this refused it with EFAULT for as long as the check asked whether
         * the entry was present. A page munmap has freed is not mapped at all,
         * and stays a fault - which is what the ABI self-test checks. */
        for (va = addr; va < end; va += 4096ull) {
            if (!vibeos_vmspace_mapped(&v, va)) {
                ks_log(VIBEOS_LOG_WARN, 13u, va, len,
                       "mprotect refused: page not mapped");
                rc = -VIBEOS_EFAULT;
                break;
            }
        }

        /* The list is the authority on whether the range is mapped, and it
         * refuses the whole request rather than applying part of it. The
         * page-table pass below then carries the decision out. A PROT_NONE
         * region is a region like any other here - that is the point of
         * describing what was asked for. */
        if (rc == 0 &&
            vibeos_vma_protect(&ks_ps(me)->vmas, addr, end - addr,
                               linux_prot_of(prot)) != 0) {
            ks_log(VIBEOS_LOG_WARN, 15u, addr, len,
                   "mprotect refused: the range is not one this process asked for");
            rc = -VIBEOS_EFAULT;
        }

        /* Through L1, which does the compare-exchange, preserves everything a
         * permission change does not alter (the frame, the ownership mark, the
         * copy-on-write mark), keeps a copy-on-write page read-only until its
         * fault makes the copy, updates a swapped entry's saved permissions,
         * and shoots down the other cores when it narrows. */
        if (rc == 0) {
            vibeos_prot_t p = linux_prot_of(prot);   /* the list's own reading */
            for (va = addr; va < end; va += 4096ull) {
                (void)vibeos_vmspace_protect(&v, va, p);
                ks_tlb_flush_page(va);
            }
        }
        ks_mm_unlock(ks_ps(me));
        return rc;
    }
}

/* munmap(): remove mappings and give the frames back.
 *
 * The arena is a bump allocator, so the address space is not reclaimed for
 * reuse - but the pages are unmapped for real, so a use-after-unmap faults
 * here exactly as it would on Linux instead of quietly still working. */
static long linux_sys_munmap(uint64_t addr, uint64_t len) {
    uint64_t end;
    int me = ks_current();

    if (me < 0 || !ks_id(me)->is_user) {
        return -VIBEOS_EINVAL;
    }
    /* `addr + len < addr` was the whole check, and it is one page short: the
     * aligned end below adds 0xFFF more, so a range ending in the last page of
     * the address space wrapped to end = 0. mprotect then did nothing and
     * reported success; munmap handed the region list 2^64 - addr as a length
     * and dropped every region above addr with the pages still mapped (found
     * beside M-006). The bound covers both sums. */
    if ((addr & 0xFFFull) != 0u || len == 0u || len > ~0ull - 0xFFFull - addr) {
        return -VIBEOS_EINVAL;
    }
    end = (addr + len + 0xFFFull) & ~0xFFFull;
    /* One mutation of this process's address space at a time: a fork cloning
     * the tables and the region list, or a sibling's brk, must not see this
     * range half-removed. See ks_mm_lock.
     *
     * The list decides what this range contains; the page tables are then made
     * to agree. That order is the phase: the tables record what the hardware
     * currently does, and asking *them* what to release is what let munmap free
     * frames that belonged to somebody else. */
    ks_mm_lock(ks_ps(me));
    linux_unmap_range(me, ks_ps(me), addr, end);
    ks_mm_unlock(ks_ps(me));
    return 0;
}

/* Describe the page backing one address of the caller's own address space.
 *
 * Only its own: the walk starts from the current task's page tables and there
 * is no way to name another. A process learning how its own memory is shared is
 * not a disclosure - it is the information it would have had if the kernel had
 * not been the one holding it.
 *
 * What it reports is an *identity* and not a physical address. The frame index
 * is stable within a boot and comparable between samples, which is all a
 * diagnosis needs, and it says nothing about where memory physically lives.
 * Handing ring 3 the layout of the machine to make debugging easier is the kind
 * of trade that outlives the bug it was made for.
 *
 * It exists because one symptom - "the bytes are not what I wrote" - has three
 * causes that a program cannot tell apart: the copy never happened, the copy
 * was made and then lost, or the page is private and somebody wrote it anyway.
 * Every report of the third kind has been investigated as if it might be the
 * first. */
static long linux_sys_pageinfo(uint64_t va, uint64_t out_uptr) {
    vibeos_pageinfo_t info;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }

    ks_pageinfo(ks_current(), va, &info);

    /* Through the fault-tolerant copy: the range was checked above, but a
     * sibling's munmap of the output buffer can land after the check. */
    if (vibeos_uaccess_copy((void *)(uintptr_t)out_uptr, &info, sizeof(info)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_MM_SYSCALLS(X) \
    X(9,    mmap,     MAP,      NOPTR, linux_sys_mmap(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4), ARG(5))) \
    X(10,   mprotect, PROTECT,  NOPTR, linux_sys_mprotect(ARG(0), ARG(1), ARG(2))) \
    X(11,   munmap,   UNMAP,    NOPTR, linux_sys_munmap(ARG(0), ARG(1))) \
    X(12,   brk,      BRK,      NOPTR, linux_sys_brk(ARG(0))) \
    X(1001, pageinfo, PAGEINFO, PTRS(OUT(1, sizeof(vibeos_pageinfo_t))), linux_sys_pageinfo(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(mm, LINUX_MM_SYSCALLS)
