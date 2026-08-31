# Tasks and Scheduling: Phases

Same rules as the memory manager's phases: each one lands with host tests and
sabotage cases before the next begins, each names what is out of scope, and each
says what "done" means in terms the phase itself controls.

**Criteria are checked against the baseline before they are used to judge
anything.** That rule is here because its absence cost two days on the memory
manager: a "48 boots with no failure" criterion was written for P2 and could
never have been met by any revision, because the machine has a pre-existing
intermittent failure of roughly one boot in eight. See
[boot_repeatability.md](../implementation_progress/boot_repeatability.md).
Every phase below is judged on its own invariants, and any boot count is a
*ratio against the same measurement taken on the parent commit*.

---

## S-P0 — Counters and the console view

**Objective.** Before anything moves, a running machine can say what its tasks
are. The memory manager did this first and it paid for itself immediately.

**Files created** — `include/vibeos/task_stats.h`, `kernel/sched/stats.c`

**Steps**
1. A stats structure: tasks created, exited, reaped, slots refused, illegal
   transitions attempted, tenancy mismatches caught.
2. A `tasks` console command: slot, generation, state, pid, tgid, cr3, and the
   provenance fields that already exist (`cr3_set_by`, `ready_by`,
   `aspace_killed_by`).
3. Gate assertions on the counters that must be zero.

**Done when.** `tasks` prints the table on a running machine; the must-be-zero
counters are asserted by the boot gate.

**Status: done** (`5d696fa`). The command prints every slot with its
generation, state, pid, tgid, ppid, cr3 and the three provenance fields; the
gate drives it and asserts four counters at zero.

---

## S-P1 — The task table and the state machine

**Objective.** A slot changes state in one place, and an illegal change is
caught rather than performed.

**Files created** — `include/vibeos/task.h`, `kernel/sched/task.c`,
`tests/kernel/task_tests.c`

**Steps, in order**
1. The state enum, the transition table, and `vibeos_task_transition`. Host
   tests first: every legal move, every illegal one refused and counted, and the
   generation increasing on each allocation.
2. Allocation and release as the only writers of `state`, with the release
   publishing FREE as its last statement.
3. Point the architecture's `g_tasks[i].state` assignments at it, one call site
   at a time.
4. Delete the direct assignments. Add a layering check: no write to `.state`
   outside `kernel/sched/`.

**Out of scope.** The run queue, exit, exec.

**Tests**
- Host: the transition table exhaustively, tenancy, publish-last ordering.
- Sabotage: `scripts/dev/cases/sched-task.txt` - allow RUNNING to FREE; publish
  the slot before clearing it; reuse a slot without bumping the generation.

**Done when.** No `.state` assignment outside the layer; illegal transitions
counted and zero on a normal boot; the sabotage cases red.

**Status: done** (`142e3fc`). All twenty-six assignments go through one
function; eleven host cases and five sabotage cases, all red.

**The table was wrong on its first boot and the machine said so.** SETUP to
RUNNING was missing: a core adopts the thread it is already executing, and an
idle task is created already on its CPU - neither passes through READY because
neither was ever waiting. The refusal named the slot, both states and the
function, which is the argument for a table over scattered assignments in one
line of output.

Storage still lives in the architecture's field; what moved is the decision
about whether a change is allowed. Moving the storage is S-P3's work.

---

## S-P2 — The run queue

**Objective.** Picking the next task is a tested data structure.

**Files created** — `kernel/sched/runq.c`, `tests/kernel/runq_tests.c`

**Steps, in order**
1. The queue as a pure structure with host tests: add, remove, pick, round-robin
   fairness, the idle fallback, and *two CPUs never picking the same slot* -
   which is a defect this project has actually had.
2. Replace `hw_pick_next`.
3. Add the layering check: nothing under `g_sched_lock` allocates, frees or
   walks page tables. Enforced by review plus a grep, as the memory manager's is.

**Done when.** `hw_pick_next` is a wrapper over the tested queue; the host
tests cover the cases; boot rate unchanged against the parent commit.

**Status: done** (`97ae443`). Seven host cases, four sabotage cases, all
red. 21 boots clean out of 24, which is the background rate, with all four task
counters zero in the failing boots as well.

**One real defect fixed in the move.** The old scan started from the *current*
task, which is fair while something is running and degenerates when the current
task is idle: the search then begins at the same place every time and the
high-numbered slots wait behind the low ones. There is a cursor per CPU now,
and the fairness case - every runnable slot must run within forty picks - is
what catches it.

