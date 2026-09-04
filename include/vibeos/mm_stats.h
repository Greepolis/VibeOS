#ifndef VIBEOS_MM_STATS_H
#define VIBEOS_MM_STATS_H

#include <stdint.h>

/* Memory management counters.
 *
 * Every layer of the memory manager reports here, including the layers that do
 * not exist yet. That is deliberate and it is the cheapest half of the rewrite
 * plan: when the page cache arrives, adding its hit rate means filling in a
 * number rather than inventing a way to see one, and a regression is caught
 * because somebody was already watching the value before it moved.
 *
 * The counters marked MUST BE ZERO below are assertions, not diagnostics. The
 * boot gate fails on them. They exist because this subsystem has produced four
 * consecutive wrong diagnoses of one defect, each of which would have been a
 * single line of output if anybody had been counting.
 *
 * See docs/mm/observability.md for what each one means and which gate
 * assertion covers it.
 */
typedef struct vibeos_mm_stats {
    /* L0 - physical frames. */
    uint64_t frames_total;       /* frames the table describes                */
    uint64_t frames_free;        /* owners == 0                               */
    uint64_t frames_allocated;   /* handed out and not yet released           */
    uint64_t frames_leaked;      /* release refused: no entry. MUST BE ZERO   */
    uint64_t frames_double_put;  /* release of an unowned frame. MUST BE ZERO */
    uint64_t poison_hits;        /* write to a freed page. MUST BE ZERO       */
    uint64_t double_allocs;      /* handed out while owned. MUST BE ZERO      */
    uint64_t free_while_mapped;  /* more mappers than owners. MUST BE ZERO    */
    uint64_t fork_undercounted;  /* shared frame with <2 owners. MUST BE ZERO */
    uint64_t rmap_mismatch;      /* holders != owners. MUST BE ZERO           */

    /* L1 - address spaces. */
    uint64_t maps;               /* user PTEs created                         */
    uint64_t unmaps;             /* user PTEs destroyed                       */
    uint64_t cow_shared;         /* pages handed to a child instead of copied */
    uint64_t cow_copied;         /* copies a later write forced               */

    /* Times a page looked exclusively ours, was widened to writable, and turned
     * out to have been shared in the window between the two. Must be rare; it
     * must never be impossible to observe, because it was silently common. */
    uint64_t cow_exclusive_lost;
    uint64_t tlb_shootdowns;     /* cross-core invalidations sent             */
    uint64_t tlb_acks;           /* ...and acknowledged                       */
    uint64_t tlb_timeouts;       /* a core never answered. MUST BE ZERO       */
    uint64_t faults_resolved;    /* page faults handled and resumed           */
    uint64_t faults_fatal;       /* page faults that killed a task            */

    /* L2 - regions. Zero until P3. */
    uint64_t vmas_live;          /* regions currently described               */
    uint64_t vmas_created;       /* insert calls that produced a region       */
    uint64_t vmas_split;         /* a partial unmap or protect cut one in two */
    uint64_t vmas_peak;          /* the most regions ever live at once       */
    uint64_t vmas_refused;       /* inserts the pool could not satisfy       */
    uint64_t vmas_removed;       /* remove calls, whatever they found         */

    /* L3 - backing stores. Zero until P4 and P5. */
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_evictions;
    uint64_t cache_writebacks;
    uint64_t swap_ins;
    uint64_t swap_outs;
    uint64_t swap_used;

    /* L4 - reclaim policy. Zero until P6. */
    uint64_t reclaim_scans;
    uint64_t reclaim_freed;
} vibeos_mm_stats_t;

/* The live counters. Never null; readable at any point after boot, including
 * from a panic path, because it is a plain structure with no lock and no
 * allocation behind it. Readers may see a torn pair on a 32-bit split, which is
 * accepted: these are for humans and for assertions on magnitudes, not for
 * arithmetic that has to balance to the unit. */
vibeos_mm_stats_t *vibeos_mm_stats(void);

/* Host tests only. Zeroes every field. Not called by the kernel: a counter that
 * can be reset at runtime is a counter that can hide a leak. */
void vibeos_mm_stats_reset(void);

#endif /* VIBEOS_MM_STATS_H */
