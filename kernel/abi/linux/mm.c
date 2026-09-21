/* Linux ABI: brk, mmap, mprotect, munmap and pageinfo.
 *
 * Lifted out of arch_hw.c (C4 stage 3). Nothing here is new: the handlers and the
 * helpers only they use, moved as they were. */

#include "linux_internal.h"

/* mmap flags and protection bits (asm-generic/mman-common.h). */
#define PROT_NONE  0x0

#define PROT_WRITE 0x2

#define PROT_EXEC  0x4

#define MAP_FIXED     0x10

#define MAP_ANONYMOUS 0x20

/* Claim `pages` of anonymous address space for the calling process, or 0.
 *
 * The cursor is the process's, so two threads can reach it at once. What this
 * replaced was worse than a race: each thread held a private copy, so a
 * thread's mmap never moved main's cursor and main was handed the same base
 * every time. A compare-exchange claims the range before anything is mapped,
 * so no two callers are given the same pages and nobody holds a lock across the
 * mapping. A range whose mapping later fails stays a hole - address space, not
 * memory. */
static uint64_t hw_mmap_claim(hw_procstate_t *ps, uint64_t pages) {
    uint64_t base;

    for (;;) {
        base = __atomic_load_n(&ps->mmap_cur, __ATOMIC_ACQUIRE);
        if (base + pages * 4096ull < base) {
            return 0u;
        }
        if (__atomic_compare_exchange_n(&ps->mmap_cur, &base,
                                        base + pages * 4096ull, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return base;
        }
    }
}

/* brk(0) reports the break; brk(addr) moves it, mapping fresh pages or giving
 * them back. Runs with the process's mm_busy claimed; see hw_sys_brk. */
static long hw_sys_brk_locked(hw_proc_t *proc, hw_procstate_t *ps, uint64_t addr) {
    uint64_t new_brk, pages;

    if (addr == 0u) {
        return (long)ps->brk_cur;
    }
    if (addr < VIBEOS_HW_USER_HEAP_BASE || addr >= VIBEOS_HW_USER_MMAP_BASE) {
        return (long)ps->brk_cur; /* out of the heap arena: unchanged */
    }
    new_brk = (addr + 0xFFFull) & ~0xFFFull;
    if (new_brk > ps->brk_cur) {
        pages = (new_brk - ps->brk_cur) / 4096ull;
        if (hw_map_user_pages(&proc->as, ps->brk_cur, pages) != 0) {
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
        vibeos_vmspace_t v = hw_vm(&proc->as);

        (void)vibeos_vma_remove(&ps->vmas, new_brk, ps->brk_cur - new_brk);
        for (va = new_brk; va < ps->brk_cur; va += 4096ull) {
            (void)vibeos_vmspace_unmap(&v, va);
        }
        /* And the frames go through the same quarantine munmap uses: the
         * unmap path releases through vb.release_deferred, so a sibling
         * thread holding a stale translation cannot be handed the frame
         * underneath it. Draining here for the same reason munmap does. */
        hw_tlbq_drain();
    }
    ps->brk_cur = new_brk;
    return (long)ps->brk_cur;
}

static long hw_sys_brk(uint64_t addr) {
    hw_procstate_t *ps;
    long r;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user ||
        (ps = g_tasks[g_current_task].ps) == 0) {
        return -VIBEOS_EINVAL;
    }
    /* One at a time per process, and now against fork too: two threads moving
     * the break at once, or a fork reading the tables while one moves it, are
     * the same hazard. See hw_mm_lock. */
    hw_mm_lock(ps);
    r = hw_sys_brk_locked(&g_tasks[g_current_task].proc, ps, addr);
    hw_mm_unlock(ps);
    return r;
}

/* Anonymous mmap: bump the per-process arena and map zeroed pages.
 *
 * The address hint is ignored - the arena is bump-allocated, so honouring a
 * fixed address would require a real VMA tree. MAP_FIXED is therefore refused
 * rather than quietly ignored: a program that asks for a specific address and
 * silently gets another one corrupts itself later, far from here. */
/* The syscall's protection bits as this kernel's own type. One place, because
 * mmap and mprotect must agree about what a region's protection means or the
 * list and the tables drift apart - which is the drift this layer exists to
 * end. */
static vibeos_prot_t hw_prot_of(uint64_t prot) {
    vibeos_prot_t p = VIBEOS_PROT_NONE;

    if (prot == PROT_NONE) {
        return p;   /* a guard: mapped, owned, and reachable by nobody */
    }
    p = (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_USER);
    if (prot & PROT_WRITE) {
        p = (vibeos_prot_t)(p | VIBEOS_PROT_WRITE);
    }
    if (prot & PROT_EXEC) {
        p = (vibeos_prot_t)(p | VIBEOS_PROT_EXEC);
    }
    return p;
}

static long hw_mmap_locked(hw_procstate_t *ps, hw_proc_t *proc,
                           uint64_t len, uint64_t prot) {
    uint64_t pages, base, leaf;
    if (prot == PROT_NONE) {
        /* A mapping with no access, which is how a thread stack is made: a C
         * library asks for stack plus guard as one PROT_NONE region and then
         * mprotects the usable part readable and writable, so a thread that
         * overruns its stack lands on the guard instead of on another
         * thread's memory. Refusing this - which this did, calling it
         * "nothing sensible to map" - is why pthread_create failed before it
         * ever reached clone().
         *
         * The pages are allocated, not merely promised. Handing back address
         * space and populating it later in mprotect was the first attempt, and
         * it could not tell a reservation from a range that munmap had just
         * freed: both are "unmapped inside the arena", so mprotect started
         * accepting an address the ABI self-test requires it to refuse. One
         * bit in the page table answers the question that two ranges could
         * not, at the cost of a frame per guard page.
         *
         * No PTE_USER, so ring 3 faults on it exactly as a guard should. */
        uint64_t i;

        pages = (len + 0xFFFull) / 4096ull;
        base = hw_mmap_claim(ps, pages);
        if (base == 0u) {
            return -VIBEOS_ENOMEM;
        }
        for (i = 0; i < pages; i++) {
            void *page = hw_alloc_user_page();

            if (!page || hw_map_page(&proc->as, base + i * 4096ull,
                                     (uint64_t)(uintptr_t)page,
                                     PTE_PRESENT) != 0) {
                /* Two leaks lived here and only one was obvious. `page` is
                 * non-null when hw_map_page is what failed, and nothing gave
                 * it back. The pages already mapped stayed mapped with no
                 * region describing them. They used to be recoverable by
                 * accident, because the cursor was not advanced on this path;
                 * it is claimed before anything is mapped now, so nothing would
                 * ever map over them again and this rollback is the only thing
                 * that leaves no trace. */
                uint64_t j;
                vibeos_vmspace_t v = hw_vm(&proc->as);

                if (page) {
                    hw_page_put((uint64_t)(uintptr_t)page);
                }
                for (j = 0; j < i; j++) {
                    (void)vibeos_vmspace_unmap(&v, base + j * 4096ull);
                }
                hw_tlbq_drain();
                return -VIBEOS_ENOMEM;
            }
            hw_page_put((uint64_t)(uintptr_t)page);   /* D9 */
        }
        (void)vibeos_vma_insert(&ps->vmas, base, pages * 4096ull,
                                hw_prot_of(prot), VIBEOS_BACKING_ANON, 0, 0);
        return (long)base;
    }

    pages = (len + 0xFFFull) / 4096ull;
    base = hw_mmap_claim(ps, pages);
    if (base == 0u) {
        return -VIBEOS_ENOMEM;
    }
    leaf = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) {
        leaf |= PTE_WRITE;
    }
    if (!(prot & PROT_EXEC)) {
        leaf |= PTE_NX;   /* executable only when asked for (M-036) */
    }
    {
        uint64_t i;
        for (i = 0; i < pages; i++) {
            void *page = hw_alloc_user_page();
            if (!page || hw_map_page(&proc->as, base + i * 4096ull,
                                     (uint64_t)(uintptr_t)page, leaf) != 0) {
                /* This loop - the one an ordinary malloc reaches - had no
                 * rollback at all. The fix for the review's M-002 went into the
                 * reservation loop above and missed this one, and the note that
                 * the leftovers could not be reached from ring 3 was true only
                 * there: these pages are PTE_USER. With the cursor now claimed
                 * before mapping they would never be mapped over again either,
                 * so without this they are a leak. */
                uint64_t j;
                vibeos_vmspace_t v = hw_vm(&proc->as);

                if (page) {
                    hw_page_put((uint64_t)(uintptr_t)page);
                }
                for (j = 0; j < i; j++) {
                    (void)vibeos_vmspace_unmap(&v, base + j * 4096ull);
                }
                hw_tlbq_drain();
                return -VIBEOS_ENOMEM;
            }
            hw_page_put((uint64_t)(uintptr_t)page);   /* D9 */
        }
    }
    (void)vibeos_vma_insert(&ps->vmas, base, pages * 4096ull,
                            hw_prot_of(prot), VIBEOS_BACKING_ANON, 0, 0);
    return (long)base;
}

