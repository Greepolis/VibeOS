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

/* Defined with the calls of L3 step 4, further down; mmap and msync, above
 * them, ask the same questions. Declared here because a use above a definition
 * compiles as an implicit declaration and fails three hundred lines later. */
static int linux_regions_cover(vibeos_procstate_t *ps, uint64_t addr, uint64_t end);
static long linux_lock_range(int me, vibeos_procstate_t *ps, uint64_t addr, uint64_t end, int on);

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
    /* RLIMIT_DATA (L2 step 4): the heap may not grow past it. A refused brk
     * answers with the break as it was, which is how Linux says no. */
    if (new_brk > ps->brk_cur && new_brk - ks_heap_base() > ps->rlim_cur[LINUX_RLIMIT_DATA]) {
        return (long)ps->brk_cur;
    }
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
 * **Whose.** A private mapping's pages are the process's own: a fork makes them
 * copy-on-write and each side's stores stay with it. A shared mapping's are
 * not (step 2): a fork hands the child the same frames, writable if the
 * parent's are, and a shared mapping of a file maps the file's own pages - the
 * ones read() and write() use - so a store is in the file as it is made, with
 * nothing to write back. That needs a filesystem that keeps its files in pages;
 * tmpfs does, and one that keeps them on a disk answers ENODEV, the gap the
 * registry names. Only the pages that hold some of the file are mapped: the
 * ones past its end are left out, and touching one kills the program with
 * SIGSEGV where Linux says SIGBUS.
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
    const int shared = (flags & LINUX_MAP_TYPE) != LINUX_MAP_PRIVATE;
    const vibeos_prot_t access = linux_prot_of(prot);
    const vibeos_prot_t want = shared ? (vibeos_prot_t)(access | VIBEOS_PROT_SHARED) : access;
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
    for (i = 0; f && shared && i < pages && r == 0; i++) {
        /* The file's own page, which comes held: the mapping takes a reference
         * of its own and the one it came with is given back. */
        void *page = 0;
        int s = f->ops->share_page ? f->ops->share_page(f, off + i * 4096ull, &page) : -VIBEOS_ENODEV;

        if (s == 1) {
            break;   /* past the file's last page: nothing there to share */
        }
        if (s < 0) {
            r = s;
            break;
        }
        if (ks_map_page(me, base + i * 4096ull, page, want) != 0) {
            r = -VIBEOS_ENOMEM;
        }
        ks_page_unhold(page);
    }
    for (i = 0; !(f && shared) && i < pages && r == 0; i++) {
        /* A file's page is mapped writable to be filled, whatever was asked
         * for, and given its real protection after. */
        if (ks_map_anon(me, base + i * 4096ull, f ? fill : want) != 0) {
            r = -VIBEOS_ENOMEM;
            break;
        }
    }
    for (i = 0; f && !shared && i < pages && r == 0; i++) {
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
    /* The region says what the mapping is, so that what comes after can ask:
     * anonymous memory of the process's own, a file's bytes in pages of its
     * own, or memory it shares - anonymous or a file's. The stack does not
     * grow into anything but the first, madvise discards only the first, and
     * mremap grows only the first. Its protection is the access alone -
     * "shared" is not something mprotect changes. */
    (void)vibeos_vma_insert(&ps->vmas, base, bytes, access,
                            shared ? VIBEOS_BACKING_SHARED
                                   : f ? VIBEOS_BACKING_FILE : VIBEOS_BACKING_ANON,
                            0, 0);
    if (f && !shared && want != fill) {
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
    if (ks_current() < 0 || !ks_id(ks_current())->is_user) {
        return -VIBEOS_EINVAL;
    }
    if (len == 0u) {
        /* Nothing to map is EINVAL - after the descriptor has been looked at:
         * a mapping of a file that is not open is EBADF whatever its length,
         * which is the order Linux answers in (LTP's mmap08 asks exactly
         * this, with -1 and 0). */
        if ((flags & LINUX_MAP_ANONYMOUS) == 0u) {
            if (!(f = linux_file_get(fd))) {
                return -VIBEOS_EBADF;
            }
            vibeos_file_put(f);
        }
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
    /* MAP_SHARED_VALIDATE is MAP_SHARED that refuses a flag it does not know,
     * where the other two ignore one: it exists so a program can ask for
     * something new and be told when the kernel is too old to do it (LTP's
     * mmap20). */
    if ((flags & LINUX_MAP_TYPE) == LINUX_MAP_SHARED_VALIDATE && (flags & ~(uint64_t)LINUX_MAP_KNOWN)) {
        return -VIBEOS_EOPNOTSUPP;
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
        if ((flags & LINUX_MAP_TYPE) != LINUX_MAP_PRIVATE && (prot & LINUX_PROT_WRITE) &&
            (f->flags & VIBEOS_O_ACCMODE) != VIBEOS_O_RDWR) {
            /* Shared and writable is writing the file, and that takes a
             * descriptor that may. A private mapping never needed one: its
             * stores stay in the process. */
            vibeos_file_put(f);
            return -VIBEOS_EACCES;
        }
    }
    ks_mm_lock(ps);
    r = linux_mmap_locked(ks_current(), ps, addr, len, prot, flags, f, off);
    if (r > 0 && (flags & LINUX_MAP_LOCKED)) {
        /* MAP_LOCKED: mlock, said at the time of the mapping. Linux does not
         * fail the mmap when the lock cannot be had, and neither does this. */
        (void)linux_lock_range(ks_current(), ps, (uint64_t)r,
                               (uint64_t)r + ((len + 0xFFFull) & ~0xFFFull), 1);
    }
    ks_mm_unlock(ps);
    if (f) {
        vibeos_file_put(f);
    }
    return r;
}

/* msync(addr, len, flags): what was stored through a shared mapping is in the
 * file.
 *
 * It already is. A shared mapping here maps the file's own pages, so there is
 * no second copy to write back and nothing for MS_INVALIDATE to throw away;
 * what is left of the call is what it refuses - an address that is not a page's
 * (EINVAL), flags it does not know or both of "wait" and "do not wait"
 * (EINVAL), and a range with a hole in it (ENOMEM), which is the answer a
 * program checks to learn whether it still has the mapping. A filesystem that
 * kept a mapped file on a disk would do its writing here; none does yet. */
static long linux_sys_msync(uint64_t addr, uint64_t len, uint64_t flags) {
    vibeos_procstate_t *ps;
    uint64_t end;
    long r = 0;

    if (ks_current() < 0 || !ks_id(ks_current())->is_user || (ps = ks_ps(ks_current())) == 0) {
        return -VIBEOS_EINVAL;
    }
    if ((addr & 0xFFFull) != 0u ||
        (flags & ~(uint64_t)(LINUX_MS_ASYNC | LINUX_MS_INVALIDATE | LINUX_MS_SYNC)) != 0u ||
        ((flags & LINUX_MS_ASYNC) && (flags & LINUX_MS_SYNC))) {
        return -VIBEOS_EINVAL;
    }
    if (len > ~0ull - 0xFFFull - addr) {
        return -VIBEOS_ENOMEM;
    }
    end = (addr + len + 0xFFFull) & ~0xFFFull;
    ks_mm_lock(ps);
    if (!linux_regions_cover(ps, addr, end)) {
        r = -VIBEOS_ENOMEM;
    } else if (flags & LINUX_MS_INVALIDATE) {
        /* "Throw the other mappings' copies away" cannot be done to a page
         * somebody locked in memory: EBUSY, as Linux says (LTP's msync03). */
        vibeos_vmspace_t v = ks_vm(ks_current());
        uint64_t va;

        for (va = addr; va < end; va += 4096ull) {
            const uint64_t *e = vibeos_vmspace_entry(&v, va);
            if (e && (*e & VIBEOS_PTE_LOCKED)) {
                r = -VIBEOS_EBUSY;
                break;
            }
        }
    }
    ks_mm_unlock(ps);
    return r;
}

/* ---- the rest of the memory calls (docs/abi/ L3 step 4) --------------------------- */

/* Is every page of [addr, end) inside some region? Under the mm lock. */
static int linux_regions_cover(vibeos_procstate_t *ps, uint64_t addr, uint64_t end) {
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

/* The caller, as a process with memory; 0 if it is not one. */
static vibeos_procstate_t *linux_mm_caller(int *me) {
    *me = ks_current();
    if (*me < 0 || !ks_id(*me)->is_user) {
        return 0;
    }
    return ks_ps(*me);
}

/* madvise(addr, len, advice): what the program expects of a range.
 *
 * Most advice is about speed, and is taken by doing nothing: every page is
 * already there, nothing is read ahead, and there is no large page to offer.
 * Two are not advice at all. MADV_DONTNEED says "I am finished with what is in
 * these pages", and a program is entitled to read zeros from them afterwards -
 * an allocator hands the range out again believing that. MADV_FREE says the
 * same and lets the kernel choose when; "now" is a permitted choice. Both give
 * a private anonymous page back and put a zeroed one in its place.
 *
 * A shared page is left as it is - its contents are not this process's to
 * discard, and Linux would give the same bytes back on the next touch. So is a
 * private page of a file, and there it is a gap: Linux would show the file's
 * bytes again, where this keeps whatever the program stored (the registry
 * names it). A locked page refuses, as on Linux.
 *
 * Advice that changes what a later call does - MADV_DONTFORK, MADV_WIPEONFORK,
 * the huge-page pair, MADV_REMOVE - is EINVAL: answering 0 would be promising
 * a behaviour this kernel does not have. */
static long linux_sys_madvise(uint64_t addr, uint64_t len, uint64_t advice) {
    vibeos_procstate_t *ps;
    vibeos_vmspace_t v;
    uint64_t va, end;
    long r = 0;
    int me, discard = 0;

    if (!(ps = linux_mm_caller(&me))) {
        return -VIBEOS_EINVAL;
    }
    if ((addr & 0xFFFull) != 0u || len > ~0ull - 0xFFFull - addr) {
        return -VIBEOS_EINVAL;
    }
    switch (advice) {
        case LINUX_MADV_NORMAL: case LINUX_MADV_RANDOM: case LINUX_MADV_SEQUENTIAL:
        case LINUX_MADV_WILLNEED: case LINUX_MADV_DONTDUMP: case LINUX_MADV_DODUMP:
        case LINUX_MADV_COLD: case LINUX_MADV_PAGEOUT:
        case LINUX_MADV_POPULATE_READ: case LINUX_MADV_POPULATE_WRITE:
            break;
        case LINUX_MADV_DONTNEED: case LINUX_MADV_FREE:
            discard = 1;
            break;
        default:
            return -VIBEOS_EINVAL;
    }
    end = (addr + len + 0xFFFull) & ~0xFFFull;
    if (end == addr) {
        return 0;
    }
    ks_mm_lock(ps);
    v = ks_vm(me);
    if (!linux_regions_cover(ps, addr, end)) {
        r = -VIBEOS_ENOMEM;
    }
    for (va = addr; discard && r == 0 && va < end; va += 4096ull) {
        const vibeos_vma_t *reg = vibeos_vma_find(&ps->vmas, va);
        const uint64_t *e = vibeos_vmspace_entry(&v, va);

        /* Every page of the range is in a region - that was asked a few
         * lines up, under the lock still held - so `reg` is not null here.
         * Said to the compiler's reader as well as to this one (code scanning
         * 149, 150): a page with no region is simply passed over. */
        if (!reg) {
            continue;
        }
        if (advice == LINUX_MADV_FREE && reg->backing != VIBEOS_BACKING_ANON) {
            r = -VIBEOS_EINVAL;   /* only memory that is nobody else's and no file's */
        } else if (e && (*e & VIBEOS_PTE_LOCKED)) {
            r = -VIBEOS_EINVAL;   /* locked in memory: not to be thrown away */
        }
    }
    for (va = addr; discard && r == 0 && va < end; va += 4096ull) {
        const vibeos_vma_t *reg = vibeos_vma_find(&ps->vmas, va);

        if (!reg || reg->backing != VIBEOS_BACKING_ANON || !vibeos_vmspace_mapped(&v, va)) {
            continue;
        }
        (void)vibeos_vmspace_unmap(&v, va);
        if (ks_map_anon(me, va, reg->prot) != 0) {
            r = -VIBEOS_ENOMEM;   /* the page is gone and no new one could be had */
        }
    }
    if (discard) {
        ks_tlb_drain();
    }
    ks_mm_unlock(ps);
    return r;
}

/* mincore(addr, len, vec): which pages of a range are in memory, a byte each.
 * 1 for a page that is here, 0 for one in swap or a part of a mapping nothing
 * backs (the end of a shared mapping past its file). ENOMEM for a range with a
 * hole in it. The answer is old by the time it is read, as it is on Linux. */
static long linux_sys_mincore(uint64_t addr, uint64_t len, uint64_t vec_uptr) {
    vibeos_procstate_t *ps;
    vibeos_vmspace_t v;
    uint64_t va, end, pages, done = 0;
    uint8_t out[64];
    long r = 0;
    int me;

    if (!(ps = linux_mm_caller(&me))) {
        return -VIBEOS_EINVAL;
    }
    if ((addr & 0xFFFull) != 0u) {
        return -VIBEOS_EINVAL;
    }
    if (len > ~0ull - 0xFFFull - addr) {
        return -VIBEOS_ENOMEM;
    }
    end = (addr + len + 0xFFFull) & ~0xFFFull;
    pages = (end - addr) / 4096ull;
    /* The vector's length follows from the range's, so no row can declare it:
     * judged here, before anything is stored through it. */
    if (pages != 0u && !linux_user_ok(vec_uptr, pages, 1)) {
        return -VIBEOS_EFAULT;
    }
    ks_mm_lock(ps);
    v = ks_vm(me);
    if (!linux_regions_cover(ps, addr, end)) {
        r = -VIBEOS_ENOMEM;
    }
    for (va = addr; r == 0 && va < end;) {
        uint32_t n = 0;

        for (; n < sizeof(out) && va < end; n++, va += 4096ull) {
            out[n] = vibeos_vmspace_resident(&v, va) == 1 ? 1u : 0u;
        }
        if (vibeos_uaccess_copy((void *)(uintptr_t)(vec_uptr + done), out, n) != 0) {
            r = -VIBEOS_EFAULT;
        }
        done += n;
    }
    ks_mm_unlock(ps);
    return r;
}

/* Lock or unlock [addr, end), whole pages, under the mm lock. */
static long linux_lock_range(int me, vibeos_procstate_t *ps, uint64_t addr, uint64_t end, int on) {
    vibeos_vmspace_t v = ks_vm(me);
    uint64_t va;

    if (!linux_regions_cover(ps, addr, end)) {
        return -VIBEOS_ENOMEM;
    }
    for (va = addr; va < end; va += 4096ull) {
        if (vibeos_vmspace_set_locked(&v, va, on) < 0) {
            return -VIBEOS_EAGAIN;   /* a page in swap that could not be brought back */
        }
    }
    return 0;
}

/* mlock, mlock2, munlock: keep a range in memory, or stop.
 *
 * Kept means reclaim leaves it: the mark is in the page's entry and page-out
 * refuses an entry that has it. A page that is in swap when it is locked is
 * brought back, because the promise is that the memory is here, not that it
 * will stay wherever it is. MLOCK_ONFAULT - lock each page when it is first
 * touched - is the same thing here, where a mapped page already exists.
 *
 * The address need not be a page's: the range is every page any byte of it
 * falls in. A lock is not inherited across fork. */
static long linux_mlock(uint64_t addr, uint64_t len, int on) {
    vibeos_procstate_t *ps;
    uint64_t start, end;
    long r;
    int me;

    if (!(ps = linux_mm_caller(&me))) {
        return -VIBEOS_EINVAL;
    }
    if (len > ~0ull - 0xFFFull - addr) {
        return -VIBEOS_ENOMEM;
    }
    start = addr & ~0xFFFull;
    end = (addr + len + 0xFFFull) & ~0xFFFull;
    if (len == 0u) {
        return 0;
    }
    ks_mm_lock(ps);
    r = linux_lock_range(me, ps, start, end, on);
    ks_mm_unlock(ps);
    return r;
}

static long linux_sys_mlock(uint64_t addr, uint64_t len) { return linux_mlock(addr, len, 1); }
static long linux_sys_munlock(uint64_t addr, uint64_t len) { return linux_mlock(addr, len, 0); }

static long linux_sys_mlock2(uint64_t addr, uint64_t len, uint64_t flags) {
    if (flags & ~(uint64_t)LINUX_MLOCK_ONFAULT) {
        return -VIBEOS_EINVAL;
    }
    return linux_mlock(addr, len, 1);
}

/* mlockall and munlockall: every mapping the process has. MCL_FUTURE - and
 * everything it maps from now on - is accepted and not kept: a mapping made
 * after the call is not locked, which is the gap the registry names. */
static long linux_lock_all(int on) {
    vibeos_procstate_t *ps;
    vibeos_vma_t *reg;
    long r = 0;
    int me;

    if (!(ps = linux_mm_caller(&me))) {
        return -VIBEOS_EINVAL;
    }
    ks_mm_lock(ps);
    for (reg = ps->vmas.head; reg && r == 0; reg = reg->next) {
        r = linux_lock_range(me, ps, reg->base, reg->base + reg->len, on);
    }
    ks_mm_unlock(ps);
    return r;
}

static long linux_sys_mlockall(uint64_t flags) {
    if (flags == 0u || (flags & ~(uint64_t)(LINUX_MCL_CURRENT | LINUX_MCL_FUTURE | LINUX_MCL_ONFAULT)) ||
        flags == LINUX_MCL_ONFAULT) {
        return -VIBEOS_EINVAL;
    }
    return (flags & LINUX_MCL_CURRENT) ? linux_lock_all(1) : 0;
}

static long linux_sys_munlockall(void) { return linux_lock_all(0); }

/* mremap(old, old_len, new_len, flags, new_addr): change a mapping's size, and
 * its address if it has to move.
 *
 * Shrinking gives the tail back. Growing takes the address space after the
 * mapping when that is free; when it is not, and the program allowed it
 * (MREMAP_MAYMOVE), the mapping moves to a range that has room - its pages go
 * with it, the same frames at new addresses, nothing copied - and the old range
 * is left unmapped. MREMAP_FIXED names where it moves to, and takes the place
 * of whatever was there, as MAP_FIXED does.
 *
 * The new pages are zeroed anonymous memory, so only a private anonymous
 * mapping grows: a shared one or a file's would need more of what it shares,
 * and the region does not remember where that came from. Refused with ENOMEM
 * and named in the registry, as are a length of zero (which duplicates a shared
 * mapping) and MREMAP_DONTUNMAP.
 *
 * The range has to lie in one mapping: EFAULT otherwise, as on Linux. */
static long linux_mremap_locked(int me, vibeos_procstate_t *ps, uint64_t old, uint64_t old_len,
                                uint64_t new_len, uint64_t flags, uint64_t new_addr) {
    vibeos_vmspace_t v = ks_vm(me);
    const vibeos_vma_t *reg = vibeos_vma_find(&ps->vmas, old);
    vibeos_backing_kind_t kind;
    vibeos_prot_t prot;
    uint64_t base, i, keep, moved;

    if (!reg || old + old_len > reg->base + reg->len) {
        return -VIBEOS_EFAULT;
    }
    kind = reg->backing;
    prot = reg->prot;
    if (kind == VIBEOS_BACKING_SHARED) {
        prot = (vibeos_prot_t)(prot | VIBEOS_PROT_SHARED);
    }
    if (!(flags & LINUX_MREMAP_FIXED)) {
        if (new_len <= old_len) {
            if (new_len < old_len) {
                linux_unmap_range(me, ps, old + new_len, old + old_len);
            }
            return (long)old;
        }
        if (kind != VIBEOS_BACKING_ANON) {
            return -VIBEOS_ENOMEM;
        }
        /* Room after it: grow where it stands. */
        if (ks_user_fixed_ok(old + old_len, new_len - old_len) &&
            !linux_region_in(ps, old + old_len, new_len - old_len)) {
            for (i = old_len; i < new_len; i += 4096ull) {
                if (ks_map_anon(me, old + i, prot) != 0) {
                    for (; i > old_len; i -= 4096ull) {
                        (void)vibeos_vmspace_unmap(&v, old + i - 4096ull);
                    }
                    ks_tlb_drain();
                    return -VIBEOS_ENOMEM;
                }
            }
            (void)vibeos_vma_insert(&ps->vmas, old + old_len, new_len - old_len, reg->prot, kind, 0, 0);
            return (long)old;
        }
        if (!(flags & LINUX_MREMAP_MAYMOVE)) {
            return -VIBEOS_ENOMEM;
        }
        base = linux_mmap_place(ps, new_len / 4096ull);
        if (base == 0u) {
            return -VIBEOS_ENOMEM;
        }
    } else {
        if ((new_addr & 0xFFFull) != 0u ||
            (new_addr < old + old_len && old < new_addr + new_len)) {
            return -VIBEOS_EINVAL;   /* not a page's address, or over the mapping being moved */
        }
        if (!ks_user_fixed_ok(new_addr, new_len)) {
            return -VIBEOS_ENOMEM;
        }
        if (new_len > old_len && kind != VIBEOS_BACKING_ANON) {
            return -VIBEOS_ENOMEM;
        }
        linux_unmap_range(me, ps, new_addr, new_addr + new_len);
        base = new_addr;
    }
    /* Move what is kept, then make what is new. A failure puts every page
     * moved so far back: the program's mapping is where it was, or where it
     * asked - never in two halves. */
    keep = old_len < new_len ? old_len : new_len;
    for (moved = 0; moved < keep; moved += 4096ull) {
        if (vibeos_vmspace_move(&v, old + moved, base + moved) < 0) {
            break;
        }
    }
    for (i = keep; moved == keep && i < new_len; i += 4096ull) {
        if (ks_map_anon(me, base + i, prot) != 0) {
            break;
        }
    }
    if (moved < keep || i < new_len) {
        for (i = keep; i < new_len; i += 4096ull) {
            (void)vibeos_vmspace_unmap(&v, base + i);
        }
        for (; moved > 0u; moved -= 4096ull) {
            (void)vibeos_vmspace_move(&v, base + moved - 4096ull, old + moved - 4096ull);
        }
        ks_tlb_drain();
        return -VIBEOS_ENOMEM;
    }
    /* The regions follow the pages: the old range stops being described, with
     * whatever of it was not kept unmapped, and the new one is. */
    {
        const vibeos_prot_t region_prot = reg->prot;

        linux_unmap_range(me, ps, old, old + old_len);
        (void)vibeos_vma_insert(&ps->vmas, base, new_len, region_prot, kind, 0, 0);
    }
    return (long)base;
}

static long linux_sys_mremap(uint64_t old, uint64_t old_len, uint64_t new_len, uint64_t flags,
                             uint64_t new_addr) {
    vibeos_procstate_t *ps;
    long r;
    int me;

    if (!(ps = linux_mm_caller(&me))) {
        return -VIBEOS_EINVAL;
    }
    if ((flags & ~(uint64_t)(LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED)) ||
        ((flags & LINUX_MREMAP_FIXED) && !(flags & LINUX_MREMAP_MAYMOVE)) ||
        (old & 0xFFFull) != 0u || new_len == 0u || old_len == 0u ||
        old_len > ~0ull - 0xFFFull - old || new_len > ~0ull - 0xFFFull) {
        return -VIBEOS_EINVAL;
    }
    old_len = (old_len + 0xFFFull) & ~0xFFFull;
    new_len = (new_len + 0xFFFull) & ~0xFFFull;
    ks_mm_lock(ps);
    r = linux_mremap_locked(me, ps, old, old_len, new_len, flags, new_addr);
    ks_mm_unlock(ps);
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
    X(25,   mremap,   MREMAP,   NOPTR, linux_sys_mremap(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(26,   msync,    MSYNC,    NOPTR, linux_sys_msync(ARG(0), ARG(1), ARG(2))) \
    X(27,   mincore,  MINCORE,  NOPTR, linux_sys_mincore(ARG(0), ARG(1), ARG(2))) \
    X(28,   madvise,  MADVISE,  NOPTR, linux_sys_madvise(ARG(0), ARG(1), ARG(2))) \
    X(149,  mlock,    MLOCK,    NOPTR, linux_sys_mlock(ARG(0), ARG(1))) \
    X(150,  munlock,  MUNLOCK,  NOPTR, linux_sys_munlock(ARG(0), ARG(1))) \
    X(151,  mlockall, MLOCKALL, NOPTR, linux_sys_mlockall(ARG(0))) \
    X(152,  munlockall, MUNLOCKALL, NOPTR, linux_sys_munlockall()) \
    X(325,  mlock2,   MLOCK2,   NOPTR, linux_sys_mlock2(ARG(0), ARG(1), ARG(2))) \
    X(1001, pageinfo, PAGEINFO, PTRS(OUT(1, sizeof(vibeos_pageinfo_t))), linux_sys_pageinfo(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(mm, LINUX_MM_SYSCALLS)
