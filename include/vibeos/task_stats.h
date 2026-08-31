#ifndef VIBEOS_TASK_STATS_H
#define VIBEOS_TASK_STATS_H

#include <stdint.h>

/* What the task subsystem has done, and what a task currently is.
 *
 * Phase S-P0 of docs/sched/. This lands before anything is restructured, for
 * the reason the memory manager's equivalent did: its counters were what turned
 * "something is wrong with memory" into "this frame, this process, this
 * operation", and every hour saved afterwards was bought by the two spent
 * building them first.
 *
 * The task subsystem has none of that today. It has three provenance fields -
 * cr3_set_by, ready_by, aspace_killed_by - each added after a defect had
 * already cost a session, and no way at all to see the task table on a running
 * machine.
 */

typedef struct vibeos_task_stats {
    /* Lifetime, counted where it happens. */
    uint64_t created;            /* slots handed out                          */
    uint64_t exited;             /* tasks that reached exit                   */
    uint64_t reaped;             /* zombies collected by a parent             */
    uint64_t forks;
    uint64_t threads;            /* clone sharing an address space            */
    uint64_t execs;

    /* Refusals. A number that grows without bound is a question somebody
     * should ask, and a refusal that is invisible is a hang somebody will
     * investigate from the wrong end. */
    uint64_t slot_refused;       /* no free task slot                         */

    /* The three that must be zero, and are asserted by the boot gate.
     *
     * Each names a defect this subsystem has actually produced: a slot written
     * to after being published as reusable, a stale reference used as if it
     * still named its task, and a task scheduled onto page tables somebody
     * else had freed. They are counters rather than panics because a count can
     * be asserted across a whole boot, and a panic can only be met once. */
    uint64_t illegal_transition; /* a state change the table does not allow  */
    uint64_t use_after_publish;  /* a write to a slot already published FREE  */
    uint64_t tenancy_mismatch;   /* a reference whose generation had moved on */
    uint64_t cr3_without_owner;  /* a task about to run on unowned tables     */
} vibeos_task_stats_t;

vibeos_task_stats_t *vibeos_task_stats(void);
void vibeos_task_stats_reset(void);

/* One task, as something outside the architecture layer can print.
 *
 * Deliberately a copy rather than a pointer into the table: the console prints
 * this while other cores are creating and destroying tasks, and a pointer would
 * be a reference to a slot that can be recycled mid-line. */
typedef struct vibeos_task_desc {
    uint32_t slot;
    uint32_t generation;         /* alloc_seq: the slot's tenancy             */
    uint32_t state;
    uint32_t pid;
    uint32_t tgid;
    uint32_t ppid;
    int      is_user;
    int      is_thread;
    int      on_cpu;
    uint64_t cr3;
    const char *state_name;
    /* Provenance. Each of these exists because a defect was diagnosed by not
     * having it. */
    const char *cr3_set_by;
    const char *ready_by;
    const char *aspace_killed_by;
    char exe[64];
} vibeos_task_desc_t;

/* How many slots the table has, and a snapshot of one. `describe` returns 0 on
 * success; a free slot is described rather than skipped, because "which slots
 * are free" is part of what somebody reading this wants to know. */
uint32_t vibeos_task_slots(void);
int vibeos_task_describe(uint32_t slot, vibeos_task_desc_t *out);

/* Printed by kernel/sched/view.c. Deciding what to say about a task is
 * portable; only reading the machine's table is not. */
void vibeos_task_print_table(void);
void vibeos_task_print_stats(void);

#endif /* VIBEOS_TASK_STATS_H */
