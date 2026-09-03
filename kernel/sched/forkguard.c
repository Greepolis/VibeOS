/* May this task create another one? Phase S-P6 of docs/sched/.
 *
 * A decision and nothing else: told the counts, returns a verdict. Which means
 * the rules are settled by host tests instead of by writing a fork bomb and
 * hoping it reproduces.
 */

#include "vibeos/forkguard.h"

static vibeos_forkguard_stats_t g_stats;
static uint32_t g_total;
static uint32_t g_reserved;
static uint32_t g_max_children;

int vibeos_forkguard_init(uint32_t slots_total, uint32_t reserved,
                          uint32_t max_children) {
    if (slots_total == 0u || max_children == 0u) {
        return -1;
    }
    /* A reserve that swallows the table would refuse every unprivileged fork,
     * which is not a guard - it is an outage with a rationale. */
    if (reserved >= slots_total) {
        return -1;
    }
    g_total = slots_total;
    g_reserved = reserved;
    g_max_children = max_children;
    g_stats.allowed = 0;
    g_stats.refused_no_slots = 0;
    g_stats.refused_reserved = 0;
    g_stats.refused_children = 0;
    return 0;
}

vibeos_fork_verdict_t vibeos_forkguard_check(uint32_t slots_in_use,
                                             uint32_t requester_children,
                                             int privileged) {
    if (g_total == 0u) {
        /* Not configured: allow, and say nothing. A guard that has not been set
         * up must not silently become a policy of its own - the machine
         * behaves as it did before this file existed. */
        return VIBEOS_FORK_OK;
    }

    if (slots_in_use >= g_total) {
        g_stats.refused_no_slots++;
        return VIBEOS_FORK_NO_SLOTS;
    }

    /* The ceiling is checked before the floor, and the order is deliberate: a
     * task that has already had its share should be told *that*, not told the
     * machine is full. The two produce the same errno and completely different
     * investigations. */
    if (!privileged && requester_children >= g_max_children) {
        g_stats.refused_children++;
        return VIBEOS_FORK_TOO_MANY_KIDS;
    }

    if (!privileged && slots_in_use + g_reserved >= g_total) {
        g_stats.refused_reserved++;
        return VIBEOS_FORK_RESERVED;
    }

    g_stats.allowed++;
    return VIBEOS_FORK_OK;
}

const char *vibeos_fork_verdict_name(vibeos_fork_verdict_t v) {
    switch (v) {
        case VIBEOS_FORK_OK:            return "ok";
        case VIBEOS_FORK_NO_SLOTS:      return "no-slots";
        case VIBEOS_FORK_RESERVED:      return "system-reserve";
        case VIBEOS_FORK_TOO_MANY_KIDS: return "too-many-children";
        default:                        return "?";
    }
}

const vibeos_forkguard_stats_t *vibeos_forkguard_stats(void) {
    return &g_stats;
}