static long hw_sys_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd) {
    hw_proc_t *proc;
    hw_procstate_t *ps;

    hw_log(VIBEOS_LOG_DEBUG, 12u, len, prot | (flags << 32), "mmap");
    if (g_current_task < 0 || !g_tasks[g_current_task].is_user || len == 0u) {
        return -VIBEOS_EINVAL;
    }
    /* Both branches below round with (len + 0xFFF) / 4096, and for a length
     * within a page of 2^64 that sum wraps to a page count of zero: nothing was
     * claimed, nothing mapped, and the call returned success with the base the
     * next caller would also get (M-006, verified). Refused before the sum, with
     * the answer Linux gives when the aligned length is zero. */
    if (len > ~0ull - 0xFFFull) {
        return -VIBEOS_ENOMEM;
    }
    /* Established here, before anything reads it. The reservation branch below
     * used it one statement too early and handed back a base of zero, which a
     * C library then mprotected at address 0x2000 - a thread stack placed on
     * top of nothing. */
    proc = &g_tasks[g_current_task].proc;
    ps = g_tasks[g_current_task].ps;
    if (!ps) {
        return -VIBEOS_EINVAL;
    }
    if (flags & MAP_FIXED) {
        hw_log(VIBEOS_LOG_WARN, 10u, addr, flags, "mmap refused: MAP_FIXED");
        return -VIBEOS_EINVAL;
    }
    /* File-backed mappings need a page cache this kernel does not have. Say so
     * instead of returning anonymous zeroes, which would look like a file full
     * of NULs. */
    if ((flags & MAP_ANONYMOUS) == 0 || VIBEOS_ARG_INT(fd) >= 0) {
        hw_log(VIBEOS_LOG_WARN, 11u, flags, fd,
               "mmap refused: file-backed mapping");
        return -VIBEOS_ENOSYS;
    }
    /* The mapping runs under the process address-space lock, so a fork
     * cloning the tables and region list, or a sibling brk/munmap, cannot
     * see it half-built. mmap does no shootdown, so holding the lock is
     * safe here (mprotect, which does, is the exception). */
    hw_mm_lock(ps);
    {
        long r = hw_mmap_locked(ps, proc, len, prot);
        hw_mm_unlock(ps);
        return r;
    }
}

