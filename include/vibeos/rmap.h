#ifndef VIBEOS_RMAP_H
#define VIBEOS_RMAP_H

#include <stdint.h>

/* L1a: the reverse map — which mappings hold a frame.
 *
 * Every other structure here answers "what does this address space map?".
 * This one answers the opposite question, and nothing could ask it before:
 * given a frame, which page-table entries point at it? The kernel could only
 * find out by scanning every address space and trusting the result, which is
 * the "reconstruct the truth from the hardware bits" mistake this whole rewrite
 * exists to end.
 *
 * Two phases of the plan are blocked on being able to ask it, which is why the
 * plan moved it from the last item of P6 to the first:
 *
 *   Compaction (P6.5) moves a frame to make contiguous space. Moving one means
 *   finding every entry that points at it and repointing them; a frame whose
 *   holders cannot be enumerated cannot be moved.
 *
 *   Swap (P5) evicts a frame, which means unmapping it from everyone. After a
 *   fork a frame belongs to several address spaces, so an eviction that knows
 *   only one of them leaves the others pointing at a slot that no longer holds
 *   their page. P5's own note says it waits for this rather than restricting
 *   itself to singly-mapped frames - which would be a swap that cannot evict
 *   the pages a forking workload actually accumulates.
 *
 * ## What it is not
 *
 * It is not the ownership count. `owners` says *how many*, which is what
 * lifetime decisions need and is cheap to keep exact. This says *which*, which
 * is what moving and unmapping need. Keeping the two separate is deliberate:
 * the count is on the hot path of every map and unmap, and the list is not.
 *
 * ## The invariant
 *
 * For a frame the address-space layer owns, the number of nodes on its list
 * equals its owner count. That is checkable, it is checked, and a mismatch is
 * counted rather than assumed away - because the defect this subsystem keeps
 * producing is precisely a mapping that no count knew about.
 */

/* One holder of a frame: the address space's root, and the address it maps it
 * at. The root is the physical address of the PML4, which is what the arch
 * layer already uses to identify an address space and what a shootdown takes. */
typedef struct vibeos_rmap_holder {
    uint64_t root_phys;
    uint64_t va;
} vibeos_rmap_holder_t;

/* Give the layer its storage. Called once, with a pool of nodes carved from
 * memory the caller owns; the layer never allocates. A pool that runs out is
 * reported through vibeos_rmap_stats()->exhausted rather than by failing a
 * mapping - losing the ability to *move* a frame is survivable, losing the
 * ability to map one is not. */
int vibeos_rmap_init(void *pool, uint64_t bytes, uint32_t frames);

/* Its own lock, supplied by whoever has one.
 *
 * The fourth time this project has needed this, and the reason is written down
 * in CLAUDE.md: a layer with mutable statics and more than one possible caller
 * locks itself, because "remember to hold the lock" is not a property a
 * compiler checks. And its own lock rather than a borrowed one - this is called
 * from inside the address-space layer, which is called from inside the frame
 * layer's callers, and sharing either lock would deadlock on the first map. */
void vibeos_rmap_set_lock(void (*lock)(void), void (*unlock)(void));

/* Record that `root_phys` maps `frame_phys` at `va`, or forget it again.
 * Adding a holder that is already recorded is a no-op, not a duplicate: map
 * over an existing entry is a legitimate operation and must not grow the list.
 */
int vibeos_rmap_add(uint64_t frame_phys, uint64_t root_phys, uint64_t va);
int vibeos_rmap_remove(uint64_t frame_phys, uint64_t root_phys, uint64_t va);

/* Forget every holder of a frame. For teardown, where walking the list one
 * entry at a time would be quadratic in the size of the address space. */
void vibeos_rmap_forget_frame(uint64_t frame_phys);

/* Forget everything an address space holds. Teardown calls this after it has
 * released the frames, so a root that is about to be reused starts clean even
 * if something failed half way. */
void vibeos_rmap_forget_root(uint64_t root_phys);

/* How many mappings hold this frame. */
uint32_t vibeos_rmap_count(uint64_t frame_phys);

/* Enumerate them. `out` receives up to `max` holders; the return value is how
 * many were written, which is at most `max` even when more exist - a caller
 * that needs all of them checks against vibeos_rmap_count. */
uint32_t vibeos_rmap_holders(uint64_t frame_phys, vibeos_rmap_holder_t *out,
                             uint32_t max);

typedef struct vibeos_rmap_stats {
    uint64_t nodes_used;      /* currently recorded holders                   */
    uint64_t nodes_peak;      /* high water mark, so the pool can be sized    */
    uint64_t exhausted;       /* an add that found no free node. SHOULD BE 0  */
    uint64_t missing_remove;  /* a remove that found nothing. MUST BE ZERO    */
} vibeos_rmap_stats_t;

vibeos_rmap_stats_t *vibeos_rmap_stats(void);

#endif /* VIBEOS_RMAP_H */
