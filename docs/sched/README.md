# Tasks and Scheduling: Rewrite Plan

The same treatment the memory manager got, for the subsystem that has produced
this project's most expensive defects. Written to the same rules: the goal
stated as properties that can be proved, invariants written down before code,
layers that can be host-tested, and a decision list I do not settle on my own.

Companion documents: [architecture.md](architecture.md),
[phases.md](phases.md), [decisions.md](decisions.md).

## 1. Why this is a rewrite and not a fix

Everything about a task lives in `kernel/arch/x86_64/arch_hw.c`, which is 8312
lines and 267 static functions holding the scheduler, the task table, syscalls,
program loading, pipes, signals, the network glue, the interrupt controllers and
the serial console. There are no boundaries inside it, so any function can reach
any state, and several have.

That is not a stylistic complaint. It is the direct cause of the defects this
subsystem has produced, and they share one shape: **a task's lifetime is
managed by several pieces of code that each know part of the truth.**

- The silent wedge, roughly one boot in three, was `hw_task_exit` publishing a
  dying task as ZOMBIE before tearing its address space down. A parent reaped
  the slot, a fork on another core took it, and the late teardown freed the new
  tenant's page tables. No panic, no output - the machine simply stopped.
- The same shape in the reaper: a slot published FREE while the task was still
  being written to.
- `execve` destroyed an address space without asking whether sibling threads
  still ran on it, while `hw_task_exit` had asked that question for months. The
  two live two thousand lines apart with no interface between them, so the rule
  existed in one and not the other.
- A slot recycled with a stale `cr3` installed page tables belonging to somebody
  else. The fix was to clear four fields in the allocator and hope the list is
  complete.

Each was fixed where it was found. None of them could have been prevented,
because there is nothing that says what a task slot's states are or who may
change them.

## 2. The goal: production ready

| Property | What it means here | How it is proved |
| --- | --- | --- |
| **One owner of a task's lifetime** | a slot changes state in one place, through one function per transition | a state-machine table; every transition asserted; no direct writes to `state` outside it |
| **No use after publish** | nothing writes to a task after publishing it as reusable, and nothing reads one after it is free | tenancy counter checked on every access; a sabotage case per publish point |
| **Correct under concurrency** | a slot recycled on one core is never half-initialised on another | host tests over the state machine; the boot gate asserts no task ever runs with a cr3 it did not set |
| **Address spaces outlive their users** | tables are freed when the last task using them stops, and not before | one shared test asked by address space, used by exit and exec alike |
| **Survives exhaustion** | no free slot fails the request, never the machine | a fork-storm test; refusals counted |
| **Predictable** | picking the next task is bounded and does not hold a lock across work | no allocation, no I/O and no page-table walk under `g_sched_lock` |
| **Observable** | a running machine can say what every task is and how it got there | `tasks` on the console, per-task provenance fields, gate assertions |
| **Host-testable** | the state machine and the run queue are portable C | `tests/kernel/sched_tests.c`, no virtual machine needed |

### Invariants

1. A task slot is in exactly one state, and every change of state goes through
   one function that records who made it.
2. A slot is published as reusable **last**: after every field it owns has been
   released and cleared.
3. A task never runs on an address space that another task has released.
4. An address space is destroyed by whoever holds the last reference to it, and
   that question is asked in exactly one place.
5. Nothing under the scheduler lock allocates, frees, reads a disk or walks a
   page table.
6. Every field of a recycled slot is either reset by the allocator or provably
   written before the task runs. There is no third category.
7. A task's `cr3` names tables that exist for as long as the task can be
   scheduled.

## 3. Design goals

1. **The state machine is data, not control flow.** A table of legal
   transitions, checked, rather than assignments spread across nine functions.
2. **Tenancy, not slot index.** A slot plus a generation number identifies a
   task; a stale index is then detectable rather than silently valid.
3. **Address-space ownership is counted, not inferred.** The same lesson as the
   memory manager's `PTE_OWNED`: exit and exec ask one shared function instead
   of each reasoning about threads.
4. **The run queue is separable.** Picking the next task is a data-structure
   question and belongs in portable C where it can be tested.
5. **Policy comes last, and only after accounting.** Priorities, fairness,
   affinity and load balancing are the point of a scheduler and they are phases
   S-P4 and S-P5, not omissions - the first draft of this plan listed them as
   non-goals in one line, which was wrong. A policy chooses between correct
   options, so it needs a correct lifetime layer underneath it; and fairness
   cannot be built before something measures CPU time, which nothing here does
   today.
6. **Observable by construction.** Provenance fields (`cr3_set_by`,
   `aspace_killed_by`, `ready_by`) already exist because they were each added
   after a bug; they become part of the design instead of scar tissue.

## 4. Non-goals

- Preemption model changes at the hardware level. Timer-driven preemption stays
  as it is; what changes at S-P5 is the quantum being a named number rather than
  whatever the timer period happens to be.
- SMP bring-up, the APIC, or the interrupt path, except where they read task
  state.
- The syscall dispatch table, which is its own subsystem and its own plan.

## 5. Tracking

A macro-area row in `docs/implementation_progress.md`, a detail file under
`docs/implementation_progress/`, and `scripts/dev/make-book-summary.py` re-run,
as the project rule requires.
