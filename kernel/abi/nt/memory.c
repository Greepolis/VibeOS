/* NT personality: virtual memory (docs/abi/windows.md W3 step 2).
 *
 * NtAllocateVirtualMemory, NtFreeVirtualMemory, NtProtectVirtualMemory and
 * NtQueryVirtualMemory, for the calling process. Windows' model has two states
 * mmap never needed: address space *reserved* - set aside, no pages, no access -
 * and memory *committed* - promised, and made on first touch. Both are region
 * states (vibeos/vma.h); the page fault makes a committed page when it is
 * reached, as it always made the stack's.
 *
 * What is Windows' here and nowhere else: reservations at 64 KiB granularity,
 * commits by the page inside one, release only of a whole reservation by its
 * base, PAGE_* and back, and MEMORY_BASIC_INFORMATION. Placing, releasing and
 * RLIMIT_AS are the address space's (kernel/abi/mem/umem.c), shared with mmap.
 *
 * Every call reads its arguments from the program before it takes the mm lock
 * and writes its answers after releasing it: a store into the program's memory
 * can fault into a path that takes that lock (ks_mm_lock).
 *
 * Gaps, named: PAGE_GUARD and PAGE_NOCACHE are recorded and reported, not
 * enforced - a guard page is accessible and raises no STATUS_GUARD_PAGE_VIOLATION.
 * MEM_TOP_DOWN places as any other request does. ZeroBits is honoured by
 * refusing what the arena cannot place under it, which is everything: the arena
 * is above four gigabytes. MEM_WRITE_WATCH and MEM_LARGE_PAGES are
 * STATUS_NOT_SUPPORTED. Another process's handle is W6's. */

#include "nt_internal.h"
#include "vibeos/umem.h"
#include "vibeos/vmspace.h"

#define NT_PAGE_MODIFIERS (VIBEOS_NT_PAGE_GUARD | VIBEOS_NT_PAGE_NOCACHE | VIBEOS_NT_PAGE_WRITECOMBINE)
#define NT_PAGE_ROUND(x)  (((x) + 0xFFFull) & ~0xFFFull)

/* A Windows protection as the region's, or -1 for one Windows refuses: exactly
 * one of the eight base values, at most one of the caching modifiers, and no
 * guard on a page nobody may touch. `copy_ok` says whether the copy-on-write
 * pair is allowed - not for private memory being allocated, where Windows
 * answers STATUS_INVALID_PAGE_PROTECTION. */
int nt_prot_to_region(uint32_t page, int copy_ok, vibeos_prot_t *out) {
    const uint32_t base = page & 0xFFu;
    const vibeos_prot_t r = (vibeos_prot_t)(VIBEOS_PROT_READ | VIBEOS_PROT_USER);

    if ((page & ~(0xFFu | NT_PAGE_MODIFIERS)) != 0u || base == 0u || (base & (base - 1u)) != 0u ||
        ((page & VIBEOS_NT_PAGE_NOCACHE) && (page & VIBEOS_NT_PAGE_WRITECOMBINE)) ||
        (base == VIBEOS_NT_PAGE_NOACCESS && (page & NT_PAGE_MODIFIERS))) {
        return -1;
    }
    if (!copy_ok && (base == VIBEOS_NT_PAGE_WRITECOPY || base == VIBEOS_NT_PAGE_EXECUTE_WRITECOPY)) {
        return -1;
    }
    switch (base) {
    case VIBEOS_NT_PAGE_NOACCESS:
        *out = VIBEOS_PROT_NONE;
        break;
    case VIBEOS_NT_PAGE_READONLY:
        *out = r;
        break;
    case VIBEOS_NT_PAGE_READWRITE:
    case VIBEOS_NT_PAGE_WRITECOPY:
        *out = (vibeos_prot_t)(r | VIBEOS_PROT_WRITE);
        break;
    case VIBEOS_NT_PAGE_EXECUTE:   /* x86 cannot execute what it cannot read */
    case VIBEOS_NT_PAGE_EXECUTE_READ:
        *out = (vibeos_prot_t)(r | VIBEOS_PROT_EXEC);
        break;
    default:
        *out = (vibeos_prot_t)(r | VIBEOS_PROT_WRITE | VIBEOS_PROT_EXEC);
        break;
    }
    return 0;
}

