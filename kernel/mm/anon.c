/* Reclaim's anonymous tier. See include/vibeos/anon.h for why it exists. */

#include "vibeos/anon.h"
#include "vibeos/frame.h"
#include "vibeos/mm_model.h"
#include "vibeos/reclaim.h"
#include "vibeos/rmap.h"
#include "vibeos/swapmap.h"
#include "vibeos/vmspace.h"

static void *(*g_map)(uint64_t phys);
/* Both touched from every core that allocates (M-056): reclaim runs from the
 * allocation path, and two cores under pressure reclaim at once. Atomics, not
 * a lock - this layer holds a disk write inside its loop, and a spinlock would
 * hold that write with interrupts off on every core that wanted memory. */
static uint32_t g_hand;
static vibeos_anon_stats_t g_stats;

void vibeos_anon_set_map(void *(*map_phys)(uint64_t phys)) {
    g_map = map_phys;
}

vibeos_anon_stats_t *vibeos_anon_stats(void) {
    return &g_stats;
}

uint32_t vibeos_anon_reclaim(uint32_t want) {
    uint64_t total = vibeos_frame_total();
    uint64_t looked = 0;
    uint32_t done = 0;

    if (!g_map || want == 0u || total == 0ull || vibeos_swap_slots() == 0u) {
        return 0;
    }

    while (done < want && looked < total) {
        uint64_t phys;
        vibeos_rmap_holder_t holder;
        vibeos_vmspace_t as;
        uint32_t slot = 0;

        /* One position per call, taken atomically: two reclaimers read-modify-
         * writing a plain hand would examine the same frame twice and skip
         * others. Wrapped by modulo rather than by a store of zero that a
         * second core could overwrite with its stale increment. */
        phys = vibeos_frame_phys_at((uint64_t)__atomic_fetch_add(&g_hand, 1u, __ATOMIC_RELAXED) % total);
        looked++;
        __atomic_fetch_add(&g_stats.scanned, 1u, __ATOMIC_RELAXED);

        if (phys == 0ull ||
            vibeos_frame_state(phys) != VIBEOS_FRAME_ALLOCATED) {
            continue;
        }
        /* Counted, not merely skipped. reclaim's skipped_pinned was declared
         * for this and nothing wrote to it, so "reclaim found nothing to take"
         * and "everything reclaim looked at was pinned" were the same silence -
         * and one of those is the kernel holding memory it will not give back. */
        if (vibeos_frame_test_flag(phys, VIBEOS_FRAME_PINNED)) {
            vibeos_reclaim_stats()->skipped_pinned++;
            continue;
        }
        /* One owner, and exactly one holder - claimed (M-056). The claim is
         * what keeps the holder's page tables alive until this eviction is done
         * with them: its owner may exit on another core meanwhile, and teardown
         * waits in vibeos_rmap_forget_root for the claim to be released instead
         * of freeing tables this loop is still walking and writing. */
        if (vibeos_frame_owners(phys) != 1u) {
            continue;
        }
        if (vibeos_rmap_claim_sole(phys, &holder) != 0) {
            continue;
        }

        /* A slot before the eviction, because vibeos_vmspace_swap_out needs
         * somewhere to write and cannot ask for one itself - it is the layer
         * below this and must not depend on it. */
        if (vibeos_swap_alloc(&slot) != 0) {
            vibeos_rmap_unclaim(holder.root_phys);
            __atomic_fetch_add(&g_stats.no_slot, 1u, __ATOMIC_RELAXED);
            break;              /* swap is full; the next candidate will be too */
        }

        as.root_phys = holder.root_phys;
        as.root = g_map(holder.root_phys);
        if (!as.root ||
            vibeos_vmspace_swap_out(&as, holder.va, slot) != 0) {
            vibeos_rmap_unclaim(holder.root_phys);
            /* Give the slot back. A refusal that kept it would leak swap space
             * that no reboot gets back, and refusals are the common case here:
             * a forking workload shares most of its pages and every shared one
             * is declined. */
            if (vibeos_swap_free(slot) != 0) {
                __atomic_fetch_add(&g_stats.slot_leaked, 1u, __ATOMIC_RELAXED);
            }
            __atomic_fetch_add(&g_stats.refused, 1u, __ATOMIC_RELAXED);
            continue;
        }
        vibeos_rmap_unclaim(holder.root_phys);
        __atomic_fetch_add(&g_stats.evicted, 1u, __ATOMIC_RELAXED);
        done++;
    }
    return done;
}
