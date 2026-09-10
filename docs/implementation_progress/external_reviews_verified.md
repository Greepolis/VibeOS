# The external reviews, checked against the code

Three reviews so far. Every finding was verified before being accepted or
dismissed, because a review is a detector and the first question about a
detector is whether it is right.

## Review two

## The result, and the thing that reframes half of it

| ID | Claim | Verdict |
|---|---|---|
| **M-001** | `vmspace_protect` modifies PTEs without CAS | **Real, live — fixed** in `ab5d7f4` |
| **S-001** | lazy init of the syscall waitset races on SMP | Real pattern, **unreachable code** |
| **S-002** | the security audit log is unprotected | Real pattern, **unreachable code** |
| **I-001** | interrupt counters are not SMP-safe | Real pattern, **unreachable code** |
| **M-002** | `clone_cow` is not transactional | Real, live, **benign** |
| **M-003** | rmap bookkeeping failures are ignored | Real, but the divergence **is** counted and gated |
| C-001 | locking discipline is not uniform | Architectural; the core plan's subject |
| F-001 | filesystem global state needs an SMP audit | Not examined here |

**Three of the four concurrency findings are in code no process reaches.**

- `vibeos_syscall_dispatch` is called from `tests/kernel/kernel_tests.c` and
  nowhere else. The arch layer never names it.
- `vibeos_sec_audit_record` is called from `kernel/core/syscall.c` — the same
  unreachable dispatcher — and from the tests. Grepping the arch layer for
  `vibeos_sec_*` returns nothing at all: **the running kernel never calls the
  security layer.**
- `kernel/core/interrupts.c` is named by no file outside `kernel/core`.

That is the third independent confirmation of the same structural fact.
`check-reachable.py` reports it mechanically, `docs/core/` states it as C3's
subject, and now an outside reviewer has found three more defects in that half
without knowing that nothing runs it.

It does not make them harmless — C3's gate is precisely that nothing there may
become reachable from ring 3 before its defects are closed, and that list has
grown from three to six. It changes what to do about them: this is evidence for
deleting that half rather than for hardening it, and C3 says the choice is made
by asking which produces the smaller diff in C4.

## Where the review's mechanism was wrong

Two findings are real and the stated mechanism is not the one in the code. Both
times the real defect was worse, which is worth recording: a review that is
approximately right is more useful than one that is vaguely right, and the
difference only appears by reading the code.

**M-002.** The review says a failed clone leaves the caller to clean up. The
caller does clean up: `hw_sys_fork` calls `hw_aspace_destroy(&child->proc.as)`
on failure, releasing the references the partial mappings took. What survives is
that the *parent's* entries already converted writable→COW stay converted — and
that is harmless. A COW-marked page is correct; the parent takes a fault and
copies. Rolling it back would not even be safe, because another core may already
have faulted on the converted entry. Documented, not "fixed".

**M-003.** The review says ignoring `rmap_add`/`rmap_remove` failures makes the
reference count and the real mapping count diverge silently. They do diverge —
but not silently: `rmap_exhausted` and `rmap_missing_remove` are both on the
`[MEM] MUSTBEZERO` line the boot gate asserts, and `add_locked` increments
`exhausted` before returning −1. What is true is that the call sites proceed
rather than rolling back, which this subsystem chooses deliberately and the
counter is what makes safe.

One genuine hole the review did not find: `rmap_add` also fails when
`frame_index(phys) == RMAP_NONE` — a frame outside the pool — and that branch
increments **nothing**. A mapping of such a frame is silently untracked, so the
"owners == rmap holders" invariant quietly does not apply to it and no number
says how often.

## The deterministic test, and the three arrangements it took

Yesterday's commit owed a test: the H-001 and H-002 fixes had no way to fail.
It exists now, driving fork's clone through an injection point (a harness cannot
otherwise produce a window a few instructions wide).

Getting it to *fail* took three attempts, and the first two were the shape this
project has recorded five times — right about the property, wrong about the
arrangement:

1. The hook fired **before** `clone_one` loaded the entry, and the test re-read
   it. The compare-exchange therefore never lost, and the sabotaged kernel
   behaved exactly like the fixed one.
2. The hook wrote **the same value back**. The page was already writable with no
   copy-on-write mark, so there was nothing for the exchange to lose against.
3. The hook installs a **different frame** — which is what the fault's copy path
   actually does. Now the exchange loses and the two kernels separate.

```
sabotage (clone_one adopts again):
    FAIL:fork left the parent writable on a shared frame
fix restored:
    ALL_TESTS_PASS
```

Without the sabotage this would have shipped green in both states, and
yesterday's fix would have stayed "verified" by something that could not tell
the difference.

## Review three

Four findings, in a table titled **"verified and reachable"**. All four are
real. None of them is reachable.

| ID | Claim | Verdict |
|---|---|---|
| VOS-001 | `THREAD_CREATE` has no ownership check | Real — **and already in `docs/core/README.md`** |
| VOS-002 | process slots leak after terminate | Real — **also already there** |
| VOS-003 | the global waitset is unsynchronised | Real — the same as review two's S-001 |
| VOS-004 | `kernel/mm/vm.c` is a model, not page tables | **Correct**, and it classifies itself correctly |

