#ifndef VIBEOS_ANON_H
#define VIBEOS_ANON_H

#include <stdint.h>

/* Reclaim's anonymous tier: the thing that actually sends a page to swap.
 *
 * Everything underneath was built and host-tested long before this existed -
 * the swap map hands out slots, `vibeos_vmspace_swap_out` does the page-table
 * work, the reverse map knows who holds a frame, the area translates a slot to
 * a sector. What was missing was the one piece that decides *which* page goes,
 * and without it `vibeos_reclaim_set_anon_source` was called by nobody and the
 * whole stack was unreachable on a booting machine. That is the defect this
 * project produces most often, and this file is the caller whose absence it
 * was.
 *
 * ## What makes a page a candidate
 *
 * Four conditions, and each one is a rule about safety rather than policy:
 *
 * - **allocated**, or there is nothing to evict;
 * - **not pinned**, because a pinned frame is one the kernel is using;
 * - **exactly one owner and exactly one mapping**, because evicting a shared
 *   frame means changing every holder's entry and dealing with every holder's
 *   TLB - a different operation, and one `vibeos_vmspace_swap_out` refuses;
 * - **owned by the address space that maps it** (`VIBEOS_PTE_OWNED`), which is
 *   what separates an anonymous page from an image page served by the cache.
 *   A cache page must not go to swap: the file still has it, and dropping it
 *   costs nothing.
 *
 * The last two are enforced by the layer below as well. Checked here anyway,
 * because reaching a refusal is a wasted slot allocation and a wasted walk.
 *
 * ## A hand, not a scan from zero
 *
 * The scan resumes where it stopped. Starting from zero every time would
 * examine the same low frames on every reclaim and reach the rest only under
 * severe pressure - which is the shape of a mechanism that looks like it works
 * and quietly covers a fraction of memory.
 */

/* Where the page contents can be reached from. The same arrangement as every
 * other layer here: a portable layer that reached for the identity map could
 * not be host-tested, and everything in this subsystem that could not be
 * host-tested is where the defects were. */
void vibeos_anon_set_map(void *(*map_phys)(uint64_t phys));

/* Evict up to `want` pages to swap; returns how many actually went.
 *
 * Registered with `vibeos_reclaim_set_anon_source`. Returning fewer than asked
 * is normal and is not an error: swap can be full, and every candidate can be
 * shared.
 */
uint32_t vibeos_anon_reclaim(uint32_t want);

typedef struct vibeos_anon_stats {
    uint64_t scanned;        /* frames examined                              */
    uint64_t evicted;        /* pages that reached swap                      */
    uint64_t no_slot;        /* swap was full                                */
    uint64_t refused;        /* the page-table layer declined the eviction   */
    uint64_t slot_leaked;    /* a slot taken and not freed after a refusal.
                              * MUST BE ZERO: a leaked slot is swap space no
                              * reboot gets back.                            */
} vibeos_anon_stats_t;

vibeos_anon_stats_t *vibeos_anon_stats(void);

#endif /* VIBEOS_ANON_H */