/* mprotect(): change permissions on pages that are already mapped.
 *
 * Applied to the real page-table entries rather than recorded and ignored. A
 * libc uses this for RELRO - it maps its relocated data writable, then takes
 * write away - and a kernel that returns success without revoking anything
 * leaves the program less protected than it believes itself to be. */
static long hw_sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    hw_log(VIBEOS_LOG_DEBUG, 14u, addr, len, "mprotect");
    hw_proc_t *proc;
    uint64_t va, end;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
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
    proc = &g_tasks[g_current_task].proc;
    end = (addr + len + 0xFFFull) & ~0xFFFull;

    /* Check the whole range first: a partial application would leave the
     * address space in a state the caller never asked for.
     *
     * "Mapped" is the question, not "mapped and reachable from ring 3": a
     * PROT_NONE region is mapped with no user access, and mprotect turning
     * that into a usable stack is the entire point of the pattern. A page
     * munmap has freed is not mapped at all, and stays a fault - which is what
     * the ABI self-test checks, and what the first version of this broke. */
    for (va = addr; va < end; va += 4096ull) {
        uint64_t *pte = hw_pte_lookup(&proc->as, va);
        if (!pte || (*pte & PTE_PRESENT) == 0) {
            hw_log(VIBEOS_LOG_WARN, 13u, va, len,
                   "mprotect refused: page not mapped");
            return -VIBEOS_EFAULT;
        }
    }

    /* The list is the authority on whether the range is mapped, and it refuses
     * the whole request rather than applying part of it. The page-table pass
     * below then carries the decision out.
     *
     * A PROT_NONE region is a region like any other here - that is the point of
     * describing what was asked for. It used to be a mapping trick that only
     * the page tables knew about, which is why "is this address reserved?" and
     * "is this address mapped?" were the same question and the ABI self-test
     * caught mprotect accepting an address it had to refuse. */
    /* The region-list update is what a fork's vibeos_vma_clone races; take the
     * lock across it, and only across it. The page-table pass below does a TLB
     * shootdown, and holding the lock across that shootdown would deadlock a
     * sibling spinning in hw_mm_lock - it cannot ack the IPI until it leaves the
     * spin, and it will not until this releases - into the shootdown's timeout
     * panic. So the list race with fork is closed here; the page-table race is
     * not, and stays until the spin can service the IPI (phases.md). */
    hw_mm_lock(g_tasks[g_current_task].ps);
    if (vibeos_vma_protect(&g_tasks[g_current_task].ps->vmas, addr, end - addr,
                           hw_prot_of(prot)) != 0) {
        hw_mm_unlock(g_tasks[g_current_task].ps);
        hw_log(VIBEOS_LOG_WARN, 15u, addr, len,
               "mprotect refused: the range is not one this process asked for");
        return -VIBEOS_EFAULT;
    }
    hw_mm_unlock(g_tasks[g_current_task].ps);

    for (va = addr; va < end; va += 4096ull) {
        uint64_t *pte = hw_pte_lookup(&proc->as, va);
        /* The pass above established that every page in the range is mapped,
         * so this cannot be NULL - but checking there and not here is the kind
         * of asymmetry that survives a later edit to one loop and not the
         * other, and the cost of being consistent is one branch. */
        if (!pte) {
            continue;
        }
        /* Reachability and writability are two bits, and mprotect decides
         * both. Only the write bit used to be touched, on the assumption that
         * anything mapped was already reachable from ring 3 - which stopped
         * being true when a PROT_NONE region became a real mapping with
         * PTE_USER deliberately clear. The page then stayed present and
         * unreachable, and the thread that had just been given a stack faulted
         * on its first write to it: present, user, write - error code 7. */
        {
            /* Through L1, which does the read-modify-write and preserves
             * everything the entry records that a permission change does not
             * alter: the frame, the ownership mark, the copy-on-write mark.
             *
             * One behaviour changes, and it is a fix rather than a
             * consequence. This used to grant PTE_WRITE on request even to a
             * page marked copy-on-write, which let a forked process write
             * straight into a page its parent was still reading - the exact
             * corruption fork's sharing exists to prevent, reachable from a
             * program simply calling mprotect on its own memory. The layer
             * leaves such a page read-only; the write then faults, the fault
             * makes the copy, and the program gets what it asked for one fault
             * later without anybody else's memory changing. */
            vibeos_vmspace_t v = hw_vm(&proc->as);
            vibeos_prot_t p = VIBEOS_PROT_NONE;

            if (prot != PROT_NONE) {
                p = (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_USER);
                if (prot & PROT_WRITE) {
                    p = (vibeos_prot_t)(p | VIBEOS_PROT_WRITE);
                }
                if (prot & PROT_EXEC) {
                    p = (vibeos_prot_t)(p | VIBEOS_PROT_EXEC);
                }
            }
            (void)vibeos_vmspace_protect(&v, va, p);
        }
        hw_invlpg(va);
    }
    return 0;
}