/* A region's protection as Windows reports it: as it was spelled, if it was;
 * otherwise read from what the region allows - an image's pages, say, which
 * the loader mapped and nobody spelled. */
static uint32_t nt_protect_of(const vibeos_vma_t *r) {
    if (r->user_prot != 0u) {
        return r->user_prot;
    }
    if (!(r->prot & VIBEOS_PROT_USER)) {
        return VIBEOS_NT_PAGE_NOACCESS;
    }
    if (r->prot & VIBEOS_PROT_EXEC) {
        return (r->prot & VIBEOS_PROT_WRITE) ? VIBEOS_NT_PAGE_EXECUTE_READWRITE
                                              : VIBEOS_NT_PAGE_EXECUTE_READ;
    }
    return (r->prot & VIBEOS_PROT_WRITE) ? VIBEOS_NT_PAGE_READWRITE : VIBEOS_NT_PAGE_READONLY;
}

/* The caller, as a process with memory, or null - and only for the
 * pseudo-handle for itself: a handle to another process is W6's. */
static vibeos_procstate_t *nt_mm_self(uint64_t process, int *me) {
    *me = ks_current();
    if (process != VIBEOS_NT_CURRENT_PROCESS || *me < 0 || !ks_id(*me)->is_user) {
        return 0;
    }
    return ks_ps(*me);
}

static int nt_get64(uint64_t uptr, uint64_t *v) {
    return vibeos_uaccess_copy(v, (const void *)(uintptr_t)uptr, sizeof(*v));
}

static int nt_put64(uint64_t uptr, uint64_t v) {
    return vibeos_uaccess_copy((void *)(uintptr_t)uptr, &v, sizeof(v));
}

/* Where the allocation based at `ab` ends. Its regions are contiguous - a
 * decommitted range stays a reserved region - so this is the first address
 * past them. Under the mm lock. */
static uint64_t nt_alloc_end(vibeos_procstate_t *ps, uint64_t ab) {
    const vibeos_vma_t *r;
    uint64_t va = ab;

    while ((r = vibeos_vma_find(&ps->vmas, va)) != 0 && r->alloc_base == ab) {
        va = r->base + r->len;
    }
    return va;
}

/* Does [start, end) lie wholly in regions of the allocation based at `ab`
 * (0: regions nobody allocated this way, an image's)? And is any of it only
 * reserved? Under the mm lock. */
static int nt_in_alloc(vibeos_procstate_t *ps, uint64_t start, uint64_t end, uint64_t ab,
                       int *reserved) {
    const vibeos_vma_t *r;
    uint64_t va = start;

    *reserved = 0;
    while (va < end) {
        r = vibeos_vma_find(&ps->vmas, va);
        if (!r || r->alloc_base != ab) {
            return 0;
        }
        if (r->state == VIBEOS_VMA_RESERVED) {
            *reserved = 1;
        }
        va = r->base + r->len;
    }
    return 1;
}

/* NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize,
 * AllocationType, Protect).
 *
 * MEM_RESERVE - or MEM_COMMIT with no address, which reserves too - sets aside
 * [base rounded down to 64 KiB, end rounded up to a page) at the address given
 * or wherever there is room, as one allocation; with MEM_COMMIT its pages are
 * committed as well. MEM_COMMIT alone commits pages of an existing allocation,
 * by the page, and gives pages already committed the new protection, as
 * Windows does. MEM_RESET is a hint that the contents are not needed, and is
 * taken by keeping them: Windows promises nothing about them either way. */
