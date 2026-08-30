/* Host tests for the address-space layer (plan phase P2).
 *
 * The page-table format here is real x86-64; only the memory is malloc'd. That
 * is the whole reason this layer sits in kernel/mm/ rather than in the arch
 * file: the logic that decides what an address space owns can be exercised in
 * milliseconds instead of by booting a virtual machine and hoping the timing
 * comes out wrong.
 *
 * The cases that matter are the refusals. Every defect this layer replaces was
 * a teardown that freed something it did not own, so most of what follows
 * checks that something is *not* released.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/vmspace.h"
#include "vibeos/frame.h"
#include "vibeos/mm_stats.h"

int test_vmspace(void);

#define VS_FRAMES 512u
#define VS_BASE   0x200000ull

static unsigned char *g_ram;

static void *vs_map(uint64_t phys) {
    if (phys < VS_BASE || phys >= VS_BASE + (uint64_t)VS_FRAMES * 4096ull) {
        return 0;
    }
    return g_ram + (phys - VS_BASE);
}

static vibeos_frame_t g_ftable[VS_FRAMES];

static uint64_t vs_alloc_table(void) {
    return vibeos_frame_alloc(VIBEOS_FRAME_PAGE_TABLE);
}

static void vs_free_table(uint64_t phys) {
    (void)vibeos_frame_put(phys);
}

/* A stand-in for the kernel's shared low-window tables: one PDPT whose first
 * entry points at one page directory of 2 MiB identity leaves.
 *
 * They live at fixed physical addresses inside the fake RAM, not in static
 * arrays, because the backend translates *physical* addresses - the first
 * version of this test put them in C globals and the low-window map failed on
 * the spot, which was the test being wrong rather than the layer.
 *
 * Their frames are reserved, so the allocator never hands them out and any
 * attempt to release one is counted as a release of something with no owners.
 * That is deliberate: the case being tested is that teardown leaves them
 * alone, and a silent success would prove nothing. */
#define SHARED_PDPT_PHYS (VS_BASE)
#define SHARED_PD0_PHYS  (VS_BASE + 4096ull)

static uint64_t *shared_pdpt(void) { return (uint64_t *)vs_map(SHARED_PDPT_PHYS); }
static uint64_t *shared_pd0(void)  { return (uint64_t *)vs_map(SHARED_PD0_PHYS); }

static const uint64_t *vs_shared_pd(uint32_t gib) {
    return (gib == 0u) ? shared_pd0() : 0;
}

static int setup(int with_low_window) {
    unsigned i;

    memset(g_ftable, 0, sizeof(g_ftable));
    memset(g_ram, 0, (size_t)VS_FRAMES * 4096u);
    vibeos_mm_stats_reset();
    if (vibeos_frame_init(VS_BASE, (uint64_t)VS_FRAMES * 4096ull,
                          g_ftable, VS_FRAMES, vs_map) != 0) {
        return -1;
    }
    if (vibeos_frame_reserve(SHARED_PDPT_PHYS, 2ull * 4096ull) != 0) {
        return -1;
    }
    for (i = 0; i < 512u; i++) {
        /* 2 MiB identity leaves, present and writable and supervisor-only -
         * the shape that a teardown deciding ownership by inspection used to
         * mistake for user pages. */
        shared_pd0()[i] = ((uint64_t)i << 21) | 1ull | 2ull | (1ull << 7);
    }
    shared_pdpt()[0] = SHARED_PD0_PHYS | 1ull | 2ull;

    {
        vibeos_vmspace_backend_t be;
        memset(&be, 0, sizeof(be));
        be.map_phys = vs_map;
        be.alloc_table = vs_alloc_table;
        be.free_table = vs_free_table;
        if (with_low_window) {
            /* Slot 0 of every address space starts as the kernel's shared
             * PDPT, which is what create() installs and what destroy()
             * recognises and leaves alone. */
            be.kernel_pml4e = SHARED_PDPT_PHYS | 1ull | 2ull;
            be.identity_limit = 0x40000000ull;
            be.shared_pdpt = shared_pdpt();
            be.shared_pd = vs_shared_pd;
        }
        return vibeos_vmspace_init(&be);
    }
}