/* munmap(): remove mappings and give the frames back.
 *
 * The arena is a bump allocator, so the address space is not reclaimed for
 * reuse - but the pages are unmapped for real, so a use-after-unmap faults
 * here exactly as it would on Linux instead of quietly still working. */
static long hw_sys_munmap(uint64_t addr, uint64_t len) {
    hw_proc_t *proc;
    uint64_t va, end;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
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
    proc = &g_tasks[g_current_task].proc;
    end = (addr + len + 0xFFFull) & ~0xFFFull;
    /* One mutation of this process's address space at a time: a fork cloning
     * the tables and the region list, or a sibling's brk, must not see this
     * range half-removed. Single exit below, so one release. See hw_mm_lock. */
    hw_mm_lock(g_tasks[g_current_task].ps);
    /* The list decides what this range contains; the page tables are then made
     * to agree. That order is the phase: the tables record what the hardware
     * currently does, and asking *them* what to release is what let munmap free
     * frames that belonged to somebody else.
     *
     * Removed first, so a region is never described after it has stopped
     * existing - the same publish-last rule as everywhere else in here. */
    (void)vibeos_vma_remove(&g_tasks[g_current_task].ps->vmas, addr, end - addr);
    {
        vibeos_vmspace_t v = hw_vm(&proc->as);

        for (va = addr; va < end; va += 4096ull) {
            /* One call, and it is the last page-table write that lived outside
             * kernel/mm/vmspace.c.
             *
             * The two rules this used to get wrong are now properties of the
             * layer rather than of this loop. It freed the frame outright,
             * which is correct only for a page nobody else has - and after a
             * fork that is the rare case, so unmapping a copy-on-write page put
             * it back on the free list while another process was still running
             * from it. And it decided ownership by asking whether the page was
             * present and user-reachable, the same inference that freed the
             * kernel's identity map from teardown; munmap had simply never been
             * pointed at a low-window address by anything that mattered.
             *
             * That premature free was chased three times from the far end and
             * presented as something different each time: a musl program
             * tripping over its own malloc bins, init printing a pointer where
             * a pid belonged, a forked child reading back something it had not
             * written. */
            if (vibeos_vmspace_unmap(&v, va) == 1) {
                hw_log(VIBEOS_LOG_DEBUG, 45u, va, 0,
                       "munmap released a page (a0 = address)");
            }
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
    hw_tlbq_drain();
    hw_mm_unlock(g_tasks[g_current_task].ps);
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
static long hw_sys_pageinfo(uint64_t va, uint64_t out_uptr) {
    vibeos_pageinfo_t info;
    uint64_t *pte;
    uint64_t entry;

    if (g_current_task < 0 || !g_tasks[g_current_task].is_user) {
        return -VIBEOS_EINVAL;
    }
    if (!linux_user_ok(out_uptr, sizeof(info), 1)) {
        return -VIBEOS_EFAULT;
    }

    info.frame = 0;
    info.flags = 0;
    info.owners = 0;
    info.first_word = 0;

    pte = hw_pte_lookup(&g_tasks[g_current_task].proc.as, va);
    if (pte) {
        entry = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
        if (entry & PTE_PRESENT) {
            uint64_t phys = entry & 0x000FFFFFFFFFF000ull;
            uint32_t id = vibeos_frame_id(phys);

            info.flags |= VIBEOS_PAGE_PRESENT;
            if (entry & PTE_WRITE)         { info.flags |= VIBEOS_PAGE_WRITE; }
            if (entry & PTE_USER)          { info.flags |= VIBEOS_PAGE_USER; }
            if (entry & PTE_COW)           { info.flags |= VIBEOS_PAGE_COW; }
            if (entry & VIBEOS_PTE_OWNED)  { info.flags |= VIBEOS_PAGE_OWNED; }
            if (entry & PTE_NX)           { info.flags |= VIBEOS_PAGE_NX; }
            /* Everything below describes one frame, so it is all read from the
             * same pinned instant (M-038). A sibling thread can munmap the page,
             * the frame can be released and handed to another process, and a
             * bare read through the address taken from an entry this function
             * does not own returns that process's first eight bytes - and an
             * identity and an owner count taken at different moments describe
             * different frames.
             *
             * Pin the frame first - try_get refuses one nobody owns - and then
             * confirm the entry still names it: had the frame been freed and
             * reused before the pin, the entry would have changed. Only then are
             * the identity, the count and the word this caller's own page's.
             * If the page is gone, the flags from the entry stay and the rest
             * reports nothing, which is the truth.
             *
             * A frame the allocator does not describe - the reserved low
             * identity window - reports no identity rather than a wrong one, and
             * has no owner to race with, so it is read as before. The word is
             * read through the kernel's identity map deliberately, not through
             * the caller's translation: the value of this field is that it does
             * not use the mapping under suspicion. */
            if (id == 0xFFFFFFFFu) {
                info.first_word = *(const volatile uint64_t *)(uintptr_t)phys;
            } else if (vibeos_frame_try_get(phys)) {
                if (__atomic_load_n(pte, __ATOMIC_ACQUIRE) == entry) {
                    info.frame = (uint64_t)id + 1ull;   /* 0 stays "nothing" */
                    /* Our own pin is one of the owners; the caller asked about
                     * the mappings that were there. */
                    info.owners = vibeos_frame_owners(phys) - 1u;
                    info.first_word = *(const volatile uint64_t *)(uintptr_t)phys;
                }
                (void)vibeos_frame_put(phys);
            }
        }
    }

    /* Through the fault-tolerant copy: the range was checked above, but a
     * sibling's munmap of the output buffer can land after the check. */
    if (vibeos_uaccess_copy((void *)(uintptr_t)out_uptr, &info, sizeof(info)) != 0) {
        return -VIBEOS_EFAULT;
    }
    return 0;
}

/* ---- the syscalls this file implements --------------------------------------- */
#define LINUX_MM_SYSCALLS(X) \
    X(9,    mmap,     MAP,      hw_sys_mmap(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4))) \
    X(10,   mprotect, PROTECT,  hw_sys_mprotect(ARG(0), ARG(1), ARG(2))) \
    X(11,   munmap,   UNMAP,    hw_sys_munmap(ARG(0), ARG(1))) \
    X(12,   brk,      BRK,      hw_sys_brk(ARG(0))) \
    X(1001, pageinfo, PAGEINFO, hw_sys_pageinfo(ARG(0), ARG(1)))

LINUX_DEFINE_SYSCALLS(mm, LINUX_MM_SYSCALLS)
