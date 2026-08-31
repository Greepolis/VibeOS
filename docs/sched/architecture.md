# Tasks and Scheduling: Architecture

Three layers, the same shape as the memory manager's: the portable ones can be
host-tested, and the architecture layer is what is left when everything that is
not machine-specific has been taken out of it.

```
  S2  lifetime      exit, reap, exec, fork/clone; who owns an address space
  S1  run queue     which task runs next, per CPU
  S0  task table    slots, states, tenancy; the state machine
```

Today all three are one file with the interrupt controllers and the console.

## S0 — The task table

Slots, their states, and the one function that changes them.

```c
typedef enum {
    VIBEOS_TASK_FREE = 0,   /* nobody's; may be allocated                  */
    VIBEOS_TASK_SETUP,      /* allocated, being filled in; not schedulable */
    VIBEOS_TASK_READY,      /* schedulable                                 */
    VIBEOS_TASK_RUNNING,    /* on a CPU                                    */
    VIBEOS_TASK_BLOCKED,    /* waiting on something                        */
    VIBEOS_TASK_ZOMBIE,     /* dead, not yet reaped                        */
    VIBEOS_TASK_STATE_COUNT
} vibeos_task_state_t;

int  vibeos_task_transition(uint32_t slot, vibeos_task_state_t to,
                            const char *why);
```

**The transition table is the design.** A two-dimensional array of legal moves,
checked on every change, with the name of the caller recorded. Today a state is
a field several functions assign to, and the illegal transitions are the ones
that produced the wedge: RUNNING to FREE without passing through ZOMBIE, or
ZOMBIE to FREE while the slot was still being written.

**Tenancy.** A slot carries a generation number that increases every time it is
allocated. A reference to a task is the pair (slot, generation), so a stale
reference is detectable instead of silently naming whoever holds the slot now.
`hw_task_t` already has `alloc_seq` for this - it was added after a bug and is
checked in two places out of the dozen that need it.

**Publish last.** The rule the reaper and exit each learned separately: every
field a slot owns is released and cleared *before* the slot becomes
`VIBEOS_TASK_FREE`. Making it the last statement of one function is the whole
mechanism.

## S1 — The run queue

Which task runs next on this CPU. A data structure, and therefore portable:

```c
void vibeos_runq_init(vibeos_runq_t *q, uint32_t cpus);
void vibeos_runq_add(vibeos_runq_t *q, uint32_t cpu, uint32_t slot);
void vibeos_runq_remove(vibeos_runq_t *q, uint32_t slot);
int  vibeos_runq_pick(vibeos_runq_t *q, uint32_t cpu);   /* slot, or -1 */
```

Round-robin over ready tasks, with a per-CPU idle task as the answer when there
are none - which is what the current `hw_pick_next` does, extracted so it can be
tested without a machine. The value of moving it is not the algorithm; it is
that "two CPUs picked the same task", a defect this project has already had, is
a unit test rather than a boot.

**Nothing under the lock.** The scheduler lock protects the queue and the state
field, and nothing else may happen while it is held: no allocation, no page
table walk, no disk. `hw_spin_lock` masks interrupts, so anything slow under it
is slow with the timer off - the same rule the FAT read learned the hard way.

## S2 — Lifetime

The transitions that mean something: fork, clone, exec, exit, reap. This is
where the address-space question lives.

```c
int vibeos_task_exit(uint32_t slot, int status);
int vibeos_task_reap(uint32_t parent, uint32_t child, int *out_status);
```

**One question about address spaces, asked in one place.** `execve` and
`hw_task_exit` both need to know "is anybody else running on these tables?" and
for months only one of them asked. `hw_aspace_shared_by_other` now exists and
both use it; the layer boundary is what stops a third caller appearing that does
not.

**Ordering, which is the whole subsystem.** Two rules, both learned from
failures documented in CLAUDE.md:

- *Tear down before announcing.* A dying task's address space is destroyed
  before the slot is published as ZOMBIE, because a parent may reap the instant
  it sees one.
- *Take what you need first, publish last.* Anything the kernel reads out of a
  task must be read before the slot becomes reusable.

These are stated here so a future change can be checked against them rather than
against somebody's memory of why the machine used to stop without a word.

## What stays in the architecture layer

Context switching, CR3 loading, the per-CPU area, the TSS and the kernel stacks,
the guard in `hw_task_load_cpu_state`. Those are machine-specific and belong
where they are - the point of the split is that they stop being mixed with the
question of what a task *is*.
