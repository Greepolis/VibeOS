# Tasks and Scheduling Progress

Status: In Progress — phases S-P0 to S-P2 of [docs/sched/](../sched/README.md)
are done. Counters and a console view exist, a transition table decides what a
slot may become, every task reference carries its tenancy, and the run queue is
a tested data structure. S-P3, which repairs the defects, is next.
Last review: 2026-08-31

## Why this is being rewritten

Everything about a task lived in `kernel/arch/x86_64/arch_hw.c` — 8312 lines,
267 static functions, with the scheduler, syscalls, program loading, pipes,
signals, the network glue, the interrupt controllers and the console all in one
space where any function can reach any state.

The defects that produced share one shape: **a task's lifetime was managed by
several pieces of code that each knew part of the truth.** The silent wedge, the
reaper writing to a published slot, `execve` not asking whether siblings shared
an address space, a recycled slot keeping a stale `cr3`. Each was fixed where it
was found; none could have been prevented, because nothing said what a slot's
states were or who could change them.

## What exists now

### S-P0 — counters and the view

`tasks` on the kernel console prints every slot: generation, state, pid, tgid,
ppid, cr3, whether it is user or kernel, thread or process, on a CPU or not, and
the four provenance fields — `cr3_set_by`, `ready_by`, `aspace_killed_by` and
who last changed its state. Three of those existed already, each added after a
defect had cost a session.

Alongside it, counters for what the subsystem has done, and four that must be
zero and are asserted by the boot gate: an illegal state change, a write to a
slot already published as reusable, a stale reference used as if it still named
its task, and a task about to run on tables somebody else freed. Each names a
defect this kernel has actually produced.

**The split is the point.** Deciding what to say about a task is portable and
lives in `kernel/sched/view.c`; only reading the machine's table is not, and the
architecture answers one function. That is the shape the rest of the rewrite
follows.

### S-P1 — the transition table

`kernel/sched/task.c` owns what a slot may become. The transitions that are
*absent* are the design: RUNNING to FREE is the silent wedge, and ZOMBIE to
READY would let a reap race a wake and put a task back on a destroyed address
space.

All twenty-six assignments to a task's state now go through one function, which
asks the table and **refuses rather than performs**. Storage still lives in the
architecture's field; what has moved is the decision.

**The table was wrong on its first boot, and the machine said so.** SETUP to
RUNNING was missing — a core adopts the thread it is already executing, and an
idle task is created already on its CPU. The refusal named the slot, both states
and the function. With assignments scattered across nine places, that knowledge
was written nowhere.

**Tenancy is everywhere** (decision T1). A bare slot number stays valid forever
and names whoever holds the slot now, so a reference kept across a lock release
silently becomes a reference to a different task. The generation is bumped when
a slot is *claimed*, never when it is released: bumping on release leaves the
freed slot already carrying the number its next occupant will have, so a
reference taken beforehand would validate against the new tenant. That is a
sabotage case, because it reads as equivalent and is not.

### S-P2 — the run queue

`hw_pick_next` is a wrapper over `kernel/sched/runq.c`, where "two CPUs never
pick the same slot" — a defect this kernel has had — is a unit test rather than
a boot somebody hopes goes wrong.

The queue does not decide what may run; it asks through a callback, so the state
machine stays the single source of truth about what READY means. A queue with
its own opinion would be a second one.

**A real defect was fixed in the move.** The old scan started from the *current*
task, which is fair while something is running and degenerates when the current
task is idle: the search then begins at the same place every time and the
high-numbered slots wait behind the low ones. There is a cursor per CPU now.

## What this cost

A weak-symbol mistake broke the Windows CI. The view's defaults were weak
definitions in one file with the real ones in another, and PE/COFF does not
resolve that across objects — the Linux build linked and said nothing. Every
weak stub that works in this kernel happens to sit in the same translation unit
as its caller, which is why the pattern looked safe when it was copied. The
view's data source is registered explicitly now, as the frame layer's lock is,
and the trap is recorded in CLAUDE.md.

## Next

S-P3, lifetime: exit, reap and exec in one layer, with the two ordering rules
made structural rather than remembered, and the address-space ownership question
asked in exactly one place. That is also what unblocks the loader rewrite, and
through it memory-plan P4 step 3.

Scheduling policy — priorities, fairness, affinity, load balancing — is S-P5,
after the per-task CPU accounting at S-P4 that nothing here has today. The first
draft of the plan listed it as a one-line non-goal, which was wrong: a plan
called "Tasks and Scheduling" that contains no scheduling is mis-scoped. The
ordering argument stands, the omission did not.

---

## S-P4 — Accounting (done, 2026-09-02)

**The machine can now say where its time went.** Nothing above this phase was
possible without it: a policy decides which of several correct choices to make,
and every way of deciding - priorities, weights, whether anybody is starving - is
a statement about time already spent. Tuning a policy against numbers this
kernel could not produce would have been tuning against a guess.

`kernel/sched/account.c` is arithmetic and nothing else: it makes no
decision, holds no lock, and calls nothing, so it is host-tested exhaustively and
a defect in it can never be a defect in scheduling.

**Charged by tick, not by timestamp.** Every tick goes to exactly one place -
one task, or one core's idleness - which makes the check an *identity* rather
than a tolerance:

    [TASKS] CPUTIME charged=0x241 idle=0x3b0 seen=0x5f1 balanced=yes dropped=0x0
            cpu0_idle=0x10c cpu1_idle=0xfc cpu2_idle=0xae cpu3_idle=0xfa

577 + 944 = 1521, and the four per-core figures add to the idle total exactly. A
tolerance is something you argue about when it drifts; an identity is something
the boot gate asserts.

**An idle task is a task, and must not read as work.** Every core here has an
idle task holding a real slot, so charging by slot alone would report a machine
doing nothing as fully busy - and every ratio built on that would be wrong in
the direction that hides a problem.

**Every core charges its own tick**, unlike the wall clock, which only core zero
owns. The distinction is easy to get backwards: the clock counts wall time and
this counts CPU time, and on four cores those differ by a factor of four.

### Three defects in the wiring, two of them found by their own counters

- **The tick handler indexed the task table with a value it had not checked.**
  It runs on every core on every interrupt, including before that table means
  anything.
- **Accounting was sized with `g_cpu_online_count`**, which is 1 when
  the scheduler starts - the other cores come up afterwards. Three quarters of
  the ticks were refused.
- **The report read the totals, then walked the per-core counters**, with ticks
  arriving in between. It printed a core with more idle time than the whole
  machine had. Two moments reported as one, which is the same mistake as a log
  line assembled from two writes.

The second is the interesting one. **A refused tick keeps the identity intact** -
charged plus idle still equalled seen, because a refused tick was never seen - so
the balance check could never have found it. It needed its own counter, and with
that counter the sabotage is unambiguous: sizing the layer for one core again
turns the boot red with `cputime_ticks_dropped=1184`.

That is the shape this project keeps meeting: a check that cannot fail is not a
check. The balance assertion is real, and it would have watched this bug go past.

### What S-P5 will be judged against

`max_wait` - the longest a task waited between becoming runnable and
running - is recorded here because this is the only moment both halves are
known. It is the number that says whether anybody is starving, and it is
deliberately the worst case rather than the last one.
