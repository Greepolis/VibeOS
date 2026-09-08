# Invariants

Properties that must hold across every phase, each with the counter or assertion
that watches it. A property with no watcher is a hope, and this project has a
rule about those.

Most are not new. What this file does is state which of them the refactor is
most likely to break, so a phase that breaks one is caught by a number rather
than by a reader — which is the difference between ninety seconds and a phase.

Two of them are new in this revision and are the ones the refactor adds risk to
rather than inherits: **blast radius** and **the hot paths**.

---

## There is one definition of each thing

**The invariant.** For any question the kernel answers — what state a task is
in, whether a signal may be sent, how big a time slice is — exactly one piece of
code answers it.

**Why it is first.** The refactor's whole failure mode is producing a second
answer that agrees today and drifts tomorrow. It has already happened here
without a refactor: a portable syscall dispatcher nothing reaches, a page cache
with no caller, a swap stack in the same state. The scheduler was on that list
and has come off it — `hw_pick_next` now consults the portable policy — which is
the one worked example of this invariant being *restored*.

**Watched by.** `check-reachable.py` — is this function named outside the file
that defines it? — and `check-counters-produced.py`, which fails when a counter
is declared and never written. A phase that moves a decision moves its counter,
and a counter that stops being produced is the alarm.

**Not yet watched:** nothing detects a *duplicated* decision, only an unreached
one. C4's enumeration test is the first thing in this plan that would.

---

## A check has one site

**The invariant.** `hw_signal_permitted`, the fork guard, and the user-pointer
range checks are called from every path that needs them and defined once.

**Why it is here.** These are choke points *because* they live in one file.
Moving handlers out of `arch_hw.c` — which is exactly what C4 does, with 4,169
lines of them — is the operation that turns one choke point into two, and the
second one always forgets a case. Every security-relevant defect review has
found in this project was in that file, which is a consequence of its size
rather than a coincidence.

**Watched by.** `check-chokepoints.py` from C1, and C4's enumeration test:
every syscall declares which checks apply, and the test asserts the declared set
is the set actually run.

---

## Adding one thing touches one file

**The invariant.** For each extension point — filesystem, block driver,
character device, input device, network interface, display, syscall, ABI — a new
implementation is added by registering a struct, and the count of files edited
does not grow.

**Why it is new, and why it is the plan's real metric.** Line count has been the
stated completion criterion twice and failed twice: both times the file grew
while the document naming its length sat unchanged. Blast radius is the property
that was actually wanted. It is already 1 for storage and block — four
filesystems and four drivers arrived without their layers being touched — and
`arch_hw.c` for everything else.

**Watched by.** `check-blast-radius.py` (C1), ratcheted. The baseline is today's
table and may only go down.

---

## The state machine is the only opinion about task state

**The invariant.** Every transition goes through `hw_task_set_state`, and the
table refuses the illegal ones.

**Why it survives the refactor unchanged.** It has caught two real defects: a
slot released outside the table stayed RUNNING for a boot, and a thread reaped
straight to FREE logged one ILLEGAL line per thread. Both were invisible except
through that table. C5 moves it; C5 must not dilute it.

**Watched by.** `[TASKS] MUSTBEZERO illegal_transition`, asserted zero.

---

## Nothing is published before it is complete

**The invariant.** A task slot becomes FREE last; an address space is torn down
before the zombie is announced; a kernel stack is freed by the core that has
provably left it.

**Why it is one property rather than three.** They are three instances of the
same mistake, found months apart, each diagnosed from the far end. The general
form: a structure becomes visible to another core before it is finished, and the
other core is faster than expected.

**Watched by.** `use_after_publish`, `tenancy_mismatch`, `cr3_without_owner`,
`dead_kstacks_freed`, all on the `[TASKS]` lines, all asserted.

---

## A frame is freed once, when nobody maps it

**The invariant.** `owners` equals the number of page-table entries pointing at
a frame, and a frame returns to the allocator exactly when that reaches zero.

**Why it is in a *core* plan.** C5 moves the structure that owns address spaces,
and this is the property most likely to break silently under that move. It is
also the one currently violated — C0 exists for it.

**Watched by.** `free_while_mapped`, `frames_double_put`, `poison_hits`,
`fork_undercounted`, `rmap_mismatch` — with a caveat that matters more than the
list: **the free-side check is sampled**, historically one free in sixteen. A
defect happening once a boot has a small chance of being seen. A zero from it is
not evidence of absence, and any phase relying on these raises the sampling
first and says what it raised it to.

---

## Time is conserved

**The invariant.** `charged + idle == seen`, exactly, on every boot.

**Why it is kept through C5.** It is an identity rather than a tolerance, so it
catches a scheduler that loses a tick as surely as one that loses a task — and
C5 is precisely the phase that could lose a tick by charging it to the wrong
owner during the handover.

**Watched by.** The `[TASKS] CPUTIME` line and its `balanced=yes`.

---

## Every module says whether it is broken

**The invariant.** Every module has at least one must-be-zero counter, printed
on its own tag and asserted by the gate.

**Why it is new.** There are five in the whole kernel today, against 25 report
tags. Every hard defect in this project's history was found by a counter or a
detector; a module with neither is a module whose defects will be found by
reading, months late, at the cost this file exists to avoid.

**Watched by.** `check-subsystem.py` (C1), which fails a module that has none.
Ratcheted, so today's gaps are the baseline and may only close.

**And the rule attached to it:** a counter nothing increments and a counter
reporting nothing happening are indistinguishable. A new one needs one
deliberate demonstration that it *can* be non-zero before anything is concluded
from a zero.

---

## A number the gate reads is produced by something

**The invariant.** Every counter the boot gate asserts is incremented somewhere.

**Why it is stated rather than assumed.** `VIBEOS_BLK_TIMEOUT` was defined,
printed, asserted and produced by no driver — green by construction.
`rmap_cycles` was in the same state. And a disk-interrupt counter read zero for
an hour because the dispatcher hook that would have incremented it had never
been added, which sent four measurements in the wrong direction.

**Watched by.** `check-counters-produced.py`, `check-assertions-covered.py`.

---

## The hot paths do not get slower

**The invariant.** Syscall entry, page fault and context switch stay within a
stated margin of the C0 baseline.

**Why it is new.** This kernel has never been measured. The architecture adds
indirect calls to paths that run millions of times a boot, and with no baseline
the refactor could not be shown *not* to have cost anything — in either
direction, since an unmeasured reassurance is worth exactly as much as an
unmeasured alarm.

**The sub-rule that keeps it cheap:** registration is resolved once, not per
call. A driver is looked up at open, an ABI is bound at process creation, a lock
is registered at init. The danger was never the indirect call; it is the lookup
or the lock somebody adds beside it.

**Watched by.** The `[PERF]` line and its ratchet, confirmed red by slowing a
hot path on purpose before it is trusted.

---

## A detector is suspected before it is believed

**The invariant.** Not a counter — a rule about how the others are read, and it
belongs here because every invariant above is enforced by an instrument that can
itself be wrong.

Three detectors in one session reported defects that were their own bugs. A
frame-poison check reported 3,019 use-after-frees in a boot that had none, and
cost a phase. The scheduler torture reported nine seeds of unfair weighting that
were its own model holding a stale `nice`.

**How it is answered, since a count never answers it.** Make the detector name
the object: which frame, which word, what value, released by whom. The 3,019
became `found=0 freed_by=0` at `base + reserved_prefix` — the pool's first
frames, released by nobody — on the first boot after the witness was added.