int test_vmspace(void) {
    vibeos_vmspace_t as;
    uint64_t f1, f2;
    uint64_t free_before;

    g_ram = (unsigned char *)malloc((size_t)VS_FRAMES * 4096u);
    if (!g_ram) {
        return -1;
    }

    /* ---- map takes a reference, unmap releases exactly one --------------- */
    if (setup(0) != 0) { goto fail; }
    if (vibeos_vmspace_create(&as) != 0) { goto fail; }
    f1 = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (f1 == 0ull) { goto fail; }
    if (vibeos_vmspace_map(&as, 0x8000000000ull, f1,
                           VIBEOS_PROT_READ | VIBEOS_PROT_WRITE |
                           VIBEOS_PROT_USER) != 0) { goto fail; }
    if (vibeos_frame_owners(f1) != 2u) { goto fail; }   /* allocation + mapping */
    if (vibeos_vmspace_owned_count(&as) != 1ull) { goto fail; }
    if (vibeos_mm_stats()->maps != 1ull) { goto fail; }

    /* The entry says it is ours, and that is a recorded fact rather than
     * something inferred from the permission bits. */
    {
        uint64_t *e = vibeos_vmspace_entry(&as, 0x8000000000ull);
        if (!e || (*e & VIBEOS_PTE_OWNED) == 0ull) { goto fail; }
    }

    if (vibeos_vmspace_unmap(&as, 0x8000000000ull) != 1) { goto fail; }
    if (vibeos_frame_owners(f1) != 1u) { goto fail; }   /* the allocation's */
    if (vibeos_vmspace_unmap(&as, 0x8000000000ull) != 0) { goto fail; }
    if (vibeos_frame_owners(f1) != 1u) { goto fail; }   /* and not twice */

    /* ---- mapping over an owned entry releases the old frame -------------- */
    if (setup(0) != 0) { goto fail; }
    if (vibeos_vmspace_create(&as) != 0) { goto fail; }
    f1 = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    f2 = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (!f1 || !f2) { goto fail; }
    if (vibeos_vmspace_map(&as, 0x8000000000ull, f1, VIBEOS_PROT_READ) != 0) { goto fail; }
    if (vibeos_vmspace_map(&as, 0x8000000000ull, f2, VIBEOS_PROT_READ) != 0) { goto fail; }
    if (vibeos_frame_owners(f1) != 1u) { goto fail; }   /* the mapping let go */
    if (vibeos_frame_owners(f2) != 2u) { goto fail; }

    /* ---- destroy releases what is owned, and only that ------------------- */
    if (vibeos_vmspace_destroy(&as) != 0) { goto fail; }
    if (vibeos_frame_owners(f2) != 1u) { goto fail; }
    if (vibeos_mm_stats()->frames_double_put != 0ull) { goto fail; }
    if (vibeos_mm_stats()->frames_leaked != 0ull) { goto fail; }

    /* ---- destroy gives every table back ---------------------------------
     *
     * Not a tidiness check. Address spaces are created and destroyed on every
     * exec, so a table leaked per process is a machine that runs out of memory
     * after a few hundred commands - which reads as an unrelated failure much
     * later in whatever program is unlucky. */
    if (setup(0) != 0) { goto fail; }
    free_before = vibeos_frame_free_count();
    if (vibeos_vmspace_create(&as) != 0) { goto fail; }
    f1 = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (vibeos_vmspace_map(&as, 0x8000000000ull, f1, VIBEOS_PROT_READ) != 0) { goto fail; }
    if (vibeos_vmspace_map(&as, 0x8000000001000ull, f1, VIBEOS_PROT_READ) != 0) { goto fail; }
    if (vibeos_vmspace_destroy(&as) != 0) { goto fail; }
    (void)vibeos_frame_put(f1);
    if (vibeos_frame_free_count() != free_before) {
        printf("FAIL:vmspace leaked %llu frames across create/destroy\n",
               (unsigned long long)(free_before - vibeos_frame_free_count()));
        goto fail;
    }

    /* ---- the low window: the kernel's identity map survives teardown -----
     *
     * This is the case the whole layer is for. Splitting a 2 MiB identity leaf
     * leaves 512 entries that are present, writable and the kernel's. A
     * teardown that decided ownership by looking at them freed the kernel's own
     * memory, and the machine stopped with no output at all. They carry no
     * ownership bit, so they are simply not considered. */
    if (setup(1) != 0) { goto fail; }
    if (vibeos_vmspace_create(&as) != 0) { goto fail; }
    f1 = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (!f1) { goto fail; }
    if (vibeos_vmspace_map(&as, 0x400000ull, f1,
                           VIBEOS_PROT_READ | VIBEOS_PROT_USER) != 0) { goto fail; }
    if (vibeos_frame_owners(f1) != 2u) { goto fail; }
    if (vibeos_vmspace_owned_count(&as) != 1ull) { goto fail; }

    /* The shared tables were copied, not modified: the kernel's originals are
     * untouched, and the copy still identity-maps everything except the one
     * page the process asked for. */
    if (shared_pd0()[2] != ((2ull << 21) | 1ull | 2ull | (1ull << 7))) {
        printf("FAIL:the shared page directory was modified in place\n");
        goto fail;
    }
    if (vibeos_vmspace_destroy(&as) != 0) { goto fail; }
    if (vibeos_frame_owners(f1) != 1u) { goto fail; }
    /* Nothing outside the frame table was handed to the allocator. If a split
     * identity entry or a shared table had been released, this is where it
     * would show, because those addresses are not describable. */
    if (vibeos_mm_stats()->frames_leaked != 0ull) {
        printf("FAIL:teardown released %llu frames it did not own\n",
               (unsigned long long)vibeos_mm_stats()->frames_leaked);
        goto fail;
    }
    if (vibeos_mm_stats()->frames_double_put != 0ull) { goto fail; }

    /* ---- a PROT_NONE guard page is owned like any other ------------------
     *
     * It has no PTE_USER, which is exactly why "reachable from ring 3" was
     * never a safe test for ownership: a C library maps a thread stack and its
     * guard as one PROT_NONE region, and losing those frames on exit is a leak
     * per thread. */
    if (setup(0) != 0) { goto fail; }
    if (vibeos_vmspace_create(&as) != 0) { goto fail; }
    f1 = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
    if (vibeos_vmspace_map(&as, 0x8000000000ull, f1, VIBEOS_PROT_NONE) != 0) { goto fail; }
    {
        uint64_t *e = vibeos_vmspace_entry(&as, 0x8000000000ull);
        if (!e) { goto fail; }
        if ((*e & VIBEOS_PTE_OWNED) == 0ull) { goto fail; }
        if ((*e & 4ull) != 0ull) {            /* PTE_USER must be clear */
            printf("FAIL:PROT_NONE page is reachable from ring 3\n");
            goto fail;
        }
        if ((*e & 1ull) == 0ull) {            /* ...and it must still be present */
            printf("FAIL:PROT_NONE was refused instead of mapped\n");
            goto fail;
        }
    }
    if (vibeos_vmspace_owned_count(&as) != 1ull) { goto fail; }
    if (vibeos_vmspace_destroy(&as) != 0) { goto fail; }
    if (vibeos_frame_owners(f1) != 1u) { goto fail; }

    free(g_ram);
    g_ram = 0;
    return 0;

fail:
    free(g_ram);
    g_ram = 0;
    return -1;
}