```
vibeos_syscall_dispatch   called from the arch layer: 0
vibeos_thread_create                                : 0
vibeos_proc_terminate                               : 0
vibeos_vm_map / vibeos_vm_create                    : 0
```

One case needed care rather than a grep. `kmain.c` **is** entered — it prints
`BOOT_OK` — and it calls `vibeos_proc_terminate`. But that call sits inside
`vibeos_kernel_dispatch_trap`, and the arch layer never calls *that*, so the
path stays dead. "The file runs" and "this function in it runs" are different
questions.

VOS-001 and VOS-002 are word for word two of the three defects the core plan has
listed for weeks. An independent reviewer rediscovering them is genuine
confirmation that they exist. Labelling them reachable is the fourth independent
misreading of the same structural fact.

## What that produced, which is worth more than the findings

Three reviews in a row read `kernel/core/syscall.c` and `kernel/proc/process.c`
as live kernel code and rated defects in them HIGH. `check-reachable.py` knew,
`docs/core/` said so, and **a person opening the file had no way to find out**.

Those four files now open with a banner saying they are not reached, carrying
the command that verifies it rather than asking to be believed, and carrying the
reason it is not an invitation to relax:

> The danger this banner guards against is the opposite of complacency — it is
> somebody wiring this up because it looks finished.

C3's gate is unchanged and its list has grown from three defects to six.

## And a defect in the checks, found while clearing the banners

The banners are comments, so they cannot change behaviour — proved rather than
asserted, by comparing the compiled `.text` with and without them: byte
identical.

What they did do was raise four `-Wcomment` warnings, because the banner
contained `kernel/arch/x86_64/*.c` and that is a `/*` inside a block comment.
Those warnings were dismissed as "transient" **three times in one session**.

The reason they could be is the real finding. `warnings=` filtered only
`build-id`, while `clang-warnings=` also filtered
`unused-command-line-argument` - which clang emits eight times on every clean
build, assembling `entry.s` with C flags. So the gcc-side counter read 8 on
every clean build, always, and a counter that is never zero is a counter nobody
reads. The two counters disagreed and the noisier one is the one seen first.

Both filters now match, and a clean build reads `warnings=0`. `CLAUDE.md`
already says to treat a moving warning count the way `rc=` is treated; that only
works if the count is telling the truth.

## Review four

One finding, and the sharpest of the set: `vibeos_vmspace_map_raw` ignores the
return of `vibeos_rmap_add`, the node pool is finite, so a mapping can be
published with its holder unrecorded. Reachable by creating enough concurrent
mappings.

All of that is true. The review then asked the question the previous three did
not: **what depends on the map being complete?**

The answer is nothing, and for a reason that was written down nowhere. Every
consumer cross-checks against `vibeos_frame_owners`, which is taken by the
mapping itself and knows nothing about the node pool:

```
anon.c:59       swap-out    owners != 1 || rmap_count != 1  -> skip
vmspace.c:1126  swap-out    the same pair                   -> refuse
vmspace.c:1257  compaction  owners != rmap_count            -> refuse
```

So an under-recorded holder makes the two disagree and the operation refuses.
Reclaim degrades; nothing is corrupted. `exhausted`, `untracked` and
`nodes_peak` say how much. The pool is sized at two nodes per frame — about
208,000 against an observed peak of 1,709 — so exhaustion needs a far more
forked workload than anything here produces.

Trusting `rmap_count` alone is what turns that into corruption: swap-out would
evict a frame a second address space still maps, and compaction would move one
and leave the other mapping pointing at the old address.

That property is stated in `include/vibeos/rmap.h` now and enforced by
`check-rmap-crosscheck.py`, validated by removing the cross-check in `anon.c`
and watching it go red.

### The check was wrong on its first run, and the fix was not to widen it

It flagged `vmspace.c:1237`, `total = vibeos_rmap_count(old_phys)`. That read
decides nothing — it bounds an array, and an under-recorded count makes the
bound *more* permissive while the real cross-check twenty lines later still
refuses. A false positive.

Widening the window to reach the guard would have swallowed the property: a bare
use twenty lines from a cross-check on a different frame would then pass. So the
site carries a `rmap-bare-ok:` marker with prose saying why instead. A second
bare use is not forbidden — it is *noticed*, and somebody has to write down why
it is safe, which is the trade `check-chokepoints.py` already makes.

### And the branch none of the four reviews found

`vibeos_rmap_add` has two failure modes and only one was counted. Pool
exhaustion increments `exhausted`. A frame outside the region this layer
describes returned −1 in silence, so that mapping went unrecorded and no number
said how often. It increments `untracked` now.

It reads zero, and that zero is qualified rather than banked: every frame this
machine maps lies inside the region, so the branch is genuinely not taken. What
does not exist yet is a run in which it is deliberately non-zero, so it is
weaker evidence than the counters beside it — which is the distinction this
project keeps between a counter that reports nothing happening and one that
nothing increments.
