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

## What is left, and a wrong turn worth recording

The first version of this file said the remaining lead was a ring-3 page fault
at `rip=0x800000013d`, a program executing bytes before its own `.text`.

**That fault was not `svc-press`.** It is `svc-crash`, the service whose entire
purpose is to dereference null so the boot can prove the kernel survives a
crashing program, and it appears on every healthy boot. The identical fault
shows up with `svc-press` not running at all, which is what settled it, and the
crash record says so outright: `exe=/EFI/BOOT/SVC_CRSH.ELF`.

CLAUDE.md names this exact mistake - "check which program the task was running
before believing an address" - and it was made anyway, on a project where every
VibeOS-native program links at the same base. The crash recorder existed the
whole time and answers it in one line.

So the honest state of the `svc-press` wedge is that four explanations are now
excluded and none has replaced them:

* not a deadlock;
* not memory exhaustion;
* not reverse-map exhaustion;
* **and no fault of its own** - it goes quiet with nothing at all.

A second calibration was needed before the first of those could be claimed. The
bound is four hundred million spins, and the boot gate gives up after
forty-five seconds of silence: under TCG the bound might simply never have been
reached, in which case "it did not fire" would mean nothing. Rebuilding with
twenty million - reachable well inside the gate's patience, since virtio's
two-million bound fires there routinely - and rerunning `svc-press` produced no
deadlock either. That is what makes the exclusion real rather than a guess
about timing.

## A log that contradicted itself

Found while reading the same output. The trap dump printed the *trap model's*
decision, and the model has no notion of privilege level, so it answers PANIC
for a page fault. A branch a few lines later overrides that for a ring-3 fault
and kills the task instead. So a boot that correctly killed one program logged
`action=PANIC` and then, on the next line, `killing task, not the machine`.

Two statements, one false, and the false one is the one a reader reaches first
- which is precisely how `action=PANIC` was read as "the machine died here" for
a whole investigation. The override is decided before the line is written now,
and the model's answer is kept alongside it as `model=PANIC`, because a fault
the model would have panicked on and one it would not are different situations.
