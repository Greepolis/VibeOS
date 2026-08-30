/* Randomised torture for the memory manager, run against a reference model.
 *
 * The host tests check the cases somebody thought of. This checks the ones
 * nobody did: it drives the frame layer and the address-space layer through
 * long random sequences of create, map, remap, unmap and destroy, and after
 * every single operation it compares every frame's owner count against a model
 * kept independently in plain arrays.
 *
 * That comparison is the point. Every hard defect in this subsystem has been a
 * reference count that was self-consistent and wrong about the world - the
 * kernel believed nobody owned a frame while a live process was running from
 * it - and no amount of asserting the kernel against itself can catch that. A
 * separate model can.
 *
 * It prints its seed on the first line, so a failure can be replayed exactly:
 *
 *     vibeos_mm_torture <seed> [rounds]
 *
 * The nightly runs many seeds under AddressSanitizer, which is the other half:
 * the model catches wrong arithmetic, the sanitizer catches walking off the
 * descriptor table while doing it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vibeos/vmspace.h"
#include "vibeos/frame.h"
#include "vibeos/mm_stats.h"

#define TORTURE_FRAMES 1024u
#define TORTURE_BASE   0x200000ull
#define ASPACES        4u
#define SLOTS          24u    /* candidate addresses; small, so collisions are common */

static unsigned char *g_ram;

static void *t_map(uint64_t phys) {
    if (phys < TORTURE_BASE ||
        phys >= TORTURE_BASE + (uint64_t)TORTURE_FRAMES * 4096ull) {
        return 0;
    }
    return g_ram + (phys - TORTURE_BASE);
}

static vibeos_frame_t g_ftable[TORTURE_FRAMES];

static uint64_t t_alloc_table(void) {
    return vibeos_frame_alloc(VIBEOS_FRAME_PAGE_TABLE);
}
static void t_free_table(uint64_t phys) {
    (void)vibeos_frame_put(phys);
}

/* The shared low-window tables, as in the unit tests: inside the fake RAM
 * because the backend translates physical addresses, and reserved so that
 * releasing one is counted rather than silently tolerated. */
#define SHARED_PDPT_PHYS (TORTURE_BASE)
#define SHARED_PD0_PHYS  (TORTURE_BASE + 4096ull)

static const uint64_t *t_shared_pd(uint32_t gib) {
    return (gib == 0u) ? (const uint64_t *)t_map(SHARED_PD0_PHYS) : 0;
}

/* ---- the model ----------------------------------------------------------
 *
 * Deliberately dumb: an array of which frame each slot of each address space
 * holds, and nothing clever. A model that shares an idea with the code it
 * checks is not a check. */
static uint64_t g_model[ASPACES][SLOTS];   /* frame, or 0 for nothing mapped */
static uint64_t g_standalone[TORTURE_FRAMES]; /* extra references we hold */

static uint64_t slot_va(unsigned s) {
    /* Half in the low window, so the identity carving is exercised, and half in
     * the high one. Spread across two page tables in each. */
    if (s < SLOTS / 2u) {
        return 0x400000ull + (uint64_t)s * 4096ull;
    }
    return 0x8000000000ull + (uint64_t)(s - SLOTS / 2u) * 4096ull;
}

static unsigned long long g_rng;
static unsigned rnd(unsigned n) {
    g_rng = g_rng * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned)((g_rng >> 33) % n);
}

/* Expected owners of a frame: one for each address space mapping it, plus any
 * standalone reference this program holds. Page tables are not in the model -
 * the layer allocates and frees them - so only frames the model knows about
 * are checked. */
static uint64_t expected_owners(uint64_t frame) {
    uint64_t n = 0;
    unsigned a, s;

    for (a = 0; a < ASPACES; a++) {
        for (s = 0; s < SLOTS; s++) {
            if (g_model[a][s] == frame) {
                n++;
            }
        }
    }
    return n + g_standalone[(frame - TORTURE_BASE) / 4096ull];
}

