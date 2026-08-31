/* L1: address spaces.
 *
 * The only writer of page-table entries. See include/vibeos/vmspace.h for why
 * that matters and docs/mm/phases.md P2 for the order this was built in.
 *
 * The shape of the file is deliberate: every function that walks the tables
 * goes through `walk`, and every function that changes an entry goes through
 * `set_leaf` or `clear_leaf`. There is no second path. The defect this layer
 * exists to close survived four fixes precisely because there were several
 * places that each decided, independently and by inspection, whether a frame
 * belonged to the address space being torn down.
 */

#include "vibeos/vmspace.h"
#include "vibeos/frame.h"
#include "vibeos/mm_stats.h"

#define PTE_PRESENT (1ull << 0)
#define PTE_WRITE   (1ull << 1)
#define PTE_USER    (1ull << 2)
#define PTE_PS      (1ull << 7)
#define PTE_NX      (1ull << 63)

#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ull

/* The copy-on-write mark. This layer does not resolve copy-on-write faults yet
 * - that is phase P5 - but it sets the mark in fork, must not destroy it when
 * permissions change, and must not grant write access over the top of one.
 * Named here rather than written as a number in three places. */
#define PTE_COW_BIT (1ull << 9)   /* must equal PTE_COW in arch_hw.c */

static vibeos_vmspace_backend_t g_be;

/* See vibeos_vmspace_current_op. One store per entry point; no lock, because a
 * lock here would change the very timing being investigated. */
static const char *g_op = "none";

const char *vibeos_vmspace_current_op(void) {
    return g_op ? g_op : "none";
}
static int g_ready;

int vibeos_vmspace_init(const vibeos_vmspace_backend_t *backend) {
    if (!backend || !backend->map_phys || !backend->alloc_table) {
        return -1;
    }
    g_be = *backend;
    g_ready = 1;
    return 0;
}

uint64_t vibeos_vmspace_leaf_flags(vibeos_prot_t prot) {
    uint64_t f = PTE_PRESENT;

    /* PROT_NONE is a mapping, not a refusal.
     *
     * A C library builds a thread stack by asking for stack plus guard as one
     * PROT_NONE region and then making the usable part accessible, so refusing
     * it breaks pthread_create before it ever reaches clone(). The pages are
     * mapped and present, simply without PTE_USER, which faults from ring 3
     * exactly as a guard should. */
    if (prot & VIBEOS_PROT_WRITE) {
        f |= PTE_WRITE;
    }
    if (prot & VIBEOS_PROT_USER) {
        f |= PTE_USER;
    }
    return f;
}

/* ---- walking ------------------------------------------------------------ */

static uint64_t *table_at(uint64_t phys) {
    return (uint64_t *)g_be.map_phys(phys & PTE_ADDR_MASK);
}

/* Find the leaf entry for `va`, optionally creating the tables on the way.
 *
 * Returns null when a level is missing and `create` is zero, when a table
 * cannot be allocated, or when a 2 MiB leaf blocks the path - the last of which
 * is not an error the high window can produce and is handled by the low-window
 * carving below. */
