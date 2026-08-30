/* Memory management counters.
 *
 * A plain structure with one accessor, and deliberately nothing more: no lock,
 * no allocation, no initialisation order to get wrong. It is read from the
 * console command, from the boot gate through the serial log, and - the reason
 * for the simplicity - from a panic path, where anything that could block or
 * fault would turn a diagnosis into a second failure.
 *
 * Counters are written from several cores without synchronisation. That is a
 * considered choice rather than an oversight: these are magnitudes for humans
 * and for assertions like "this must be zero" or "this must be non-zero", and
 * a lock on every page mapping to make a statistic exact would cost more than
 * the statistic is worth. Anything that must balance exactly - the reference
 * counts themselves - is atomic and lives in the frame layer, not here.
 */

#include "vibeos/mm_stats.h"

static vibeos_mm_stats_t g_mm_stats;

vibeos_mm_stats_t *vibeos_mm_stats(void) {
    return &g_mm_stats;
}

void vibeos_mm_stats_reset(void) {
    uint64_t *p = (uint64_t *)(void *)&g_mm_stats;
    unsigned i;

    for (i = 0; i < sizeof(g_mm_stats) / sizeof(uint64_t); i++) {
        p[i] = 0;
    }
}