static int check_all(const char *where, unsigned round) {
    unsigned a, s;

    for (a = 0; a < ASPACES; a++) {
        for (s = 0; s < SLOTS; s++) {
            uint64_t f = g_model[a][s];
            uint64_t want, got;
            if (!f) {
                continue;
            }
            want = expected_owners(f);
            got = vibeos_frame_owners(f);
            if (want != got) {
                printf("FAIL: round %u after %s: frame 0x%llx has %llu owners, "
                       "the model says %llu\n",
                       round, where, (unsigned long long)f,
                       (unsigned long long)got, (unsigned long long)want);
                return -1;
            }
            /* A frame something maps must never be sitting on the free list.
             * This is the premature free, asked directly. */
            if (vibeos_frame_state(f) == VIBEOS_FRAME_FREE) {
                printf("FAIL: round %u after %s: frame 0x%llx is free and "
                       "still mapped\n", round, where, (unsigned long long)f);
                return -1;
            }
        }
    }
    if (vibeos_mm_stats()->frames_double_put != 0ull ||
        vibeos_mm_stats()->poison_hits != 0ull ||
        vibeos_mm_stats()->frames_leaked != 0ull) {
        printf("FAIL: round %u after %s: double_put=%llu poison=%llu leaked=%llu\n",
               round, where,
               (unsigned long long)vibeos_mm_stats()->frames_double_put,
               (unsigned long long)vibeos_mm_stats()->poison_hits,
               (unsigned long long)vibeos_mm_stats()->frames_leaked);
        return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    vibeos_vmspace_t as[ASPACES];
    int live[ASPACES];
    unsigned rounds = 4000u;
    unsigned r, i;
    uint64_t free_at_start;

    g_rng = (argc > 1) ? strtoull(argv[1], 0, 0) : 1u;
    if (argc > 2) {
        rounds = (unsigned)strtoul(argv[2], 0, 0);
    }
    printf("mm-torture seed=%llu rounds=%u\n", (unsigned long long)g_rng, rounds);

    g_ram = (unsigned char *)malloc((size_t)TORTURE_FRAMES * 4096u);
    if (!g_ram) {
        return 2;
    }
    memset(g_ram, 0, (size_t)TORTURE_FRAMES * 4096u);
    memset(g_ftable, 0, sizeof(g_ftable));
    memset(g_model, 0, sizeof(g_model));
    memset(g_standalone, 0, sizeof(g_standalone));
    vibeos_mm_stats_reset();

    if (vibeos_frame_init(TORTURE_BASE, (uint64_t)TORTURE_FRAMES * 4096ull,
                          g_ftable, TORTURE_FRAMES, t_map) != 0 ||
        vibeos_frame_reserve(SHARED_PDPT_PHYS, 2ull * 4096ull) != 0) {
        return 2;
    }
    for (i = 0; i < 512u; i++) {
        ((uint64_t *)t_map(SHARED_PD0_PHYS))[i] =
            ((uint64_t)i << 21) | 1ull | 2ull | (1ull << 7);
    }
    ((uint64_t *)t_map(SHARED_PDPT_PHYS))[0] = SHARED_PD0_PHYS | 1ull | 2ull;

    {
        vibeos_vmspace_backend_t be;
        memset(&be, 0, sizeof(be));
        be.map_phys = t_map;
        be.alloc_table = t_alloc_table;
        be.free_table = t_free_table;
        be.kernel_pml4e = SHARED_PDPT_PHYS | 1ull | 2ull;
        be.identity_limit = 0x40000000ull;
        be.shared_pdpt = (const uint64_t *)t_map(SHARED_PDPT_PHYS);
        be.shared_pd = t_shared_pd;
        if (vibeos_vmspace_init(&be) != 0) {
            return 2;
        }
    }

    free_at_start = vibeos_frame_free_count();
    memset(live, 0, sizeof(live));

    for (r = 0; r < rounds; r++) {
        unsigned a = rnd(ASPACES);
        unsigned op = rnd(100u);

        if (!live[a]) {
            /* Bring it up rather than skipping the round, so the mix of
             * operations does not quietly drift towards doing nothing. */
            if (vibeos_vmspace_create(&as[a]) != 0) {
                continue;   /* out of memory is a legitimate answer (I5) */
            }
            live[a] = 1;
            memset(g_model[a], 0, sizeof(g_model[a]));
            if (check_all("create", r) != 0) { return 1; }
            continue;
        }

        if (op < 45u) {
            /* map, possibly over something already there */
            unsigned s = rnd(SLOTS);
            uint64_t f = vibeos_frame_alloc(VIBEOS_FRAME_ALLOCATED);
            vibeos_prot_t prot = VIBEOS_PROT_READ | VIBEOS_PROT_USER;

            if (!f) {
                continue;   /* exhausted; the layers must simply say no */
            }
            if (rnd(4u) == 0u) {
                prot = VIBEOS_PROT_NONE;      /* a guard page, owned and unreachable */
            } else if (rnd(2u)) {
                prot = (vibeos_prot_t)(prot | VIBEOS_PROT_WRITE);
            }
            /* The allocation's own reference is what the model calls
             * standalone; map takes a second. Dropping ours immediately is the
             * D9 contract that the kernel's callers follow. */
            if (vibeos_vmspace_map(&as[a], slot_va(s), f, prot) != 0) {
                (void)vibeos_frame_put(f);
                continue;
            }
            g_model[a][s] = f;
            (void)vibeos_frame_put(f);
            if (check_all("map", r) != 0) { return 1; }

        } else if (op < 70u) {
            unsigned s = rnd(SLOTS);
            uint64_t had = g_model[a][s];
            int rc = vibeos_vmspace_unmap(&as[a], slot_va(s));

            if (had && rc != 1) {
                printf("FAIL: round %u: unmap of a mapped slot returned %d\n", r, rc);
                return 1;
            }
            if (!had && rc == 1) {
                printf("FAIL: round %u: unmap released something the model "
                       "never mapped\n", r);
                return 1;
            }
            g_model[a][s] = 0;
            if (check_all("unmap", r) != 0) { return 1; }

        } else if (op < 85u) {
            /* Share a frame into another address space, which is what fork
             * does and where the counting has historically gone wrong. */
            unsigned s = rnd(SLOTS);
            unsigned b = rnd(ASPACES);
            uint64_t f = g_model[a][s];

            if (!f || !live[b] || b == a) {
                continue;
            }
            if (vibeos_vmspace_map(&as[b], slot_va(s), f,
                                   VIBEOS_PROT_READ | VIBEOS_PROT_USER) != 0) {
                continue;
            }
            g_model[b][s] = f;
            if (check_all("share", r) != 0) { return 1; }

        } else {
            /* destroy: everything this address space owns goes back at once */
            if (vibeos_vmspace_destroy(&as[a]) != 0) {
                printf("FAIL: round %u: destroy refused\n", r);
                return 1;
            }
            memset(g_model[a], 0, sizeof(g_model[a]));
            live[a] = 0;
            if (check_all("destroy", r) != 0) { return 1; }
        }
    }

    /* Tear everything down and account for every frame. A leak here is not
     * cosmetic: address spaces are created and destroyed on every exec, so a
     * frame lost per process is a machine that dies after a few hundred
     * commands, in whatever program happens to be running. */
    for (i = 0; i < ASPACES; i++) {
        if (live[i]) {
            (void)vibeos_vmspace_destroy(&as[i]);
            live[i] = 0;
        }
    }
    if (vibeos_frame_free_count() != free_at_start) {
        printf("FAIL: %llu frames were never returned "
               "(free %llu at the start, %llu at the end)\n",
               (unsigned long long)(free_at_start - vibeos_frame_free_count()),
               (unsigned long long)free_at_start,
               (unsigned long long)vibeos_frame_free_count());
        return 1;
    }
    if (vibeos_mm_stats()->frames_leaked != 0ull ||
        vibeos_mm_stats()->frames_double_put != 0ull ||
        vibeos_mm_stats()->poison_hits != 0ull) {
        printf("FAIL: counters non-zero at the end\n");
        return 1;
    }

    printf("mm-torture OK: %u rounds, every frame accounted for\n", rounds);
    free(g_ram);
    return 0;
}
