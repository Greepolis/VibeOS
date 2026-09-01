/* Exec refusal counters. Phase X-P0 of docs/exec/.
 *
 * No lock, for the reason the task and memory counters have none: these are
 * counters, not state. A lost increment under contention costs a number that is
 * one too low. What must not be lost is the *line* that accompanies it, and
 * that is written under the console lock by the caller.
 */

#include "vibeos/exec_stats.h"

static vibeos_exec_stats_t g_stats;

vibeos_exec_stats_t *vibeos_exec_stats(void) {
    return &g_stats;
}

void vibeos_exec_stats_reset(void) {
    uint64_t *p = (uint64_t *)(void *)&g_stats;
    uint32_t i;

    for (i = 0; i < sizeof(g_stats) / sizeof(uint64_t); i++) {
        p[i] = 0;
    }
}

const char *vibeos_exec_fail_name(vibeos_exec_fail_t why) {
    switch (why) {
        case VIBEOS_EXEC_OK:              return "ok";
        case VIBEOS_EXEC_NOT_FOUND:       return "not-found";
        case VIBEOS_EXEC_SHORT_READ:      return "short-read";
        case VIBEOS_EXEC_BAD_HEADER:      return "bad-header";
        case VIBEOS_EXEC_BAD_WINDOW:      return "bad-window";
        case VIBEOS_EXEC_NO_INTERP:       return "no-interpreter";
        case VIBEOS_EXEC_INTERP_CHAIN:    return "interpreter-chain";
        case VIBEOS_EXEC_NO_STAGING:      return "no-staging-buffer";
        case VIBEOS_EXEC_TOO_LARGE:       return "image-too-large";
        case VIBEOS_EXEC_NO_MEMORY:       return "no-memory";
        case VIBEOS_EXEC_ARGS_TOO_LARGE:  return "args-too-large";
        case VIBEOS_EXEC_NO_ASPACE:       return "no-address-space";
        default:                          return "?";
    }
}

const char *vibeos_exec_refuse(vibeos_exec_fail_t why) {
    if ((uint32_t)why >= (uint32_t)VIBEOS_EXEC_REASON_COUNT) {
        return "?";
    }
    g_stats.refused[why]++;
    return vibeos_exec_fail_name(why);
}
