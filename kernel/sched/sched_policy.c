/* Which of several correct choices to make. Phase S-P5 of docs/sched/.
 *
 * A pure function of state the caller supplies. No task table, no lock, no
 * clock - so every rule here is decided by a host test rather than by watching
 * a boot and forming an impression, which is the only way a policy can be
 * argued about at all.
 */

#include "vibeos/sched_policy.h"

typedef struct {
    uint64_t vruntime;
    uint32_t cpu_mask;      /* 0 means anywhere */
    uint16_t weight;
    uint8_t cls;
    uint8_t present;
    int8_t nice;
} policy_task_t;

static policy_task_t g_task[VIBEOS_SCHED_MAX_SLOTS];
static uint32_t g_slots;

/* nice -> weight, as a table rather than a formula.
 *
 * A formula would have to be commented with an example anyway, and the property
 * that matters - each step of nice is about 25% more or less CPU, so the range
 * spans roughly a factor of a thousand - is readable here and is not readable
 * in an expression. The numbers are Linux's, because the shape is well
 * understood and inventing a different curve would mean inventing the argument
 * for it too.
 */
static const uint16_t g_weight[40] = {
    /* -20 */ 8875, 7100, 5680, 4544, 3635,
    /* -15 */ 2908, 2326, 1861, 1489, 1191,
    /* -10 */  953,  762,  610,  488,  390,
    /*  -5 */  312,  250,  200,  160,  128,
    /*   0 */  102,   82,   66,   52,   42,
    /*   5 */   33,   27,   21,   17,   14,
    /*  10 */   11,    9,    7,    6,    5,
    /*  15 */    4,    3,    2,    2,    1,
};

/* The reference: nice 0. A tick charged to a nice-0 task advances its virtual
 * time by exactly one tick, which keeps the units readable. */
#define WEIGHT_BASE 102u

/* Virtual time is fixed point, and it has to be.
 *
 * The first version advanced it by (ticks * WEIGHT_BASE) / weight in whole
 * units. For any task more favoured than nice 0 the weight exceeds the base, so
 * that division truncates to zero: a nice -5 task accumulated no virtual time
 * at all, always held the smallest, and ran forever. Not a rounding error - a
 * total starvation of everything else, and the host test found it on its first
 * run.
 *
 * Ten bits of fraction makes the smallest step (nice -20, weight 8875) come out
 * at 11 rather than 0, and the largest (nice 19, weight 1) at 104448 - so a
 * hundred thousand ticks stays four orders of magnitude clear of a 64-bit
 * overflow. */
#define VRUNTIME_SCALE 1024ull

uint32_t vibeos_sched_policy_quantum(vibeos_sched_class_t cls) {
    switch (cls) {
        /* Short: a kernel task is expected to finish or block quickly, and
         * holding a core while it does not is worse than switching. */
        case VIBEOS_SCHED_KERNEL: return 2u;
        /* Long enough that the switch is not most of the work. At 100 Hz this
         * is 50 ms, which is a scheduling decision and now says so. */
        case VIBEOS_SCHED_NORMAL: return 5u;
        /* An idle task is preempted the moment anything else is runnable, so
         * its quantum only bounds how long a core stays idle when work has
         * appeared but no timer has fired. */
        case VIBEOS_SCHED_IDLE:   return 1u;
        default:                  return 5u;
    }
}

static int slot_ok(uint32_t slot) {
    return slot < g_slots && g_task[slot].present;
}

int vibeos_sched_policy_init(uint32_t slots) {
    uint32_t i;

    if (slots == 0u || slots > VIBEOS_SCHED_MAX_SLOTS) {
        return -1;
    }
    for (i = 0; i < VIBEOS_SCHED_MAX_SLOTS; i++) {
        g_task[i].vruntime = 0;
        g_task[i].cpu_mask = 0;
        g_task[i].weight = WEIGHT_BASE;
        g_task[i].cls = (uint8_t)VIBEOS_SCHED_NORMAL;
        g_task[i].present = 0;
        g_task[i].nice = 0;
    }
    g_slots = slots;
    return 0;
}

/* A new task starts at the smallest virtual time in its class, not at zero.
 *
 * Starting at zero would hand every newly created task the whole machine until
 * it caught up with everybody else - which is not a subtle unfairness, it is a
 * fork bomb that needs no malice. Starting level is the standard answer and it
 * is the one property of admission worth testing. */
static uint64_t class_floor(uint8_t cls) {
    uint64_t lowest = 0;
    int found = 0;
    uint32_t i;

    for (i = 0; i < g_slots; i++) {
        if (!g_task[i].present || g_task[i].cls != cls) {
            continue;
        }
        if (!found || g_task[i].vruntime < lowest) {
            lowest = g_task[i].vruntime;
            found = 1;
        }
    }
    return lowest;
}

