# A spinlock that gives up, and what it ruled out

**Status: the bound is in and gated by a sabotage case. It did not find the
defect it was built for — and that is the useful result.**

## Why

A spin loop that never gives up turns a deadlock into silence, and silence is
the most expensive failure this project has. The machine stops, the log stops,
and what is left is a wedge report naming whichever static function happens to
precede the spin address. In this one-translation-unit kernel that name is
frequently wrong: both wedges examined this week reported `hw_task_slots`, a
function whose entire body is `return VIBEOS_HW_MAX_TASKS;`.

So `hw_spin_lock` counts, and past a bound that no critical section here comes
close to, it panics — which parks every core and prints a backtrace — naming
the lock, the waiting core and the function it is waiting in, the holding core
and the function that took it.

The names are captured at acquisition rather than resolved from addresses,
because a confidently wrong name is worse than a number. `hw_spin_lock_named`
takes `__func__`; the 34 acquisition sites pass it automatically.

The owner fields are cleared **before** the release, not after: a core taking
the lock the instant it is freed would otherwise have its owner overwritten by
the previous holder's tidying, and the report would name the wrong one. Same
shape as the exit ordering this kernel already carries a long comment about.

## What it ruled out

The `svc-press` wedge was the reason to build this: at twenty-eight megabytes
of anonymous mmap the machine went quiet, and four cores sitting in `pause`
loops looked exactly like a deadlock.

**It is not a deadlock.** The bound never fired. Under the same workload the
machine now reports a ring-3 page fault instead, and the wedge is a consequence
of that fault rather than a lock nobody released.

Two more things the same run ruled out, from a temporary trace of every
anonymous mmap:

* **Not memory exhaustion.** Free frames went from 104,572 to 98,735 of
  107,485. Reclaim never ran once — `scans=0`.
* **Not the reverse-map pool.** `nodes_peak` grew linearly at 64 nodes per
  256 KiB block, reaching about seven thousand against a pool of two hundred
  and fifteen thousand.

## What is left

`rip=0x800000013d`, and `.text` in that program starts at `0x8000000160`. The
program is executing bytes *before its own code* — its ELF header, mapped
executable by the first `PT_LOAD` — and the first thing those bytes do is write
to address zero. Control flow went somewhere it was never sent.

That is a much narrower question than "the machine wedges", and it is the next
one. Recorded here rather than folded into a guess, because three of this
project's longest investigations ended at a plausible pointer with no mechanism
behind it.

One thing to fix on the way: the trap line says `action=PANIC` and the line
after it says `ring3 fault: killing task, not the machine`. Both cannot be
true, and a log that contradicts itself is the thing this project checks for
first.
