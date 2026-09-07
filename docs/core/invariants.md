# Invariants

Properties that must hold across every phase, each with the counter or
assertion that watches it. A property with no watcher is a hope, and this
project has a rule about those.

These are not new. Almost every one already exists somewhere in the tree; what
this file does is state which of them the refactor is most likely to break, so
that a phase that breaks one is caught by a number rather than by a reader.

---

## There is one definition of each thing

**The invariant.** For any question the kernel answers — what state a task is
in, whether a signal may be sent, how big a time slice is — exactly one piece
of code answers it.

**Why it is the first one.** The refactor's whole failure mode is producing a
second answer that agrees today and drifts tomorrow. It has already happened
here without a refactor: there is a portable syscall dispatcher nothing
reaches, a scheduler policy the boot path does not consult, and until this week
a page cache and a swap stack in the same state.

**Watched by.** `check-counters-produced.py`, which fails when a counter is
declared and never written — the cheapest available proxy for "this code is
not reached". A phase that moves a decision must move its counter, and a
counter that stops being produced is the alarm.

**Not yet watched, and worth building:** nothing detects a *duplicated*
decision, only an unreached one. C4's enumeration test is the first thing in
this plan that would.

---

## A check has one site

**The invariant.** `hw_signal_permitted`, the fork guard, and the user-pointer
range checks are called from every path that needs them and are defined once.

**Why it is here.** These are choke points *because* they live in one file.
Moving handlers out of `arch_hw.c` is exactly the operation that turns one
choke point into two, and the second one is always the one that forgets a case.
Every security-relevant defect review has found in this project has been in
that file, and that is a consequence of its size rather than a coincidence.

**Watched by.** C4's enumeration test: every syscall declares which checks
apply, and the test asserts the declared set is the set actually run. Until C4
exists, by nothing — which is a gap this plan states rather than papers over.

---

## The state machine is the only opinion about task state

**The invariant.** Every transition goes through `hw_task_set_state`, and the
transition table refuses the illegal ones.

**Why it survives the refactor unchanged.** It has already caught two real
defects: a slot released outside the table stayed RUNNING for a boot, and a
thread reaped straight to FREE logged one ILLEGAL line per thread. Both were
invisible except through that table.

**Watched by.** `[TASKS] MUSTBEZERO illegal_transition`, asserted zero by the
boot gate.

---

## Nothing is published before it is complete

**The invariant.** A task slot becomes FREE last; an address space is torn down
before the zombie is announced; a kernel stack is freed by the core that has
provably left it.

**Why it is stated as one property.** These are three instances of the same
mistake, found months apart, each time diagnosed from the far end. The general
form is: a structure becomes visible to another core before it is finished, and
the other core is faster than expected.

**Watched by.** `use_after_publish`, `tenancy_mismatch`, `cr3_without_owner`
and `dead_kstacks_freed`, all on the `[TASKS]` lines, all asserted.

---

## A frame is freed once, when nobody maps it

**The invariant.** `owners` equals the number of page-table entries that point
at a frame, and a frame is returned to the allocator exactly when that reaches
zero.

**Why it is in a *core* plan at all.** Because C2 moves the structure that owns
address spaces, and this is the property most likely to be broken silently by
that move. It is also the one currently violated: an argv vector reads as the
free-page poison about one boot in six, which is why C0 comes first.

**Watched by.** `free_while_mapped`, `frames_double_put`, `poison_hits`,
`fork_undercounted`, `rmap_mismatch` — and a caveat that matters more than the
list: **the free-side check is sampled**, historically one free in sixteen. A
defect that happens once a boot has a small chance of being seen by a check
that looks at one free in sixteen, and a zero from it is therefore not evidence
of absence. Any phase that relies on these counters should raise the sampling
first and say what it raised it to.

---

## Time is conserved

**The invariant.** `charged + idle == seen`, exactly, on every boot.

**Why it is worth keeping through C3.** It is an identity rather than a
tolerance, so it catches a scheduler that loses a tick as surely as one that
loses a task — and C3 is precisely the phase that could lose a tick by charging
it to the wrong owner during the handover.

**Watched by.** The `[TASKS] CPUTIME` line and its `balanced=yes`.

---

## A number the gate reads is produced by something

**The invariant.** Every counter the boot gate asserts is incremented somewhere
in the tree.

**Why it is here rather than assumed.** `VIBEOS_BLK_TIMEOUT` was defined,
printed, asserted by the gate and produced by no driver — green by
construction. `rmap_cycles` was in the same state. And this week a disk
interrupt counter read zero for an hour because the dispatcher hook that would
have incremented it had never been added, which sent four measurements in the
wrong direction.

The general form, and it is the one to carry into this refactor: **a counter
nothing increments and a counter reporting nothing happening are
indistinguishable.** A new counter needs one deliberate check that it *can* be
non-zero before anything is concluded from its being zero.

**Watched by.** `check-counters-produced.py` and
`check-assertions-covered.py`.
