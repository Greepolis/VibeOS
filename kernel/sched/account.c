/* What each task actually costs. Phase S-P4 of docs/sched/.
 *
 * Deliberately arithmetic and nothing else. It makes no decision, holds no
 * lock, and calls nothing - so it can be host-tested exhaustively, and so a
 * defect in it can never be a defect in scheduling.
 */

#include "vibeos/account.h"

static vibeos_task_account_t g_task[VIBEOS_ACCOUNT_MAX_SLOTS];
static uint64_t g_idle[VIBEOS_ACCOUNT_MAX_CPUS];
static uint64_t g_seen;          /* ticks this layer was told about */
static uint64_t g_charged;       /* of those, charged to a task     */
static uint64_t g_idle_total;    /* of those, charged to idleness   */
static uint64_t g_dropped;
static uint32_t g_slots;
static uint32_t g_cpus;

/* No lock, and the reason is a property of the caller rather than of the data.
 *
 * A tick is charged by the core that took it, to the task that core is running.
 * Two cores never run one task - that is what the lifetime layer's `on_cpu`
 * exists to guarantee, and it counts the violation if it ever fails - so two
 * cores never touch one task's counters. Idle is per-CPU by construction.
 *
 * The three totals are the exception: every core adds to them, so they are
 * atomic. The first version left them plain and said a lost increment "costs
 * one tick", which was wrong about the consequence - it costs the identity, and
 * the identity is this layer's entire promise. Twelve hundred atomic adds a
 * second on four cores is nothing.
 */

static void bump(uint64_t *counter) {
    (void)__atomic_fetch_add(counter, 1ull, __ATOMIC_RELAXED);
}

int vibeos_account_init(uint32_t slots, uint32_t cpus) {
    uint32_t i;

    if (slots == 0u || slots > VIBEOS_ACCOUNT_MAX_SLOTS ||
        cpus == 0u || cpus > VIBEOS_ACCOUNT_MAX_CPUS) {
        return -1;
    }
    for (i = 0; i < VIBEOS_ACCOUNT_MAX_SLOTS; i++) {
        g_task[i].ticks = 0;
        g_task[i].switches = 0;
        g_task[i].last_ran = 0;
        g_task[i].max_wait = 0;
    }
    for (i = 0; i < VIBEOS_ACCOUNT_MAX_CPUS; i++) {
        g_idle[i] = 0;
    }
    g_seen = 0;
    g_dropped = 0;
    g_charged = 0;
    g_idle_total = 0;
    g_slots = slots;
    g_cpus = cpus;
    return 0;
}

void vibeos_account_tick(uint32_t cpu, int slot, int slot_is_idle) {
    if (g_slots == 0u) {
        return;
    }
    if (cpu >= g_cpus) {
        /* Counted, not dropped silently. There is no idle counter to charge and
         * inventing one would be worse - but a tick that simply vanishes leaves
         * the balance intact and the picture wrong, which is the one failure
         * this layer must not be able to hide. */
        bump(&g_dropped);
        return;
    }
    bump(&g_seen);

    /* An idle task is a task, and it must not read as work.
     *
     * This kernel gives every core an idle task with a real slot, so charging
     * by slot alone would report a machine that is doing nothing as a machine
     * that is fully busy - and every ratio built on that would be meaningless
     * in the same direction. */
    if (slot < 0 || slot >= (int)g_slots || slot_is_idle) {
        g_idle[cpu]++;   /* per-core: only this core writes it */
        bump(&g_idle_total);
        return;
    }
    g_task[slot].ticks++;   /* one core runs one task */
    bump(&g_charged);
}

void vibeos_account_switch(uint32_t cpu, int slot, uint64_t ready_since) {
    (void)cpu;
    if (g_slots == 0u || slot < 0 || slot >= (int)g_slots) {
        return;
    }
    g_task[slot].switches++;
    g_task[slot].last_ran = g_seen;

    /* The longest a task waited between being runnable and running is the one
     * number that says whether anybody is being starved, and it is the number
     * S-P5's policy will be judged against. Recorded here because this is the
     * only moment both halves are known.
     *
     * A ready_since of zero means the caller did not know, which records no
     * wait rather than a wait of everything since boot. */
    if (ready_since != 0ull && g_seen > ready_since) {
        uint64_t waited = g_seen - ready_since;
        if (waited > g_task[slot].max_wait) {
            g_task[slot].max_wait = waited;
        }
    }
}

const vibeos_task_account_t *vibeos_account_task(uint32_t slot) {
    if (slot >= g_slots) {
        return 0;
    }
    return &g_task[slot];
}

uint64_t vibeos_account_idle(uint32_t cpu) {
    return (cpu < g_cpus) ? g_idle[cpu] : 0ull;
}

uint64_t vibeos_account_ticks(void) {
    return g_seen;
}

uint64_t vibeos_account_dropped(void) {
    return g_dropped;
}

int vibeos_account_balance(uint64_t *out_charged, uint64_t *out_idle,
                           uint64_t *out_seen) {
    uint64_t seen = __atomic_load_n(&g_seen, __ATOMIC_RELAXED);
    uint64_t charged = __atomic_load_n(&g_charged, __ATOMIC_RELAXED);
    uint64_t idle = __atomic_load_n(&g_idle_total, __ATOMIC_RELAXED);
    uint64_t parts = charged + idle;
    uint64_t skew = (parts > seen) ? (parts - seen) : (seen - parts);

    if (out_charged) { *out_charged = charged; }
    if (out_idle)    { *out_idle = idle; }
    if (out_seen)    { *out_seen = seen; }

    /* Not an identity while the machine is running, and claiming otherwise was
     * the first version's mistake.
     *
     * A tick increments the total and then one of the two parts, so a core
     * caught between those two adds makes the parts one short. Reading in the
     * other order just moves the discrepancy to the other side; no read order
     * fixes it, because the reader is not atomic with respect to four writers.
     *
     * What *is* true is that at most one tick per core can be mid-update, so
     * the two sides agree to within the number of cores. That is the bound the
     * plan asked for, and asserting an exact match instead turned a correct
     * kernel red twice in twelve boots - a check reporting healthy behaviour,
     * which is a check people learn to ignore.
     *
     * Anything larger than that bound is a tick charged twice or lost, which no
     * amount of concurrency explains. */
    return (skew <= (uint64_t)g_cpus) ? 0 : -1;
}