int vibeos_sched_policy_admit(uint32_t slot, vibeos_sched_class_t cls, int nice,
                        uint32_t cpu_mask) {
    uint64_t floor;

    if (slot >= g_slots || (uint32_t)cls >= (uint32_t)VIBEOS_SCHED_CLASS_COUNT) {
        return -1;
    }
    if (nice < VIBEOS_NICE_MIN || nice > VIBEOS_NICE_MAX) {
        return -1;
    }
    floor = class_floor((uint8_t)cls);
    g_task[slot].cls = (uint8_t)cls;
    g_task[slot].nice = (int8_t)nice;
    g_task[slot].weight = g_weight[nice - VIBEOS_NICE_MIN];
    g_task[slot].cpu_mask = cpu_mask;
    g_task[slot].vruntime = floor;
    g_task[slot].present = 1;
    return 0;
}

void vibeos_sched_policy_forget(uint32_t slot) {
    if (slot < g_slots) {
        g_task[slot].present = 0;
    }
}

int vibeos_sched_policy_set_nice(uint32_t slot, int nice) {
    if (!slot_ok(slot) || nice < VIBEOS_NICE_MIN || nice > VIBEOS_NICE_MAX) {
        return -1;
    }
    /* The weight changes; the virtual time does not. Re-basing it would let a
     * task reset its own history by renicing, which is a way to get the machine
     * for free. */
    g_task[slot].nice = (int8_t)nice;
    g_task[slot].weight = g_weight[nice - VIBEOS_NICE_MIN];
    return 0;
}

int vibeos_sched_policy_nice(uint32_t slot) {
    return slot_ok(slot) ? (int)g_task[slot].nice : 0;
}

int vibeos_sched_policy_set_affinity(uint32_t slot, uint32_t cpu_mask) {
    if (!slot_ok(slot)) {
        return -1;
    }
    g_task[slot].cpu_mask = cpu_mask;
    return 0;
}

void vibeos_sched_policy_charge(uint32_t slot, uint64_t ticks) {
    if (!slot_ok(slot) || ticks == 0ull) {
        return;
    }
    /* Weighted: a favourable nice makes a tick count for less, so a favoured
     * task's virtual time advances more slowly and the picker returns to it
     * sooner. That is the whole of the fairness rule; everything else is
     * bookkeeping around it. */
    g_task[slot].vruntime += (ticks * (uint64_t)WEIGHT_BASE * VRUNTIME_SCALE) /
                             (uint64_t)g_task[slot].weight;
}

static int may_run_here(uint32_t slot, uint32_t cpu) {
    uint32_t mask = g_task[slot].cpu_mask;
    return mask == 0u || (mask & (1u << (cpu & 31u))) != 0u;
}

int vibeos_sched_policy_pick(uint32_t cpu, uint64_t runnable) {
    uint32_t i;
    int best = -1;

    for (i = 0; i < g_slots; i++) {
        if ((runnable & (1ull << i)) == 0ull) {
            continue;
        }
        /* A slot the policy has never been told about is skipped even though
         * the caller says it is runnable. Scheduling something it knows nothing
         * about would mean charging it nothing and preferring it forever. */
        if (!g_task[i].present || !may_run_here(i, cpu)) {
            continue;
        }
        if (best < 0) {
            best = (int)i;
            continue;
        }
        /* Class first, and absolutely: a runnable kernel task runs before any
         * normal one, however long the normal one has waited. That is what
         * separates a class from a large weight, and it is why the starvation
         * bound below is stated per class rather than across the machine. */
        if (g_task[i].cls != g_task[best].cls) {
            if (g_task[i].cls < g_task[best].cls) {
                best = (int)i;
            }
            continue;
        }
        if (g_task[i].vruntime < g_task[best].vruntime) {
            best = (int)i;
        }
    }
    return best;
}

uint64_t vibeos_sched_policy_vruntime(uint32_t slot) {
    return slot_ok(slot) ? g_task[slot].vruntime : 0ull;
}

uint64_t vibeos_sched_policy_max_lag(uint64_t runnable) {
    uint64_t worst = 0;
    uint32_t cls;

    for (cls = 0; cls < (uint32_t)VIBEOS_SCHED_CLASS_COUNT; cls++) {
        uint64_t lowest = 0, highest = 0;
        int found = 0;
        uint32_t i;

        for (i = 0; i < g_slots; i++) {
            if ((runnable & (1ull << i)) == 0ull || !g_task[i].present ||
                g_task[i].cls != (uint8_t)cls) {
                continue;
            }
            if (!found || g_task[i].vruntime < lowest) { lowest = g_task[i].vruntime; }
            if (!found || g_task[i].vruntime > highest) { highest = g_task[i].vruntime; }
            found = 1;
        }
        if (found && highest - lowest > worst) {
            worst = highest - lowest;
        }
    }
    return worst;
}
