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
#include "vibeos/vmspace.h"
#include "vibeos/mm_stats.h"

static uint64_t g_low;
static uint64_t g_min;
static int      g_marks_set;

static vibeos_reclaim_stats_t g_stats;
static vibeos_reclaim_drop_fn g_drop_clean;
static vibeos_reclaim_drop_fn g_drop_anon;
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

void vibeos_reclaim_set_anon_source(vibeos_reclaim_drop_fn fn) {
    g_drop_anon = fn;
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

/* ---- compaction ---------------------------------------------------------- */

static uint64_t g_region_base;
static uint32_t g_region_frames;

void vibeos_reclaim_set_region(uint64_t base_phys, uint32_t frames) {
    g_region_base = base_phys;
    g_region_frames = frames;
}

static uint64_t frame_at(uint32_t i) {
    return g_region_base + (uint64_t)i * 4096ull;
}

static int frame_is_free(uint32_t i) {
    return vibeos_frame_state(frame_at(i)) == VIBEOS_FRAME_FREE;
}

/* The largest run of free frames, measured here rather than asked of the frame
 * layer, because this walk is already happening and a second one under a
 * different lock could disagree with it. */
static uint32_t largest_run(void) {
    uint32_t i, run = 0, best = 0;

    for (i = 0; i < g_region_frames; i++) {
        if (frame_is_free(i)) {
            if (++run > best) {
                best = run;
            }
        } else {
            run = 0;
        }
    }
    return best;
}

uint32_t vibeos_reclaim_compact(uint32_t want) {
    uint32_t i, best_start = 0, best_cost = 0xFFFFFFFFu;
    uint32_t moved = 0;

    if (want == 0u || g_region_frames == 0u || want > g_region_frames) {
        return largest_run();
    }

    /* Cheapest window first.
     *
     * "Cheapest" is the number of frames in the way, not their size - every
     * frame costs one copy. Scanning for the minimum rather than taking the
     * first window that could work matters more than it looks: the first
     * suitable window is often one occupied by long-lived pages, and moving
     * those is both the most work and the most likely to be refused. */
    for (i = 0; i + want <= g_region_frames; i++) {
        uint32_t j, cost = 0;

        for (j = 0; j < want; j++) {
            if (!frame_is_free(i + j)) {
                cost++;
            }
        }
        if (cost == 0u) {
            return largest_run();   /* already a run this long; nothing to do */
        }
        if (cost < best_cost) {
            best_cost = cost;
            best_start = i;
        }
    }

    /* Empty the window into free frames outside it.
     *
     * A target taken from inside the window would be undone by the next move,
     * and a run of moves that chase each other around one window is the shape
     * of a compactor that appears to work and never finishes. */
    for (i = 0; i < want; i++) {
        uint32_t src = best_start + i;
        uint32_t k;

        if (frame_is_free(src)) {
            continue;
        }
        for (k = 0; k < g_region_frames; k++) {
            if (k >= best_start && k < best_start + want) {
                continue;   /* inside the window */
            }
            if (!frame_is_free(k)) {
                continue;
            }
            if (vibeos_vmspace_move_frame(frame_at(src), frame_at(k)) == 0) {
                moved++;
            }
            break;   /* one attempt per frame: a refusal is about the source */
        }
    }

    vibeos_reclaim_stats()->compact_runs++;
    vibeos_reclaim_stats()->compact_frames_moved += moved;
    return largest_run();
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

    /* Tier two: anonymous pages, to swap.
     *
     * Second because the order is by cost and this one costs a write. It is
     * only tried for the shortfall - a reclaim that took from both tiers when
     * the first had already satisfied it would be paying for pages nobody
     * asked for.
     *
     * The source is absent unless a swap area exists, and that absence is the
     * honest state rather than a gap: a kernel with nowhere to write an
     * anonymous page cannot reclaim one. */
    if (freed < want && g_drop_anon) {
        uint32_t n = g_drop_anon(want - freed);

        freed += n;
        g_stats.freed_anon += n;
    }

    /* Whatever is still missing had nowhere to go.
     *
     * Counted rather than left silent, so "reclaim did nothing" and "reclaim
     * had nothing it was allowed to take" are different numbers. The first is
     * a defect; the second is the machine being honest about its scope, and a
     * gate that could not tell them apart would be satisfied by a reclaim that
     * had quietly stopped working. Dirty page-cache write-back is the tier
     * that is still missing entirely. */
    if (freed < want) {
        g_stats.skipped_no_swap += (uint64_t)(want - freed);
    }
    return freed;
}