static long nt_allocate_virtual_memory(uint64_t process, uint64_t base_u, uint64_t zero_bits,
                                       uint64_t size_u, uint64_t type, uint64_t protect) {
    const uint32_t known = VIBEOS_NT_MEM_COMMIT | VIBEOS_NT_MEM_RESERVE | VIBEOS_NT_MEM_RESET |
                           VIBEOS_NT_MEM_TOP_DOWN | VIBEOS_NT_MEM_WRITE_WATCH |
                           VIBEOS_NT_MEM_RESET_UNDO | VIBEOS_NT_MEM_LARGE_PAGES;
    vibeos_procstate_t *ps;
    vibeos_prot_t prot;
    uint64_t want = 0, size = 0, start, end;
    uint32_t st = VIBEOS_STATUS_SUCCESS;
    int me, reserved;

    if (!(ps = nt_mm_self(process, &me))) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    if (nt_get64(base_u, &want) != 0 || nt_get64(size_u, &size) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if ((type & ~known) != 0u || !(type & (VIBEOS_NT_MEM_COMMIT | VIBEOS_NT_MEM_RESERVE |
                                           VIBEOS_NT_MEM_RESET | VIBEOS_NT_MEM_RESET_UNDO))) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    /* A reset is a request of its own, never part of an allocation. */
    if ((type & (VIBEOS_NT_MEM_RESET | VIBEOS_NT_MEM_RESET_UNDO)) &&
        (type & (VIBEOS_NT_MEM_COMMIT | VIBEOS_NT_MEM_RESERVE))) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    if (type & (VIBEOS_NT_MEM_WRITE_WATCH | VIBEOS_NT_MEM_LARGE_PAGES)) {
        return NT_STATUS(VIBEOS_STATUS_NOT_SUPPORTED);
    }
    if (zero_bits > 21u && zero_bits < 32u) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_3);
    }
    if (size == 0u || size > ~0ull - 0xFFFFull - want) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    if (!(type & (VIBEOS_NT_MEM_RESET | VIBEOS_NT_MEM_RESET_UNDO)) &&
        nt_prot_to_region((uint32_t)protect, 0, &prot) != 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PAGE_PROTECTION);
    }
    /* The arena is above four gigabytes, and ZeroBits asks for an address
     * below 2^(32 - ZeroBits) (or under a mask, past 31): nothing it could
     * place would do. Refused as Windows refuses a request it has no room
     * for. */
    if (zero_bits != 0u) {
        return NT_STATUS(VIBEOS_STATUS_NO_MEMORY);
    }

    ks_mm_lock(ps);
    if (type & (VIBEOS_NT_MEM_RESET | VIBEOS_NT_MEM_RESET_UNDO)) {
        start = want & ~0xFFFull;
        end = NT_PAGE_ROUND(want + size);
        {
            const vibeos_vma_t *r = vibeos_vma_find(&ps->vmas, start);

            if (!r || !nt_in_alloc(ps, start, end, r->alloc_base, &reserved) || reserved) {
                st = VIBEOS_STATUS_CONFLICTING_ADDRESSES;
            }
        }
    } else if (want == 0u || (type & VIBEOS_NT_MEM_RESERVE)) {
        const int commit = (type & VIBEOS_NT_MEM_COMMIT) != 0u;

        if (want != 0u) {
            start = want & ~(VIBEOS_NT_ALLOCATION_GRANULARITY - 1u);
            end = NT_PAGE_ROUND(want + size);
            if (!ks_user_fixed_ok(start, end - start)) {
                st = VIBEOS_STATUS_INVALID_PARAMETER_2;
            } else if (vibeos_umem_region_in(ps, start, end - start)) {
                st = VIBEOS_STATUS_CONFLICTING_ADDRESSES;
            }
        } else {
            end = NT_PAGE_ROUND(size);
            start = vibeos_umem_place(ps, end, VIBEOS_NT_ALLOCATION_GRANULARITY);
            end += start;
            if (start == 0u) {
                st = VIBEOS_STATUS_NO_MEMORY;
            }
        }
        if (st == VIBEOS_STATUS_SUCCESS && !vibeos_umem_as_room(ps, end - start)) {
            st = VIBEOS_STATUS_NO_MEMORY;   /* RLIMIT_AS, the commit limit's stand-in */
        }
        if (st == VIBEOS_STATUS_SUCCESS &&
            vibeos_vma_insert_alloc(&ps->vmas, start, end - start,
                                    commit ? prot : VIBEOS_PROT_NONE,
                                    commit ? (uint32_t)protect : 0u,
                                    commit ? VIBEOS_VMA_DEMAND : VIBEOS_VMA_RESERVED,
                                    start, (uint32_t)protect) != 0) {
            st = VIBEOS_STATUS_NO_MEMORY;   /* the pool of regions is finite */
        }
    } else {
        const vibeos_vma_t *r;

        start = want & ~0xFFFull;
        end = NT_PAGE_ROUND(want + size);
        r = vibeos_vma_find(&ps->vmas, start);
        /* Inside one allocation of this personality's: an image's pages, or
         * address space nobody reserved, are not committed by asking. */
        if (!r || r->alloc_base == 0u || r->backing != VIBEOS_BACKING_ANON ||
            !nt_in_alloc(ps, start, end, r->alloc_base, &reserved)) {
            st = VIBEOS_STATUS_CONFLICTING_ADDRESSES;
        } else if (vibeos_vma_change(&ps->vmas, start, end - start,
                                     VIBEOS_VMA_CHANGE_PROT | VIBEOS_VMA_CHANGE_STATE, prot,
                                     (uint32_t)protect, VIBEOS_VMA_DEMAND) != 0) {
            st = VIBEOS_STATUS_NO_MEMORY;
        } else {
            vibeos_umem_protect_pages(me, start, end, prot);   /* the pages already made */
        }
    }
    ks_mm_unlock(ps);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (nt_put64(base_u, start) != 0 || nt_put64(size_u, end - start) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtFreeVirtualMemory(ProcessHandle, BaseAddress, RegionSize, FreeType).
 *
 * MEM_RELEASE gives back a whole allocation, named by its base - with a size
 * of zero, or the allocation's own. MEM_DECOMMIT gives back the pages of a
 * range inside one, to the end of it for a size of zero, and leaves the range
 * reserved. The region list first, then the pages, as munmap orders it. */
static long nt_free_virtual_memory(uint64_t process, uint64_t base_u, uint64_t size_u,
                                   uint64_t type) {
    vibeos_procstate_t *ps;
    const vibeos_vma_t *r;
    uint64_t want = 0, size = 0, start, end = 0, ab, alloc_end;
    uint32_t st = VIBEOS_STATUS_SUCCESS;
    int me, reserved;

    if (!(ps = nt_mm_self(process, &me))) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    if (nt_get64(base_u, &want) != 0 || nt_get64(size_u, &size) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (type != VIBEOS_NT_MEM_RELEASE && type != VIBEOS_NT_MEM_DECOMMIT) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER_4);
    }
    if (size > ~0ull - 0xFFFull - want) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    start = want & ~0xFFFull;

    ks_mm_lock(ps);
    r = vibeos_vma_find(&ps->vmas, start);
    if (!r || r->alloc_base == 0u) {
        st = VIBEOS_STATUS_MEMORY_NOT_ALLOCATED;
    } else if (r->backing != VIBEOS_BACKING_ANON) {
        st = VIBEOS_STATUS_UNABLE_TO_DELETE_SECTION;   /* a view: NtUnmapViewOfSection's */
    } else {
        ab = r->alloc_base;
        alloc_end = nt_alloc_end(ps, ab);
        end = size == 0u ? alloc_end : NT_PAGE_ROUND(want + size);
        if (type == VIBEOS_NT_MEM_RELEASE) {
            if (start != ab) {
                st = VIBEOS_STATUS_FREE_VM_NOT_AT_BASE;
            } else if (end != alloc_end) {
                st = VIBEOS_STATUS_UNABLE_TO_FREE_VM;
            } else if (vibeos_umem_unmap(me, ps, start, end) != 0) {
                st = VIBEOS_STATUS_NO_MEMORY;
            }
        } else if (end > alloc_end || !nt_in_alloc(ps, start, end, ab, &reserved)) {
            st = VIBEOS_STATUS_UNABLE_TO_FREE_VM;
        } else if (vibeos_vma_change(&ps->vmas, start, end - start,
                                     VIBEOS_VMA_CHANGE_PROT | VIBEOS_VMA_CHANGE_STATE,
                                     VIBEOS_PROT_NONE, 0u, VIBEOS_VMA_RESERVED) != 0) {
            st = VIBEOS_STATUS_NO_MEMORY;
        } else {
            vibeos_vmspace_t v = ks_vm(me);
            uint64_t va;

            /* Through the address-space layer, which gives a frame back only
             * when nobody else holds it, and parks it until every core has
             * let go of its translation (see vibeos_umem_unmap). */
            for (va = start; va < end; va += 4096ull) {
                (void)vibeos_vmspace_unmap(&v, va);
            }
            ks_tlb_drain();
        }
    }
    ks_mm_unlock(ps);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (nt_put64(base_u, start) != 0 || nt_put64(size_u, end - start) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtProtectVirtualMemory(ProcessHandle, BaseAddress, RegionSize, NewProtect,
 * OldProtect). Every page of the range committed and of one allocation - or of
 * the loader's mappings, which an image's IAT is made writable in - and the old
 * protection is the first page's. The list decides, all or nothing, and the
 * pages that exist are made to agree. */
static long nt_protect_virtual_memory(uint64_t process, uint64_t base_u, uint64_t size_u,
                                      uint64_t protect, uint64_t old_u) {
    vibeos_procstate_t *ps;
    vibeos_prot_t prot;
    const vibeos_vma_t *r;
    uint64_t want = 0, size = 0, start, end;
    uint32_t st = VIBEOS_STATUS_SUCCESS, old = 0;
    int me, reserved;

    if (!(ps = nt_mm_self(process, &me))) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    if (nt_get64(base_u, &want) != 0 || nt_get64(size_u, &size) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    if (size == 0u || size > ~0ull - 0xFFFull - want) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    if (nt_prot_to_region((uint32_t)protect, 1, &prot) != 0) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PAGE_PROTECTION);
    }
    start = want & ~0xFFFull;
    end = NT_PAGE_ROUND(want + size);

    ks_mm_lock(ps);
    r = vibeos_vma_find(&ps->vmas, start);
    if (!r || !nt_in_alloc(ps, start, end, r->alloc_base, &reserved)) {
        st = VIBEOS_STATUS_CONFLICTING_ADDRESSES;
    } else if (reserved) {
        st = VIBEOS_STATUS_NOT_COMMITTED;
    } else {
        const vibeos_vma_t *c;
        uint64_t va;

        old = nt_protect_of(r);
        /* What may never be writable - a view of a section opened for reading -
         * is not made so by asking. */
        for (va = start; (prot & VIBEOS_PROT_WRITE) && va < end; va = c->base + c->len) {
            c = vibeos_vma_find(&ps->vmas, va);
            /* nt_in_alloc walked this range under the same lock and found a
             * region at every step - a guarantee held by another function,
             * which is asked again here rather than trusted (CodeQL #312). */
            if (!c) {
                st = VIBEOS_STATUS_CONFLICTING_ADDRESSES;
                break;
            }
            if (c->prot & VIBEOS_PROT_NOWRITE) {
                st = VIBEOS_STATUS_ACCESS_DENIED;
                break;
            }
        }
        if (st == VIBEOS_STATUS_SUCCESS &&
            vibeos_vma_change(&ps->vmas, start, end - start, VIBEOS_VMA_CHANGE_PROT, prot,
                              (uint32_t)protect, VIBEOS_VMA_DEMAND) != 0) {
            st = VIBEOS_STATUS_NO_MEMORY;
        }
        if (st == VIBEOS_STATUS_SUCCESS) {
            vibeos_umem_protect_pages(me, start, end, prot);
        }
    }
    ks_mm_unlock(ps);
    if (st != VIBEOS_STATUS_SUCCESS) {
        return NT_STATUS(st);
    }
    if (vibeos_uaccess_copy((void *)(uintptr_t)old_u, &old, sizeof(old)) != 0 ||
        nt_put64(base_u, start) != 0 || nt_put64(size_u, end - start) != 0) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

/* NtQueryVirtualMemory(ProcessHandle, BaseAddress, MemoryInformationClass,
 * MemoryInformation, Length, ReturnLength), MemoryBasicInformation.
 *
 * The run of pages from the one asked about that share everything the
 * structure says - the allocation, the state, the protection. Free memory runs
 * to the next region, or to the end of the user window. */
static long nt_query_virtual_memory(uint64_t process, uint64_t va, uint64_t cls, uint64_t buf,
                                    uint64_t len, uint64_t ret_len) {
    vibeos_nt_memory_basic_t mbi;
    vibeos_procstate_t *ps;
    const vibeos_vma_t *r, *n;
    const uint64_t page = va & ~0xFFFull;
    uint64_t got = sizeof(mbi);
    uint32_t i;
    int me;

    if (!(ps = nt_mm_self(process, &me))) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_HANDLE);
    }
    if ((uint32_t)cls != VIBEOS_NT_MEMORY_BASIC_INFORMATION) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_INFO_CLASS);
    }
    if (len < sizeof(mbi)) {
        return NT_STATUS(VIBEOS_STATUS_INFO_LENGTH_MISMATCH);
    }
    if (va >= ks_user_top()) {
        return NT_STATUS(VIBEOS_STATUS_INVALID_PARAMETER);
    }
    for (i = 0; i < sizeof(mbi); i++) {
        ((uint8_t *)&mbi)[i] = 0;
    }
    mbi.base_address = page;

    ks_mm_lock(ps);
    r = vibeos_vma_find(&ps->vmas, page);
    if (!r) {
        uint64_t next = ks_user_top();

        for (n = ps->vmas.head; n; n = n->next) {
            if (n->base > page && n->base < next) {
                next = n->base;
            }
        }
        mbi.region_size = next - page;
        mbi.state = VIBEOS_NT_MEM_FREE;
        mbi.protect = VIBEOS_NT_PAGE_NOACCESS;
    } else {
        const uint32_t protect = r->state == VIBEOS_VMA_RESERVED ? 0u : nt_protect_of(r);
        uint64_t end = r->base + r->len;

        mbi.allocation_base = r->alloc_base ? r->alloc_base : r->base;
        mbi.allocation_protect = r->alloc_prot ? r->alloc_prot : nt_protect_of(r);
        mbi.state = r->state == VIBEOS_VMA_RESERVED ? VIBEOS_NT_MEM_RESERVE : VIBEOS_NT_MEM_COMMIT;
        mbi.protect = protect;
        mbi.type = r->backing == VIBEOS_BACKING_ANON ? VIBEOS_NT_MEM_PRIVATE : VIBEOS_NT_MEM_MAPPED;
        /* Regions differ in what NtQueryVirtualMemory does not say - a
         * machine protection two spellings share - and run on as one. */
        while (r->alloc_base != 0u && (n = vibeos_vma_find(&ps->vmas, end)) != 0 &&
               n->alloc_base == r->alloc_base && n->state == r->state &&
               (n->state == VIBEOS_VMA_RESERVED || nt_protect_of(n) == protect)) {
            end = n->base + n->len;
        }
        mbi.region_size = end - page;
    }
    ks_mm_unlock(ps);
    if (vibeos_uaccess_copy((void *)(uintptr_t)buf, &mbi, sizeof(mbi)) != 0 ||
        (ret_len != 0u && nt_put64(ret_len, got) != 0)) {
        return NT_STATUS(VIBEOS_STATUS_ACCESS_VIOLATION);
    }
    return NT_STATUS(VIBEOS_STATUS_SUCCESS);
}

