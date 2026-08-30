/* What memory is being used for, right now.
 *
 * The counters in stats.c say what has happened - how many frames were mapped,
 * how many copies fork forced. This answers the other question, the one a
 * person actually asks when a machine is behaving oddly: what is in memory at
 * this moment, and who has it. It is the shape of what RAMMap shows on Windows,
 * and of the memory column in a task manager.
 *
 * Two rules shape the implementation:
 *
 *   - No allocation. The caller supplies the buffer. This has to work from a
 *     panic, where a heap is the last thing to trust, and from a console
 *     command on a machine that may be out of memory - which is exactly when
 *     somebody wants to look.
 *   - No guessing. Figures the current kernel cannot answer report zero, and
 *     the console says which ones those are. A memory tool that invents a
 *     number is worse than one that admits a gap, because the invented number
 *     is the one somebody will act on.
 *
 * P0 fills in what exists today: the frame table does not exist yet, so the
 * breakdown comes from the allocator and the per-process view is empty. P1
 * gives it the state histogram, P2 the per-process figures, P4 the cache line.
 * The interface does not change when they arrive - that is the point of writing
 * it now rather than later.
 */

#include "vibeos/mm_model.h"
#include "vibeos/frame.h"

/* Provided by the architecture layer, which owns the allocator today. Weak, so
 * the host test binary links without dragging in the kernel. */
__attribute__((weak)) uint64_t vibeos_mm_bytes_total(void) { return 0; }
__attribute__((weak)) uint64_t vibeos_mm_bytes_free(void) { return 0; }
__attribute__((weak)) uint64_t vibeos_mm_bytes_reserved(void) { return 0; }
__attribute__((weak)) int vibeos_mm_walk_processes(vibeos_mm_process_usage_t *out,
                                                   uint32_t max) {
    (void)out;
    (void)max;
    return 0;
}

const char *vibeos_frame_state_name(vibeos_frame_state_t state) {
    switch (state) {
        case VIBEOS_FRAME_FREE:       return "free";
        case VIBEOS_FRAME_ALLOCATED:  return "allocated";
        case VIBEOS_FRAME_RESERVED:   return "reserved";
        case VIBEOS_FRAME_PAGE_TABLE: return "page-table";
        case VIBEOS_FRAME_CACHE:      return "cache";
        default:                      return "unknown";
    }
}

int vibeos_mm_usage(vibeos_mm_usage_t *out) {
    unsigned i;

    if (!out) {
        return -1;
    }
    for (i = 0; i < (unsigned)VIBEOS_FRAME_STATE_COUNT; i++) {
        out->frames_by_state[i] = 0;
    }
    out->bytes_total = vibeos_mm_bytes_total();
    out->bytes_free = vibeos_mm_bytes_free();
    out->bytes_reserved = vibeos_mm_bytes_reserved();

    /* Everything below needs the frame table (P1) or the address-space layer
     * (P2). Reported as zero, and the console prints them as "not yet
     * measured" rather than as a figure somebody might believe. */
    out->bytes_kernel = 0;
    out->bytes_user = 0;
    out->bytes_shared = 0;
    out->bytes_cache = 0;

    /* Counted from the frame table, in one walk, so the states partition the
     * total exactly.
     *
     * They used to be derived: allocated was total minus free, which quietly
     * swallowed every reserved frame, and reserved came from a different source
     * again - so the three numbers added up to more than the machine had. The
     * frame table can be asked directly now, and one source that can be wrong
     * beats three that can disagree. */
    vibeos_frame_survey(out->frames_by_state, &out->largest_free_run);

    out->processes = 0;
    return 0;
}

int vibeos_mm_process_usage(vibeos_mm_process_usage_t *out, uint32_t max) {
    if (!out || max == 0u) {
        return -1;
    }
    return vibeos_mm_walk_processes(out, max);
}
