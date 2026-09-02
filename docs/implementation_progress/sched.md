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

### And then the balance assertion was wrong in the other direction

It asserted an exact identity, and turned a correct kernel red twice in twelve
boots: `charged=540 idle=874 seen=1413`, one apart.

Two separate faults, and both are mine. The three totals were incremented
non-atomically from four cores - excused in a comment saying a lost increment
"costs one tick", which was wrong about the consequence: it costs the identity,
and the identity is the whole promise. They are atomic now, which at a hundred
ticks a second on four cores costs nothing measurable.

The second is the more interesting. **No read order makes it exact.** A tick
increments the total and then one of the parts, so a core caught between those
two adds leaves the parts one short; reading the parts first just moves the
discrepancy to the other side. The reader is not atomic with respect to four
writers and cannot be made so cheaply.

What is true is that **at most one tick per core can be mid-update**, so the two
sides agree to within the number of cores - which is exactly the bound
`docs/sched/phases.md` asked for, and which I tightened into an identity
without noticing I was changing the requirement. The bound is a fact about the
counters, not a tolerance chosen to make a check pass; anything beyond it is a
tick counted twice or lost, which concurrency does not explain.

A check that reports healthy behaviour is one people learn to ignore. This
project has said so since the interleaved-line detector, and I wrote one anyway
within an hour of quoting it.

### What S-P5 will be judged against

`max_wait` - the longest a task waited between becoming runnable and
running - is recorded here because this is the only moment both halves are
known. It is the number that says whether anybody is starving, and it is
deliberately the worst case rather than the last one.

---

## S-P5 — Policy (the layer, done 2026-09-02)

**The phase that makes it a scheduler rather than a rotation.** It is last on
purpose: a policy decides which of several *correct* choices to make, so it can
only sit on a lifetime layer that is correct, and it can only be evaluated with
the accounting from S-P4. Built earlier it would have been tuned against a
defect.

`kernel/sched/sched_policy.c` is a pure function of state the caller
supplies - which slots are runnable, which core is asking. No task table, no
lock, no clock. Every rule is therefore settled by a host test rather than by
watching a boot and forming an impression, which is the only way a policy can be
argued about at all.

**What it decides**

- **The quantum is a number somebody chose.** Preemption happened whenever the
  timer fired, so the slice was "the timer period" - nobody's decision. It is
  now per class, in ticks, with the reason written next to each.
- **Classes are absolute.** A runnable kernel task runs before any normal one,
  however long the normal one has waited. That is what makes a class different
  from a large weight. Idle is a class rather than a flag, so an idle task
  competes by the same rule instead of needing a special case in the picker.
- **Weights come from nice**, as a table rather than a formula - the property
  that matters, roughly 25% per step over a range of about a thousand, is
  readable in a table and is not readable in an expression.
- **A new task is admitted level with its class, not at zero.** Admitting at
  zero hands every newly created task the whole machine until it catches up:
  not a subtle unfairness but a fork bomb that needs no malice.
- **Renicing does not reset history.** Otherwise a task can have the machine for
  free by renicing itself.
- **Affinity is checked in the picker**, so a pinned task cannot appear
  elsewhere even when it is the only thing runnable.

**The tests measure rather than inspect.** They run the picker for tens of
thousands of ticks and count who got what: nice 0 against nice 5 must land
between 2.7:1 and 3.5:1, which is the weight ratio doing its job rather than a
number read off the table. And every rule was confirmed by breaking it - class
ordering, weighting, affinity and level admission each produce their own named
failure when removed.

### The first run found a real starvation bug

Virtual time advanced by `(ticks * WEIGHT_BASE) / weight` in whole
units. For any task more favoured than nice 0 the weight exceeds the base, so
**that division truncates to zero**: a nice -5 task accumulated no virtual time
at all, always held the smallest, and ran forever. Not a rounding error - total
starvation of everything else.

Virtual time is fixed point now, ten bits of fraction, which puts the smallest
step (nice -20) at 11 instead of 0 and leaves four orders of magnitude before a
64-bit overflow.

### A mistake worth recording: the header collided with an existing one

The first version of this layer was written to `include/vibeos/policy.h`,
which **already existed** - it is the security policy, capabilities and MAC. The
file was overwritten. It was restored from git intact and the scheduling one
renamed to `sched_policy.h`.

The tell was there and was read past: the editor reported the file as *updated*
rather than *created*. Check whether a name is taken before taking it, and read
what a tool says it did rather than what it was asked to do.

### Still open in S-P5

The layer exists, is tested and is not yet wired into `hw_pick_next`.
Steps 4's work stealing and step 5's priority inversion are untouched - the
latter is decision T7 and is not mine to settle.

---

## Fork-bomb protection (2026-09-02)

**The failure this closes is not that fork can fail.** It already could, and
returned an error as it should. It is what running out of slots *meant*: the
task table is first-come, so a program forking in a loop took every slot, and
then init could not start a service and the shell could not run a command. The
machine stayed alive and became unadministrable, which is worse than one that
refuses.

kernel/sched/forkguard.c answers one question - may this task create another -
from counts the caller supplies, so the rules are settled by host tests instead
of by writing a fork bomb and hoping it reproduces.

Two rules, and **neither is interesting alone**:

- **A reserve.** Four slots, one per core, that only privileged tasks may take.
  Without the ceiling below, a bomb still fills every unreserved slot and every
  ordinary program stops working.
- **A ceiling.** Eight live children per task. Without the reserve above,
  enough cooperating processes reach the same place.

Both numbers are chosen rather than discovered, and the reasoning is written
where they are set: four is the smallest reserve that leaves every core able to
start something, and eight is comfortably more than anything in this boot forks
- the shell's deepest pipeline is three - so the limit binds on a bomb and on
nothing else. A healthy boot refuses **zero** forks.

The refusal says which rule it was, because both return EAGAIN and they are
completely different investigations: one is a program misbehaving, the other is
a machine that is full.

    [SCHED] fork refused reason=too-many-children pid=0x19 children=0x6 slots_in_use=0x11

Confirmed by tightening the child limit to two, which makes an ordinary boot
trip it six times with the reason, the pid and the counts. Live children are
counted by walking the table rather than kept in a field: a counter has to be
right on every exit path including the ones that fail half way, and the table is
twenty-four entries long. Zombies count, because a zombie still holds a slot.