#define NT_MEMORY_SERVICES(X) \
    X(VIBEOS_NT_NR_NtAllocateVirtualMemory, NtAllocateVirtualMemory, MAP, \
      PTRS(OUT(1, 8), OUT(3, 8)), \
      nt_allocate_virtual_memory(ARG(0), ARG(1), ARG(2), ARG(3), NT_ARG32(4), NT_ARG32(5))) \
    X(VIBEOS_NT_NR_NtFreeVirtualMemory, NtFreeVirtualMemory, UNMAP, \
      PTRS(OUT(1, 8), OUT(2, 8)), \
      nt_free_virtual_memory(ARG(0), ARG(1), ARG(2), NT_ARG32(3))) \
    X(VIBEOS_NT_NR_NtProtectVirtualMemory, NtProtectVirtualMemory, PROTECT, \
      PTRS(OUT(1, 8), OUT(2, 8), OUT(4, 4)), \
      nt_protect_virtual_memory(ARG(0), ARG(1), ARG(2), NT_ARG32(3), ARG(4))) \
    X(VIBEOS_NT_NR_NtQueryVirtualMemory, NtQueryVirtualMemory, MINCORE, \
      PTRS(OUT(3, sizeof(vibeos_nt_memory_basic_t)), OUT_OPT(5, 8)), \
      nt_query_virtual_memory(ARG(0), ARG(1), NT_ARG32(2), ARG(3), ARG(4), ARG(5)))

NT_DEFINE_SERVICES(memory, NT_MEMORY_SERVICES)
