/* L4: reclaim. See include/vibeos/reclaim.h for why the order is a property.
 *
 * Small on purpose. Everything this layer decides is a policy question, and
 * every fact it needs belongs to somebody else: how much is free is the frame
 * layer's, which cache pages are droppable is the cache's, and which frames
 * are pinned is the only thing kept here - because "pinned" is not a fact
 * about a frame's contents but a statement about who else is holding an
 * address for it, and nothing else in the system is in a position to know.
 */

#include "vibeos/reclaim.h"
#include "vibeos/frame.h"

static uint64_t g_low;
static uint64_t g_min;
static int      g_marks_set;

static vibeos_reclaim_stats_t g_stats;
static vibeos_reclaim_drop_fn g_drop_clean;
static uint64_t (*g_free_frames)(void);

/* The last pressure reported, so a transition can be counted once rather than
 * once per query. A counter that ticks every time somebody *asks* measures how
 * chatty the caller is, not how often the machine was in trouble. */
static vibeos_mem_pressure_t g_last;

vibeos_reclaim_stats_t *vibeos_reclaim_stats(void) {
    return &g_stats;
}

void vibeos_reclaim_set_clean_source(vibeos_reclaim_drop_fn fn) {
    g_drop_clean = fn;
}

void vibeos_reclaim_set_free_source(uint64_t (*free_frames)(void)) {
    g_free_frames = free_frames;
}

int vibeos_reclaim_set_marks(uint64_t low, uint64_t min) {
    /* A minimum at or above the low mark makes every allocation critical and
     * nothing ever runs - a machine that refuses to work rather than one that
     * survives pressure. Refused rather than clamped, because silently moving
     * somebody's number is how a configuration mistake becomes a mystery. */
    if (min >= low) {
        return -1;
    }
    g_low = low;
    g_min = min;
    g_marks_set = 1;
    g_last = VIBEOS_MEM_OK;
    return 0;
}

static uint64_t free_frames(void) {
    if (g_free_frames) {
        return g_free_frames();
    }
    return vibeos_frame_free_count();
}

vibeos_mem_pressure_t vibeos_reclaim_pressure(void) {
    uint64_t f;
    vibeos_mem_pressure_t p;

    if (!g_marks_set) {
        return VIBEOS_MEM_OK;   /* nobody has said what pressure means here */
    }
    f = free_frames();
    if (f <= g_min) {
        p = VIBEOS_MEM_CRITICAL;
    } else if (f <= g_low) {
        p = VIBEOS_MEM_LOW;
    } else {
        p = VIBEOS_MEM_OK;
    }

    if (p != g_last) {
        if (p == VIBEOS_MEM_LOW) {
            g_stats.low_events++;
        } else if (p == VIBEOS_MEM_CRITICAL) {
            g_stats.critical_events++;
        }
        g_last = p;
    }
    return p;
}

int vibeos_reclaim_admit(int privileged) {
    /* Privileged allocations go through at any level, and that is the whole
     * point of the minimum rather than a hole in it: the reserve exists so the
     * work that ends the pressure - a page table for the reclaim itself, the
     * kernel's own bookkeeping - can still allocate. A minimum that refuses
     * everybody is a machine that deadlocks precisely when it most needs to
     * free something. */
    if (privileged) {
        return 1;
    }
    if (vibeos_reclaim_pressure() == VIBEOS_MEM_CRITICAL) {
        g_stats.admit_refused++;
        return 0;
    }
    return 1;
}

/* ---- pinning ------------------------------------------------------------- */

/* Pinning is kept as a flag on the frame descriptor rather than as a list
 * here, because the question "is this frame pinned?" is asked once per
 * candidate during a scan and a list would make that a search. The flag field
 * was reserved for it at P1. */
void vibeos_reclaim_pin(uint64_t phys) {
    vibeos_frame_set_flag(phys, VIBEOS_FRAME_PINNED);
}

void vibeos_reclaim_unpin(uint64_t phys) {
    vibeos_frame_clear_flag(phys, VIBEOS_FRAME_PINNED);
}

int vibeos_reclaim_is_pinned(uint64_t phys) {
    return vibeos_frame_test_flag(phys, VIBEOS_FRAME_PINNED);
}

/* ---- the scan ------------------------------------------------------------ */

uint32_t vibeos_reclaim_run(uint32_t want) {
    uint32_t freed = 0;

    g_stats.scans++;
    if (want == 0u) {
        return 0;
    }

    /* Tier one: clean page-cache pages. Dropping one costs nothing, because
     * the file still has it and the next fault reads it back.
     *
     * Asked of the cache rather than reimplemented. The cache already knows
     * which entries are resident and how to let one go, and a second copy of
     * that knowledge here is how two structures come to disagree about the
     * same fact - which is exactly the family of defect this subsystem has
     * spent four investigations on. */
    if (g_drop_clean) {
        freed = g_drop_clean(want);
        g_stats.freed_clean += freed;
    }

    /* Tier two would be dirty cache pages written back, and tier three
     * anonymous pages to swap. Neither exists yet: there is nowhere to write.
     *
     * Counted rather than left silent, so "reclaim did nothing" and "reclaim
     * had nothing it was allowed to take" are different numbers. The first is
     * a defect; the second is this phase being honest about its scope, and a
     * gate that could not tell them apart would be satisfied by a reclaim that
     * had quietly stopped working. */
    if (freed < want) {
        g_stats.skipped_no_swap += (uint64_t)(want - freed);
    }
    return freed;
}
