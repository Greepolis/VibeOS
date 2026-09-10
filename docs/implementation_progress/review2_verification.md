# The second external review, checked against the code

Eight findings. Every one was verified before being accepted or dismissed,
because a review is a detector and the first question about a detector is
whether it is right.

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
