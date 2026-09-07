# Phases

Each phase says what it builds, what proves it, and what turns it red. Every
phase ends the same way: host tests, a sabotage case file under
`scripts/dev/cases/`, and — where the phase reaches the running machine — a
boot-gate assertion. A phase with no sabotage case is not finished, because
this project has three times shipped a check that could not fail.

The order is not negotiable in two places and both say so.

---

## C0 — the boot is repeatable before anything moves

**Objective.** The known memory-lifetime defect is closed, and the boot gate
passes at a rate a person can reason about.

**Not negotiable, and first.** A large refactor on a machine with an
intermittent memory bug means every failure afterwards is attributable to two
things, and this project has already spent whole sessions on exactly that
confusion. Starting C1 with the argv defect open would make every later phase
harder to judge and would probably hide the very regressions the phases exist
to catch.

**What is open.** An argv vector that reads as the free-page poison, about one
boot in six, refused by `hw_copy_user_argv` as
`use_after_free:argv_is_poison`. The reference accounting in
`kernel/mm/vmspace.c` has been read and is sound - `release_pt` uses
`vibeos_frame_put`, and `map_raw` pairs every get with a published entry - so
the missing reference is taken by something that maps outside that path. The
detector that would name it (`free_while_mapped`) was blind during teardown
until 2026-09-07 and has not yet been given a run with the blindfold off.

**Done when.** Sixteen consecutive boots with no `POISON_BROKEN`, no
`free_while_mapped`, and no `bad-args` refusal. Not "the rate looks better":
this project has a rule about deciding intermittent questions with counts, and
sixteen is the number the kernel-stack fix was held to.

**Sabotage.** `mm-argv-lifetime.txt` — whatever the defect turns out to be, put
it back and confirm the boot goes red with the same signature.

---

## C1 — the second dispatcher is fixed or deleted, and the decision is written

**Objective.** `kernel/core/syscall.c` and `kernel/proc/process.c` stop being a
liability with a plausible name.

**Two honest options, and the choice is made on evidence rather than
sentiment.** Either the three known defects are fixed and the code becomes the
foundation C2 builds on, or it is deleted and C2 starts from the live path in
`arch_hw.c`. What is not an option is leaving it as it is while building
something new next to it.

**The three defects, restated so the fix cannot be vague.**
1. Thread creation does not check the caller. Anything that can call it can
   make a thread in any process.
2. A process slot is never freed. The table fills and the failure surfaces as
   a fork refused on a machine with plenty of memory - which is exactly how the
   thread-slot leak surfaced, and it took a day to attribute.
3. The caller's identity is read from an argument the caller supplies. A
   syscall that asks the caller who it is has no security model at all.

**The gate on this phase, and it is absolute.** Nothing in this code may become
reachable from ring 3 before those three are closed. Not afterwards, not in the
same change. If C1 chooses deletion, this is satisfied trivially and that is a
point in deletion's favour.

**Done when.** Either the three defects have host tests that go red when the
fix is removed, or the files are gone and `check-mm-layering.sh` has one fewer
thing to allow.

**Sabotage.** `core-dispatcher.txt` — if fixed: create a thread as another
process, exhaust the slot table, lie about the caller id, and confirm each is
refused. If deleted: confirm nothing references it.

---

## C2 — one owner for "what is a task"

**Objective.** The task table has a single definition, and the arch layer holds
only what a context switch needs.

**Files.** `kernel/core/task.c` grows; `arch_hw.c`'s `g_tasks` shrinks to the
register state, the kernel stack and the page-table root.

**Why this one first among the moves.** Because the scheduler, exec, signals,
pipes and the process supervisor all reach into `hw_task_t` today, so it is the
structure that binds the monolith together. Splitting it is what makes the
later phases small; leaving it is what makes them all touch the same file.

**Steps.**
1. `vibeos_task_t` in `kernel/core/` holds identity, state, parent, exit
   status, credentials, and the descriptor table.
2. `hw_task_t` keeps `ctx`, `kstack_*`, `cr3` and a pointer to the core task.
3. Every reader of `g_tasks[i].pid` and its neighbours moves, one caller at a
   time, with the boot gate run between each.
4. The state machine and its transition table - which already exist and
   already catch illegal transitions - move with it, because that table is the
   single source of truth about what READY means and a second opinion is how
   two cores ran one task.

**Done when.** `arch_hw.c` no longer names a field that is not part of a
context switch, and `[TASKS] MUSTBEZERO` still reads zero on every boot.

**Sabotage.** `core-task.txt` — publish a slot before its state is set; let two
cores claim one task; free a kernel stack from the core standing on it. All
three are defects this project has actually had, and the fix must not lose the
guards that catch them.

---

## C3 — the scheduler decides, the arch layer switches

**Objective.** `hw_schedule` stops picking and starts obeying.

**Steps.**
1. `kernel/sched/` already has a policy, a run queue and a torture test that
   found a real defect. It is not reached from the boot path.
2. `hw_schedule` calls it for the decision and keeps only the switch.
3. The quantum, the classes and the affinity mask stop being computed twice.

**Done when.** The scheduler torture runs against the same code the machine
runs, and `charged + idle == seen` still holds exactly.

**Why this is safer than it sounds.** The decision is already written and
already tested; what changes is which copy the machine consults. The risk is
concentrated in one function and the existing accounting identity is a strong
check on it.

**Sabotage.** `core-sched.txt` — return a task another core is running; ignore
the quantum; return an idle task while a ready one exists.

---

## C4 — one syscall table, with the checks in one place

**Objective.** The Linux syscall table leaves `arch_hw.c`, and every permission
check has exactly one site.

**Deliberately last, and it is the phase most likely to introduce a security
defect.** The checks that matter - `hw_signal_permitted`, the fork guard, the
range checks on user pointers - are choke points today *because* they are in
one file. Moving nine hundred lines of dispatch is exactly the operation that
turns one choke point into two that drift apart.

**Steps.**
1. The table moves as data: number, name, handler, and *which checks apply*.
2. The checks stay where they are and are called from the dispatcher, rather
   than travelling with the handlers.
3. A test enumerates every entry and asserts the declared checks are the ones
   actually run - which is a thing the current arrangement cannot state at all.

**Done when.** Adding a syscall without declaring its checks fails a host test,
not a review.

**Sabotage.** `core-syscall.txt` — remove a check from one handler and confirm
the enumeration test names it; declare a check and do not run it; run a check
that is not declared.

---

## What this plan does not do

It does not make `kernel/core/` portable to a second architecture. There is no
second architecture, and building for one that does not exist is how the
current portable kernel ended up untested and unreachable. The goal is one
kernel with boundaries, not two kernels with a shared vocabulary.
