#ifndef VIBEOS_RECLAIM_H
#define VIBEOS_RECLAIM_H

#include <stdint.h>

/* L4: reclaim — surviving memory pressure, in a deliberate order.
 *
 * Two questions, and they are not the same one:
 *
 *   *When* to reclaim. Watermarks. Below the low mark a reclaim runs; below
 *   the minimum an allocation that is not privileged is refused rather than
 *   taking the last of memory away from the kernel that has to clean up after
 *   it. Without a minimum, the machine dies at the moment it most needs to
 *   free something.
 *
 *   *What* to reclaim. Cheapest first, and never anything pinned. A clean page
 *   cache entry costs nothing to drop because the file still has it. Anything
 *   else costs a write, and the pages that need one cannot be evicted at all
 *   until there is somewhere to write them - so this phase evicts the clean
 *   tier and says so, rather than pretending to a completeness it has not got.
 *
 * ## Why the order is a property and not a preference
 *
 * "Evict something" is easy to get accidentally right on a machine with plenty
 * of memory and accidentally catastrophic on one without. An eviction policy
 * that reaches a page table, a DMA buffer, or a frame a device already has the
 * address of does not degrade - it corrupts, and it corrupts asynchronously.
 * So pinning is not an optimisation here; it is the safety property, and it is
 * checked before anything is taken.
 *
 * ## What this deliberately does not do yet
 *
 * Anonymous pages need swap (P5) and swap needs the reverse map, which is why
 * the reverse map moved to the front of this phase. Until then an anonymous
 * page is simply not a candidate, and `skipped_no_swap` counts how often that
 * mattered - so "reclaim did nothing" and "reclaim had nothing it was allowed
 * to take" are different numbers rather than the same silence.
 *
 * Compaction (step 5) is separate: reclaiming free memory and making it
 * *usable* in one piece are different problems, and the second needs the
 * reverse map to repoint what it moves.
 */

typedef enum vibeos_mem_pressure {
    VIBEOS_MEM_OK = 0,     /* above the low mark; nothing to do              */
    VIBEOS_MEM_LOW,        /* below the low mark; reclaim should run         */
    VIBEOS_MEM_CRITICAL    /* below the minimum; refuse unprivileged demand  */
} vibeos_mem_pressure_t;

/* Set the marks, as a count of free frames.
 *
 * Given rather than computed from a percentage, because the right numbers
 * depend on what the machine is for and a fraction that suits a big machine
 * starves a small one. `min` must be below `low`, and both are refused if they
 * are not - a minimum above the low mark would make every allocation critical
 * and nothing would ever run. */
int vibeos_reclaim_set_marks(uint64_t low, uint64_t min);

/* What the current free count means. */
vibeos_mem_pressure_t vibeos_reclaim_pressure(void);

/* May this allocation proceed?
 *
 * `privileged` is for the allocations that exist to *end* the pressure - page
 * tables the reclaim itself needs, the kernel's own bookkeeping. Refusing
 * those is how a machine deadlocks at the minimum instead of recovering from
 * it. */
int vibeos_reclaim_admit(int privileged);

/* Try to free `want` frames, and return how many were actually freed.
 *
 * Returning the count rather than a success flag is deliberate: a caller that
 * asked for eight and got three needs to decide what to do about the five, and
 * a boolean throws that away. */
uint32_t vibeos_reclaim_run(uint32_t want);

/* Pin and unpin. A pinned frame is never a candidate. Page tables, DMA
 * buffers, and anything a device holds an address for. */
void vibeos_reclaim_pin(uint64_t phys);
void vibeos_reclaim_unpin(uint64_t phys);
int  vibeos_reclaim_is_pinned(uint64_t phys);

/* Where the clean tier comes from. The page cache already knows which of its
 * entries are resident and how to drop one; reclaim asks rather than growing a
 * second copy of that knowledge, which is how two structures come to disagree
 * about the same fact. Returns the number of frames actually dropped. */
typedef uint32_t (*vibeos_reclaim_drop_fn)(uint32_t want);

void vibeos_reclaim_set_clean_source(vibeos_reclaim_drop_fn fn);

typedef struct vibeos_reclaim_stats {
    uint64_t scans;             /* reclaim runs                              */
    uint64_t freed_clean;       /* frames dropped from the clean tier        */
    uint64_t skipped_pinned;    /* candidates refused because pinned         */
    uint64_t skipped_no_swap;   /* anonymous pages, unreclaimable until P5   */
    uint64_t admit_refused;     /* allocations refused at the minimum        */
    uint64_t low_events;        /* transitions into LOW                      */
    uint64_t critical_events;   /* transitions into CRITICAL                 */
    uint64_t compact_runs;      /* compaction attempts                       */
    uint64_t compact_frames_moved;
} vibeos_reclaim_stats_t;

vibeos_reclaim_stats_t *vibeos_reclaim_stats(void);

/* Open a contiguous run of `want` frames by moving what is in the way.
 *
 * Returns the largest free run after trying, in frames - so a caller compares
 * it against what it asked for and decides, rather than being told "success"
 * about a run it cannot use. A machine with plenty free and nothing in one
 * piece is the whole reason this exists, and it is the state a boolean would
 * hide.
 *
 * Best-effort by construction: it picks the window that needs the fewest moves
 * and moves what it is allowed to. Anything pinned or writable stays, so a
 * window can be chosen and then not fully cleared - which is why the honest
 * answer is a measurement rather than a verdict. */
uint32_t vibeos_reclaim_compact(uint32_t want);

/* The frame region, so the compactor can walk it. Given rather than asked for,
 * because the frame layer holds its own lock and this walks outside it. */
void vibeos_reclaim_set_region(uint64_t base_phys, uint32_t frames);

/* How the layer sees free memory. Supplied rather than called directly so the
 * host tests can drive pressure without a frame allocator. */
void vibeos_reclaim_set_free_source(uint64_t (*free_frames)(void));

#endif /* VIBEOS_RECLAIM_H */