**And one lesson about the shape.** The queue does not decide what may run; it
asks through a callback, so the state machine stays the single source of truth
about what READY means. The sabotage case that matters is removing the
question.

---

## S-P3 — Lifetime: exit, reap, exec

**This is the phase that repairs the defects.**

**Files created** — `kernel/sched/lifetime.c`, `tests/kernel/lifetime_tests.c`

**Steps, in order**
1. `vibeos_task_exit` and `vibeos_task_reap` in the new layer, with the two
   ordering rules from the architecture document made structural rather than
   remembered.
2. The address-space ownership question routed through the single shared test
   for exit, exec and fork's failure paths alike.
3. Fork and clone move in; the failure paths release what they allocated before
   publishing the slot.
4. Delete the old paths.

**Out of scope.** Signals, which have their own lifetime questions and their own
phase later.

**Tests**
- Host: a fake task table exercising exit-while-sibling-runs, reap racing exit,
  exec with and without siblings, fork failing at each allocation point.
- Sabotage: `scripts/dev/cases/sched-lifetime.txt` - announce before tearing
  down; skip the shared-address-space test in exec; publish a slot in fork's
  error path without destroying what it built.

**Done when.** All four ordering rules are in one file; the sabotage cases red;
the boot rate measured against the parent commit rather than against a number.

---

## S-P4 — Accounting: what each task actually costs

**Objective.** The machine can say how much CPU every task has had. Nothing
above this phase is possible without it, and none of it exists today.

**Files created** — `kernel/sched/account.c`, `tests/kernel/account_tests.c`

**Steps, in order**
1. A per-task tick count and a wall-clock stamp of when it was last scheduled,
   updated in the one place the context switch happens.
2. Per-CPU idle time, so "the machine is busy" and "one task is busy" stop being
   the same number.
3. `tasks` reports it; the boot gate asserts that the accounted time is within a
   bound of the elapsed time, which is what catches time being lost or
   double-counted.

**Out of scope.** Using any of it to make a decision. That is S-P5.

**Done when.** Accounted CPU time across all tasks plus idle matches elapsed
time to within one tick per core.

---

## S-P5 — Policy: priorities, fairness, affinity

**This is the phase that makes it a scheduler rather than a rotation.**

It is last on purpose, and the ordering is the argument: a policy decides which
of several correct choices to make, so it can only be built on a lifetime layer
that is correct, and it can only be evaluated with the accounting from S-P4.
Building it earlier means tuning a thing whose behaviour is dominated by a
defect.

**Files created** — `kernel/sched/policy.c`, `tests/kernel/policy_tests.c`

**Steps, in order**
1. **Time slices, stated rather than implied.** The timer preempts today and
   the quantum is whatever the timer period happens to be. It becomes a number
   with a name, per class.
2. **Priorities and classes.** A static priority per task, a small number of
   classes, and the rule that a higher class runs before a lower one. `nice`
   becomes a syscall that does something.
3. **Fairness within a class.** Weighted round-robin over the accounted time
   from S-P4 - the simplest thing that is provably not starving anybody, and
   testable as a data structure.
4. **Affinity and balance.** A task may be pinned; an idle core may take work
   from a loaded one. Both are decisions the run queue makes, so both are host
   tests over the queue rather than boot experiments.
5. **Priority inversion.** Once priorities exist, a low-priority task holding a
   lock a high-priority task wants is a new failure mode. The kernel's locks are
   spinlocks with interrupts masked, so the answer here is probably "do not hold
   one across a scheduling point" enforced by a check, rather than inheritance -
   but that is decision T7 and not mine to settle.

**Tests**
- Host: a task that never runs is a failure the queue reports; weights produce
  the ratios they promise over a long run; a pinned task never appears on
  another CPU; stealing does not take a running task.
- Gate: no task in the boot goes more than N ticks without running.
- Sabotage: `scripts/dev/cases/sched-policy.txt` - ignore the weight; let the
  highest priority starve everything; steal a running task.

**Done when.** A starvation test passes, the weights are measured rather than
asserted by inspection, and `tasks` shows the accounting that justifies them.

---

## S-P6 — Acceptance

- **Fork storm.** A service that forks until slots run out, then exits: refusals
  counted, no panic, `tasks` returns to its starting shape.
- **Thread churn.** Threads created and joined while the process execs, which is
  the combination that produced the `execve` defect.
- **Soak.** The counters return to their starting values after a boot's work.
- **The state machine is asserted**, not merely exercised: the gate fails if any
  illegal transition was attempted.

---

## What this plan does not answer

Decisions are in [decisions.md](decisions.md) and are asked before the phase
that needs them, not settled here.
