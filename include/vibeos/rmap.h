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

/* The physical address the frame table starts at, so this layer can turn an
 * address into an index without calling back into the frame layer while a
 * different lock is held. Set before init. */
void vibeos_rmap_set_base(uint64_t base_phys);

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

/* Claiming a holder: what keeps an address space's page tables alive while
 * reclaim works inside them (M-056).
 *
 * Reclaim runs from the allocation path, on any core, and evicts a page from an
 * address space that is not its own - holding none of that process's locks.
 * Before claims existed, nothing stopped the owner from exiting in the middle:
 * teardown freed the page tables that the eviction was walking, and the
 * eviction's compare-exchange then wrote into a page the allocator had already
 * taken back.
 *
 * claim_sole returns the frame's holder only when it has exactly one, and
 * records a claim on that holder's root, under this layer's lock - the lock
 * teardown takes first. forget_root, which teardown calls before it frees
 * anything, removes the root's holders (so no new claim on it can succeed) and
 * then waits for the claims already taken to be released. The wait goes through
 * a hook, called with how many times this wait has spun so far, because what
 * waiting means - and how long is too long - is the machine's to say: the
 * machine turns a wait that never ends into a named panic, as it does for its
 * locks. With no hook it spins.
 *
 * Returns 0 and fills `out`, or -1: not exactly one holder, or the claim table
 * is full (counted as claim_full). */
int vibeos_rmap_claim_sole(uint64_t frame_phys, vibeos_rmap_holder_t *out);
void vibeos_rmap_unclaim(uint64_t root_phys);
void vibeos_rmap_set_relax(void (*relax)(uint64_t spins));

/* The claim table: one entry per root with a claim outstanding. More than
 * enough for one reclaimer per core. */
#define VIBEOS_RMAP_CLAIMS 16u

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
    uint64_t cycles;          /* a holder list that loops. MUST BE ZERO       */
    uint64_t untracked;       /* an add for a frame this layer does not
                               * describe. Counted because it used to be the
                               * one failure branch that incremented nothing:
                               * the mapping is simply not recorded and no
                               * number said how often. Not a MUST BE ZERO -
                               * a frame outside the pool is a legitimate
                               * thing to map - but it is how much of the
                               * invariant below does not apply.            */
    uint64_t claims;          /* holders claimed by reclaim                    */
    uint64_t claim_waits;     /* teardowns that found a claim on their root and
                               * waited for it - the race M-056 described,
                               * happening and being handled                  */
    uint64_t claim_full;      /* a claim refused for want of a table entry     */
    uint64_t unclaim_missing; /* an unclaim of a root with no claim. MUST BE
                               * ZERO: a claim released twice would let a
                               * teardown through while a reclaimer is still
                               * inside its tables                            */
} vibeos_rmap_stats_t;


/* ---- what this layer promises, and what it does not -----------------------
 *
 * The reverse map is **best effort**. `vibeos_rmap_add` can fail - the node
 * pool is finite, and a frame outside the region this layer describes has no
 * list at all - and every caller ignores the result on purpose. The mapping is
 * published, the reference count is taken, and the holder is simply not
 * recorded. `arch_hw.c` states that choice at the allocation site: an unusual
 * workload should degrade reclaim rather than fail a mapping.
 *
 * That is only safe because of a property nothing used to write down:
 *
 *     **No decision rests on `vibeos_rmap_count` alone.** Every consumer
 *     compares it against `vibeos_frame_owners` first, and refuses when the two
 *     disagree.
 *
 * `owners` is taken by the mapping itself and knows nothing about this pool, so
 * an under-recorded holder makes the two disagree and the operation refuses. A
 * page that should be swapped out is not; a frame that could be compacted is
 * not. Reclaim degrades, and `exhausted` and `nodes_peak` say why.
 *
 * Trusting `rmap_count` on its own turns that into corruption: swap-out would
 * evict a frame a second address space still maps, and compaction would move
 * one and leave the other mapping pointing at the old address. Both of those
 * consumers exist today and both cross-check. `check-rmap-crosscheck.py`
 * enforces it, because a rule that lives only in a comment erodes one
 * reasonable-looking line at a time.
 */

vibeos_rmap_stats_t *vibeos_rmap_stats(void);

#endif /* VIBEOS_RMAP_H */
