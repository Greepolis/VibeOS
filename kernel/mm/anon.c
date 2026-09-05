/* Reclaim's anonymous tier. See include/vibeos/anon.h for why it exists. */

#include "vibeos/anon.h"
#include "vibeos/frame.h"
#include "vibeos/mm_model.h"
#include "vibeos/rmap.h"
#include "vibeos/swapmap.h"
#include "vibeos/vmspace.h"

static void *(*g_map)(uint64_t phys);
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

        if ((uint64_t)g_hand >= total) {
            g_hand = 0;
        }
        phys = vibeos_frame_phys_at(g_hand);
        g_hand++;
        looked++;
        g_stats.scanned++;

        if (phys == 0ull ||
            vibeos_frame_state(phys) != VIBEOS_FRAME_ALLOCATED ||
            vibeos_frame_test_flag(phys, VIBEOS_FRAME_PINNED) ||
            vibeos_frame_owners(phys) != 1u ||
            vibeos_rmap_count(phys) != 1u) {
            continue;
        }
        if (vibeos_rmap_holders(phys, &holder, 1u) != 1u) {
            continue;
        }

        /* A slot before the eviction, because vibeos_vmspace_swap_out needs
         * somewhere to write and cannot ask for one itself - it is the layer
         * below this and must not depend on it. */
        if (vibeos_swap_alloc(&slot) != 0) {
            g_stats.no_slot++;
            break;              /* swap is full; the next candidate will be too */
        }

        as.root_phys = holder.root_phys;
        as.root = g_map(holder.root_phys);
        if (!as.root ||
            vibeos_vmspace_swap_out(&as, holder.va, slot) != 0) {
            /* Give the slot back. A refusal that kept it would leak swap space
             * that no reboot gets back, and refusals are the common case here:
             * a forking workload shares most of its pages and every shared one
             * is declined. */
            if (vibeos_swap_free(slot) != 0) {
                g_stats.slot_leaked++;
            }
            g_stats.refused++;
            continue;
        }
        g_stats.evicted++;
        done++;
    }
    return done;
}
