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

**Done when.** `hw_pick_next` is gone; the host tests cover the six cases; boot
rate unchanged against the parent commit.

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

## S-P4 — Acceptance

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