static uint64_t *walk(vibeos_vmspace_t *as, uint64_t va, int create) {
    static const uint32_t shifts[3] = {39u, 30u, 21u};
    uint64_t *tbl = as->root;
    uint32_t level;

    for (level = 0; level < 3u; level++) {
        uint32_t idx = (uint32_t)((va >> shifts[level]) & 0x1FFu);

        if ((tbl[idx] & PTE_PRESENT) == 0u) {
            uint64_t page;
            if (!create) {
                return 0;
            }
            page = g_be.alloc_table();
            if (!page) {
                return 0;
            }
            /* Intermediate entries carry USER because access is the AND of the
             * bits along the path: the leaf is what decides, and an
             * intermediate that says no would make a PROT_NONE guard page and
             * a kernel page indistinguishable from ring 3's point of view.
             * They deliberately do not carry the ownership bit - they are
             * tables, not user frames, and destroy frees them by structure. */
            tbl[idx] = (page & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE | PTE_USER;
        } else if (tbl[idx] & PTE_PS) {
            return 0;   /* a large leaf stands here; the caller must split it */
        }
        tbl = table_at(tbl[idx]);
        if (!tbl) {
            return 0;
        }
    }
    return &tbl[(va >> 12) & 0x1FFu];
}

uint64_t *vibeos_vmspace_entry(vibeos_vmspace_t *as, uint64_t va) {
    if (!g_ready || !as || !as->root) {
        return 0;
    }
    return walk(as, va, 0);
}

/* ---- the low window ------------------------------------------------------
 *
 * Below the identity limit the kernel reaches memory by its physical address,
 * and the tables covering that region are shared by every address space and use
 * 2 MiB pages. There is nowhere to put a 4 KiB user entry without first making
 * private copies, so this walks down un-sharing exactly as much as it must:
 * the shared PDPT, then the shared page directory for that GiB, then the 2 MiB
 * leaf, which is split into 512 identity entries reproducing the same mapping
 * at finer granularity.
 *
 * Those 512 entries are the reason this layer exists. They are present and
 * writable and belong to the kernel, and a teardown that decided ownership by
 * looking at them freed the kernel's identity map. They do not carry the
 * ownership bit, so nothing has to reason about them ever again.
 */
static int unshare_low(vibeos_vmspace_t *as, uint64_t va, uint64_t **out_pt) {
    uint32_t gi  = (uint32_t)((va >> 30) & 0x1FFu);
    uint32_t pdi = (uint32_t)((va >> 21) & 0x1FFu);
    uint64_t *pdpt, *pd;

    pdpt = table_at(as->root[0]);
    if (!pdpt) {
        return -1;
    }
    if (g_be.shared_pdpt && pdpt == g_be.shared_pdpt) {
        uint64_t page = g_be.alloc_table();
        uint64_t *priv;
        uint32_t i;
        if (!page) {
            return -1;
        }
        priv = table_at(page);
        if (!priv) {
            return -1;
        }
        for (i = 0; i < 512u; i++) {
            priv[i] = g_be.shared_pdpt[i];
        }
        as->root[0] = (page & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE | PTE_USER;
        pdpt = priv;
    }

    pd = table_at(pdpt[gi]);
    if (g_be.shared_pd) {
        const uint64_t *shared = g_be.shared_pd(gi);
        if (shared && pd == shared) {
            uint64_t page = g_be.alloc_table();
            uint64_t *priv;
            uint32_t i;
            if (!page) {
                return -1;
            }
            priv = table_at(page);
            if (!priv) {
                return -1;
            }
            for (i = 0; i < 512u; i++) {
                priv[i] = shared[i];
            }
            pdpt[gi] = (page & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE | PTE_USER;
            pd = priv;
        }
    }
    if (!pd) {
        return -1;
    }

    if ((pd[pdi] & PTE_PRESENT) == 0u || (pd[pdi] & PTE_PS) != 0u) {
        uint64_t region = ((uint64_t)gi << 30) | ((uint64_t)pdi << 21);
        uint64_t page = g_be.alloc_table();
        uint64_t *priv;
        uint32_t i;
        if (!page) {
            return -1;
        }
        priv = table_at(page);
        if (!priv) {
            return -1;
        }
        for (i = 0; i < 512u; i++) {
            /* Same physical address, same supervisor-only access, finer
             * granularity. No PTE_USER: this is still the kernel's memory. And
             * no ownership bit: the kernel did not get these frames from this
             * address space and must not lose them when it dies. */
            priv[i] = (region + (uint64_t)i * 4096ull) | PTE_PRESENT | PTE_WRITE;
        }
        pd[pdi] = (page & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE | PTE_USER;
        *out_pt = priv;
        return 0;
    }

    *out_pt = table_at(pd[pdi]);
    return *out_pt ? 0 : -1;
}

/* ---- create and destroy -------------------------------------------------- */

int vibeos_vmspace_create(vibeos_vmspace_t *out) {
    uint64_t page;

    if (!g_ready || !out) {
        return -1;
    }
    page = g_be.alloc_table();
    if (!page) {
        return -1;
    }
    out->root_phys = page;
    out->root = table_at(page);
    if (!out->root) {
        g_be.free_table(page);
        out->root_phys = 0;
        return -1;
    }
    /* Slot 0 is the kernel's identity map: shared, supervisor-only, and never
     * freed by this layer. */
    out->root[0] = g_be.kernel_pml4e;
    return 0;
}

/* Walk one page table, releasing what this address space owns, and say whether
 * the table itself was private to it.
 *
 * A table is private when this layer allocated it, which is every table below
 * the top level that is not one of the backend's shared ones. Rather than test
 * against the shared list at every level - which is the inference this rewrite
 * removes - the caller tracks it structurally: a table reached through an entry
 * this layer wrote is private, and the only entries this layer does not write
 * are slot 0 of the root and the backend's shared tables, both known by
 * identity at the one place they are installed. */
static void release_pt(uint64_t *pt) {
    uint32_t i;

    for (i = 0; i < 512u; i++) {
        if ((pt[i] & PTE_PRESENT) && (pt[i] & VIBEOS_PTE_OWNED)) {
            /* Stop pointing at it, then let go - the same rule as map and the
             * fault. Teardown is the least exposed of the three, because the
             * address space is dying and nothing runs on it, but a rule with an
             * exception is a rule somebody will apply the exception to. */
            uint64_t phys = pt[i] & PTE_ADDR_MASK;

            pt[i] = 0;
            (void)vibeos_frame_put(phys);
            vibeos_mm_stats()->unmaps++;
        }
    }
}

static void release_pd(uint64_t *pd, const uint64_t *shared) {
    uint32_t i;

    for (i = 0; i < 512u; i++) {
        uint64_t e = pd[i];
        uint64_t *pt;

        if ((e & PTE_PRESENT) == 0u || (e & PTE_PS) != 0u) {
            continue;   /* absent, or a 2 MiB identity leaf that is not ours */
        }
        if (shared && (shared[i] & PTE_ADDR_MASK) == (e & PTE_ADDR_MASK)) {
            continue;   /* still the kernel's table; we never copied it */
        }
        pt = table_at(e);
        if (pt) {
            release_pt(pt);
        }
        g_be.free_table(e & PTE_ADDR_MASK);
        pd[i] = 0;
    }
}

int vibeos_vmspace_destroy(vibeos_vmspace_t *as) {
    uint32_t slot, gi;
    g_op = "destroy";

    if (!g_ready || !as || !as->root) {
        return -1;
    }

    for (slot = 0; slot < 512u; slot++) {
        uint64_t *pdpt;
        int slot_is_kernel = (slot == 0u);

        if ((as->root[slot] & PTE_PRESENT) == 0u) {
            continue;
        }
        /* Slot 0 is the kernel's, unless this process had pages in the low
         * window and unshare_low replaced it with a private copy. Comparing
         * against the entry create() installed is exact and needs no reasoning
         * about addresses. */
        if (slot_is_kernel &&
            (as->root[slot] & PTE_ADDR_MASK) ==
            (g_be.kernel_pml4e & PTE_ADDR_MASK)) {
            continue;
        }
        pdpt = table_at(as->root[slot]);
        if (!pdpt) {
            continue;
        }
        for (gi = 0; gi < 512u; gi++) {
            uint64_t e = pdpt[gi];
            uint64_t *pd;
            const uint64_t *shared = 0;

            if ((e & PTE_PRESENT) == 0u || (e & PTE_PS) != 0u) {
                continue;
            }
            if (slot_is_kernel && g_be.shared_pd) {
                shared = g_be.shared_pd(gi);
                if (shared && (shared == table_at(e))) {
                    continue;   /* never copied; the kernel still owns it */
                }
            }
            pd = table_at(e);
            if (pd) {
                release_pd(pd, shared);
            }
            g_be.free_table(e & PTE_ADDR_MASK);
            pdpt[gi] = 0;
        }
        g_be.free_table(as->root[slot] & PTE_ADDR_MASK);
        as->root[slot] = 0;
    }

    g_be.free_table(as->root_phys);
    as->root = 0;
    as->root_phys = 0;
    return 0;
}

/* ---- map and unmap ------------------------------------------------------- */

int vibeos_vmspace_map(vibeos_vmspace_t *as, uint64_t va, uint64_t pa,
                       vibeos_prot_t prot) {
    return vibeos_vmspace_map_raw(as, va, pa, vibeos_vmspace_leaf_flags(prot));
}

int vibeos_vmspace_map_raw(vibeos_vmspace_t *as, uint64_t va, uint64_t pa,
                           uint64_t leaf) {
    uint64_t *pte = 0;
    g_op = "map";

    if (!g_ready || !as || !as->root || (va & 0xFFFull) || (pa & 0xFFFull)) {
        return -1;
    }

    if (g_be.identity_limit != 0ull && va < g_be.identity_limit) {
        uint64_t *pt = 0;
        if (unshare_low(as, va, &pt) != 0) {
            return -1;
        }
        pte = &pt[(va >> 12) & 0x1FFu];
    } else {
        pte = walk(as, va, 1);
        if (!pte) {
            return -1;
        }
    }


    /* Take the reference *before* the entry is visible, not after.
     *
     * The other order published a mapping that no count knew about yet, and
     * every other core can see a page-table entry the instant it is stored.
     * During fork that window is wide enough to matter: a core resolving a
     * copy-on-write fault on the same frame reads owners == 1, concludes it is
     * the only owner, and keeps the shared page writable - so parent and child
     * end up writing the same memory. The mirror case is worse: the same read
     * lets a release take the count to zero while the freshly published entry
     * still points at the frame, which is a page reclaimed while a live process
     * maps it.
     *
     * Both were observed: the stress service saw a child read its parent's
     * value, and the release watch named the frame and the process that still
     * mapped it. Counting first costs nothing and closes the window entirely -
     * an extra reference on a mapping that then fails to be installed is a
     * leak, and a leak is the direction this subsystem chooses on purpose. */
    {
        /* A compare-exchange, like every other store to an entry in this file.
         *
         * This was a plain read-modify-write, and two cores mapping the same
         * address both read the old entry, both took a reference, both stored,
         * and both released what had been there - one release too many for a
         * single mapping, which puts a frame on the free list while somebody
         * still holds it. The reference is taken before the exchange and given
         * back if the exchange loses, so a frame is never published without a
         * count and never counted without being published.
         *
         * Replacing an owned entry releases what it held, and only after the
         * new entry is in place: this address space has to stop pointing at a
         * frame before it can let go of it. */
        uint64_t old;
        uint64_t desired = (pa & PTE_ADDR_MASK) | leaf | PTE_PRESENT |
                           VIBEOS_PTE_OWNED;

        for (;;) {
            old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
            vibeos_frame_get(pa & PTE_ADDR_MASK);
            if (__atomic_compare_exchange_n(pte, &old, desired, 0,
                                            __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
                break;
            }
            (void)vibeos_frame_put(pa & PTE_ADDR_MASK);
        }

        if ((old & PTE_PRESENT) && (old & VIBEOS_PTE_OWNED)) {
            (void)vibeos_frame_put(old & PTE_ADDR_MASK);
            vibeos_mm_stats()->unmaps++;
        }
    }
    vibeos_mm_stats()->maps++;

    if (g_be.invlpg) {
        g_be.invlpg(va);
    }
    return 0;
}

int vibeos_vmspace_protect(vibeos_vmspace_t *as, uint64_t va,
                           vibeos_prot_t prot) {
    uint64_t *pte;
    g_op = "protect";
    uint64_t before;

    if (!g_ready || !as || !as->root) {
        return -1;
    }
    pte = walk(as, va, 0);
    if (!pte || (*pte & PTE_PRESENT) == 0u) {
        return -1;   /* not mapped: a permission change is not a mapping */
    }
    before = *pte;

    /* Reachability and writability are two separate bits and this decides both.
     * Only the write bit used to be touched, on the assumption that anything
     * mapped was already reachable from ring 3 - which stopped being true when
     * a PROT_NONE region became a real mapping with PTE_USER deliberately
     * clear. The page then stayed present and unreachable, and a thread that
     * had just been given a stack faulted on its first write to it. */
    *pte &= ~(PTE_USER | PTE_WRITE);
    if (prot != VIBEOS_PROT_NONE) {
        if (prot & VIBEOS_PROT_USER) {
            *pte |= PTE_USER;
        }
        if ((prot & VIBEOS_PROT_WRITE) && (before & PTE_COW_BIT) == 0u) {
            *pte |= PTE_WRITE;
        }
    }

    if (g_be.invlpg) {
        g_be.invlpg(va);
    }
    /* Narrowing needs the other cores told; widening does not, because a stale
     * entry that is more restrictive only costs a spurious fault, and the fault
     * handler recognises an already-permitted access and simply invalidates. */
    if (g_be.shootdown && (before & (PTE_USER | PTE_WRITE)) &
                          ~(*pte & (PTE_USER | PTE_WRITE))) {
        g_be.shootdown(as->root_phys);
    }
    return 0;
}

int vibeos_vmspace_unmap(vibeos_vmspace_t *as, uint64_t va) {
    uint64_t *pte;
    uint64_t entry;
    g_op = "unmap";

    if (!g_ready || !as || !as->root) {
        return -1;
    }
    pte = walk(as, va, 0);
    if (!pte) {
        return 0;
    }
    entry = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    if ((entry & PTE_PRESENT) == 0u) {
        return 0;
    }
    if ((entry & VIBEOS_PTE_OWNED) == 0u) {
        /* Present, and not ours. An identity-split entry, or a table shared
         * with the kernel. Left exactly as it is: this is the case that used to
         * be freed on the strength of it being present and writable. */
        return 0;
    }
    {
        /* Clear it with a compare-exchange, and release only if this core is
         * the one that cleared it.
         *
         * Two threads unmapping the same address both read the same entry, both
         * store zero, and both release - two references dropped for one
         * mapping, which puts the frame on the free list while somebody still
         * has it. The same race as the copy-on-write fault, and the same
         * answer: the store decides who owns the release. */
        uint64_t expected = entry;

        if (!__atomic_compare_exchange_n(pte, &expected, 0ull, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            return 0;   /* somebody else unmapped it; theirs to release */
        }
        if (g_be.invlpg) {
            g_be.invlpg(va);
        }
        (void)vibeos_frame_put(entry & PTE_ADDR_MASK);
        vibeos_mm_stats()->unmaps++;
    }
    return 1;
}

/* ---- fork ---------------------------------------------------------------- */

/* Visit every leaf this address space owns, with the virtual address it sits
 * at. Shared by fork and by the inspection count, so the two cannot disagree
 * about what "owned" means - and they are the two places most likely to drift,
 * because one is on a hot path and the other is not. */
static int foreach_owned(vibeos_vmspace_t *as,
                         int (*fn)(vibeos_vmspace_t *, uint64_t, uint64_t *, void *),
                         void *ctx) {
    uint32_t slot, gi, pdi, i;

    for (slot = 0; slot < 512u; slot++) {
        uint64_t *pdpt;
        if ((as->root[slot] & PTE_PRESENT) == 0u) {
            continue;
        }
        pdpt = table_at(as->root[slot]);
        if (!pdpt) {
            continue;
        }
        for (gi = 0; gi < 512u; gi++) {
            uint64_t *pd;
            if ((pdpt[gi] & PTE_PRESENT) == 0u || (pdpt[gi] & PTE_PS)) {
                continue;
            }
            pd = table_at(pdpt[gi]);
            if (!pd) {
                continue;
            }
            for (pdi = 0; pdi < 512u; pdi++) {
                uint64_t *pt;
                if ((pd[pdi] & PTE_PRESENT) == 0u || (pd[pdi] & PTE_PS)) {
                    continue;
                }
                pt = table_at(pd[pdi]);
                if (!pt) {
                    continue;
                }
                for (i = 0; i < 512u; i++) {
                    uint64_t va;
                    if ((pt[i] & PTE_PRESENT) == 0u ||
                        (pt[i] & VIBEOS_PTE_OWNED) == 0u) {
                        continue;
                    }
                    va = ((uint64_t)slot << 39) | ((uint64_t)gi << 30) |
                         ((uint64_t)pdi << 21) | ((uint64_t)i << 12);
                    if (fn(as, va, &pt[i], ctx) != 0) {
                        return -1;
                    }
                }
            }
        }
    }
    return 0;
}

static int clone_one(vibeos_vmspace_t *src, uint64_t va, uint64_t *pte, void *ctx) {
    vibeos_vmspace_t *dst = (vibeos_vmspace_t *)ctx;
    uint64_t entry = *pte;
    uint64_t phys = entry & PTE_ADDR_MASK;
    uint64_t flags = entry & (PTE_PRESENT | PTE_USER);

    /* Three cases, and conflating the last two is a silent disaster.
     *
     *   writable        -> becomes copy-on-write on both sides
     *   already COW     -> stays copy-on-write; it is read-only because it is
     *                      shared, not because the program may not write it
     *   read-only       -> shared as is; it can never be written, so it never
     *                      needs duplicating, and marking it would turn a
     *                      genuine protection fault into a silent success
     *
     * A second fork sees the first fork's pages as read-only. Treating them as
     * the third case drops the mark, and the page becomes permanently
     * unwritable for everyone - a shell that runs two commands and dies on the
     * third. */
    if (entry & PTE_WRITE) {
        uint64_t expected = entry;
        uint64_t desired = (entry & ~PTE_WRITE) | PTE_COW_BIT;

        flags |= PTE_COW_BIT;
        if (!__atomic_compare_exchange_n(pte, &expected, desired, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            /* Another core changed this entry between the read and here - a
             * thread of the same process resolving its own fault on it. Take
             * what is there now rather than storing a stale value back, which
             * would reinstate a frame the other core has already stopped
             * pointing at and released. */
            entry = expected;
            phys = entry & PTE_ADDR_MASK;
            flags = entry & (PTE_PRESENT | PTE_USER);
            if (entry & PTE_COW_BIT) {
                flags |= PTE_COW_BIT;
            }
        }
        if (g_be.invlpg) {
            g_be.invlpg(va);
        }
    } else if (entry & PTE_COW_BIT) {
        flags |= PTE_COW_BIT;
    }

    /* The reference is taken by the mapping, as it is everywhere else. There is
     * deliberately no explicit count here: a future path that shares a frame
     * cannot forget to do something it never had to remember. */
    if (vibeos_vmspace_map_raw(dst, va, phys, flags) != 0) {
        return -1;
    }
    vibeos_mm_stats()->cow_shared++;
    (void)src;
    return 0;
}

int vibeos_vmspace_clone_cow(vibeos_vmspace_t *dst, vibeos_vmspace_t *src) {
    g_op = "fork";
    if (!g_ready || !dst || !src || !dst->root || !src->root) {
        return -1;
    }
    if (foreach_owned(src, clone_one, dst) != 0) {
        return -1;
    }
    if (g_be.shootdown) {
        g_be.shootdown(src->root_phys);
    }
    return 0;
}

/* ---- faults -------------------------------------------------------------- */

/* Copy 4 KiB between two frames, through whatever the backend calls memory. */
static int copy_frame(uint64_t dst, uint64_t src) {
    uint64_t *d = (uint64_t *)g_be.map_phys(dst);
    const uint64_t *s = (const uint64_t *)g_be.map_phys(src);
    uint32_t i;

    if (!d || !s) {
        return -1;
    }
    for (i = 0; i < 4096u / 8u; i++) {
        d[i] = s[i];
    }
    return 0;
}

int vibeos_vmspace_fault(vibeos_vmspace_t *as, uint64_t va, int write) {
    uint64_t *pte;
    uint64_t entry, expected, desired, phys, fresh;

    g_op = "cow-fault";
    if (!g_ready || !as || !as->root || !write) {
        return 0;
    }
    pte = walk(as, va, 0);
    if (!pte) {
        return 0;
    }

    /* Read the entry once, and act on that value.
     *
     * Two threads of one process can fault on the same page at the same
     * instant, and re-reading *pte at each step lets each of them make a
     * decision based on a world the other has already changed. Every store back
     * is a compare-exchange against this value, so a core that lost the race
     * discovers it instead of overwriting the winner. */
    entry = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    if ((entry & PTE_PRESENT) == 0u) {
        return 0;
    }

    /* Already writable: another thread resolved this page on another core and
     * this one faulted on a translation that had not caught up. Without this
     * the page looks like a plain read-only mapping - no copy-on-write mark,
     * because the other core cleared it - and the task is killed for a
     * violation it did not commit. */
    if ((entry & PTE_WRITE) && (entry & PTE_USER)) {
        if (g_be.invlpg) {
            g_be.invlpg(va);
        }
        return 1;
    }

    if ((entry & PTE_COW_BIT) == 0u || (entry & VIBEOS_PTE_OWNED) == 0u) {
        return 0;   /* not a shared page: a genuine protection violation */
    }
    phys = entry & PTE_ADDR_MASK;

    /* Sole owner: take the write permission back rather than copying. The
     * common case once the other side has exec'd or exited, and copying there
     * would waste a page and a copy for nothing. */
    if (vibeos_frame_owners(phys) <= 1u) {
        expected = entry;
        desired = phys | PTE_PRESENT | PTE_WRITE | PTE_USER | VIBEOS_PTE_OWNED;
        (void)__atomic_compare_exchange_n(pte, &expected, desired, 0,
                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
        /* Either we granted the write or somebody else already did; the
         * instruction is retried and will succeed or fault again honestly. */
        if (g_be.invlpg) {
            g_be.invlpg(va);
        }
        return 1;
    }

    fresh = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (!fresh) {
        return 0;   /* out of memory; nothing dropped, so nothing to undo (I5) */
    }
    if (copy_frame(fresh, phys) != 0) {
        (void)vibeos_frame_put(fresh);
        return 0;
    }

    /* Install the copy only if the entry is still the one we read.
     *
     * This is the race that survived the ordering fix. Two threads of the same
     * process faulting on one page both reached this point, both allocated a
     * copy, both stored, and both released the shared frame - so one reference
     * was dropped for a mapping that only ever existed once, and the frame went
     * back on the free list while the other process was still running from it.
     *
     * It fits the numbers the detector reported. With a page shared three ways
     * - a parent and two children - two threads of one child each drop a
     * reference for the single mapping they share, leaving two address spaces
     * holding the frame and one reference counted: mappers=2 owners=1, which is
     * what the log said.
     *
     * Losing the exchange is not a failure. The other core has already made
     * this page writable and private; our copy is unused, and the shared frame
     * was never ours to release. */
    expected = entry;
    desired = (fresh & PTE_ADDR_MASK) | PTE_PRESENT | PTE_WRITE | PTE_USER |
              VIBEOS_PTE_OWNED;
    if (!__atomic_compare_exchange_n(pte, &expected, desired, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
        (void)vibeos_frame_put(fresh);
        if (g_be.invlpg) {
            g_be.invlpg(va);
        }
        return 1;
    }

    /* We won: this address space has stopped pointing at the shared frame, so
     * now it may let go of it. */
    (void)vibeos_frame_put(phys);
    if (g_be.invlpg) {
        g_be.invlpg(va);
    }

    /* Not about permissions: the page's *physical address* just changed. A
     * thread of this same process on another core still has the old frame
     * cached, so from here on it reads and writes a page nobody else can see -
     * it never observes anything this thread stores.
     *
     * Not theory. A test that forks with a worker thread running, then sets a
     * flag the worker is spinning on, hung outright: the flag lived on a page
     * fork had marked copy-on-write, the parent's store copied it, and the
     * worker watched the original frame for a change that could never arrive. */
    if (g_be.shootdown) {
        g_be.shootdown(as->root_phys);
    }
    vibeos_mm_stats()->cow_copied++;
    return 1;
}

/* ---- inspection ---------------------------------------------------------- */

static int count_one(vibeos_vmspace_t *as, uint64_t va, uint64_t *pte, void *ctx) {
    (void)as; (void)va; (void)pte;
    (*(uint64_t *)ctx)++;
    return 0;
}

uint64_t vibeos_vmspace_owned_count(vibeos_vmspace_t *as) {
    uint64_t n = 0;

    if (!g_ready || !as || !as->root) {
        return 0;
    }
    /* The same walk fork uses. Four nested loops written twice is how two
     * pieces of code come to disagree about what "owned" means, and one of
     * these is on a hot path while the other is not - which is exactly the
     * pair that drifts. */
    (void)foreach_owned(as, count_one, &n);
    return n;
}
