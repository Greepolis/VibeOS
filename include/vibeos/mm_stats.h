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
    uint64_t rmap_audit_torn;    /* the audit's two reads saw different worlds */
    uint64_t free_watch_torn;    /* the frame was handed out during the watch  */
    /* The last frame that tripped rmap_mismatch, and the three counts that did
     * not add up, so a single-boot fire says which frame and by how much rather
     * than only that it happened. Instrument for the open rmap_mismatch defect. */
    uint64_t rmap_mm_phys;
    uint64_t rmap_mm_holders;
    uint64_t rmap_mm_held;
    uint64_t rmap_mm_owners;
    /* Whether two of the frame's rmap holders name the same (root, va). If they
     * do, the reverse map double-counted one mapping and holders>owners is the
     * detector over-counting, not a lost owner reference; if not, three distinct
     * address-space mappings really do share a frame owners says two of - a real
     * leak. Plus the first holder, for context. Pins which of the two it is. */
    uint64_t rmap_mm_dup;
    uint64_t rmap_mm_h0_root;
    uint64_t rmap_mm_h0_va;

    /* Compaction. The first two say it works; the four refusals say what it
     * could not take and why - so "compaction did nothing" and "compaction was
     * not allowed to do anything" stay different numbers, which is the same
     * distinction skipped_no_swap exists for one layer up. */
    uint64_t compact_moved;
    uint64_t compact_mappings_moved;
    uint64_t compact_refused_pinned;
    uint64_t compact_refused_writable;
    uint64_t compact_refused_many;
    uint64_t compact_refused_raced;
    uint64_t compact_refused_untracked;  /* a reference that is not a mapping */

    /* Swap. The refusals matter as much as the successes: a swap that cannot
     * touch shared pages will never reclaim what a forking workload
     * accumulates, and that should be a number rather than a silence. */
    uint64_t swap_refused_pinned;
    uint64_t swap_refused_shared;
    uint64_t swap_write_failed;
    uint64_t swap_read_failed;
    /* A swap-out abandoned because the entry changed while its page was being
     * written - its final exchange lost (M-056) - and, of those, how many were
     * a store cancelling it. Not failures: this is the protocol working. Zero
     * for ever would say nothing races with reclaim, or that the exchange is
     * not there. */
    uint64_t swap_out_raced;
    uint64_t swap_out_cancelled;

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
    uint64_t tlb_targets;        /* cores asked to flush, one per target      */
    uint64_t tlb_flushed;        /* ...and seen to have flushed. EQUAL        */
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
    uint64_t swap_dropped;        /* swapped entries munmap/teardown let go   */
    uint64_t fork_swapped_in;     /* pages fork brought back to share them    */
    uint64_t fork_swapped_failed; /* ...and could not: the fork failed        */
    uint64_t swap_read_checked;   /* page-ins compared with what was written   */
    uint64_t swap_read_mismatch;  /* ...that differed. MUST BE ZERO            */
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
