# The first performance measurement this kernel has ever had

C0 of the core plan. Before this there was no benchmark, no timing, no budget
and no regression gate, so every statement anyone could make about this
kernel's speed — including a reassuring one — was invented.

The refactor adds indirect calls to the syscall, fault and switch paths. Without
a baseline it could not have been shown *not* to have cost anything, in either
direction, since an unmeasured reassurance is worth exactly what an unmeasured
alarm is.

## What is measured

Three hot paths, instrumented at one site each:

| Path | Where | What |
|---|---|---|
| syscall | `vibeos_x86_64_syscall_dispatch` | count, total cycles, **minimum** |
| page fault | the vector-14 call site in `vibeos_x86_64_isr_handler` | count, total cycles, **minimum** |
| context switch | `hw_task_load_cpu_state` | count only |

The syscall is timed at the *dispatch* rather than inside the handler, because
what the refactor changes is the classify/marshal step an ABI translator adds —
timing only the handler would miss precisely the thing being introduced.

The fault is timed at the call site rather than inside `hw_handle_cow_fault`,
which has five returns: at the call site the measurement is one pair of reads
and cannot miss an exit path. Only *resolved* faults are counted; an unresolved
one ends in a kill or a panic, and folding those in would make the number drift
with how often the machine crashed.

The switch is counted and not timed. That function *is* the switch — it ends by
installing `cr3` and returning into the next task — so there is no second point
at which to stop a clock.

## Two things the first measurements corrected

**The mean is not a baseline.** The first boot read **46 million cycles per
syscall**. That is not dispatch cost: a syscall that blocks is timed across the
block, so the number was reporting how long `waitpid` waited. It would have
moved with anything that changed scheduling — the opposite of what a performance
ratchet is for.

The **minimum** replaced it. A blocked task cannot lower a minimum, and an added
lookup or lock on the path raises it. The syscall minimum is around **600
cycles**, against a mean sixty thousand times larger.

**The counts are not deterministic.** They were going to be described as such,
and ratcheted on that basis, on the reasoning that the same boot does the same
work. Six boots:

| boot | syscalls | syscall min | faults | fault min | switches |
|---:|---:|---:|---:|---:|---:|
| 1 | 1416 | — | 396 | — | 516 |
| 2 | 1381 | 728 | 378 | 807,972 | 497 |
| 3 | 1383 | 517 | 366 | 736,387 | 461 |
| 4 | 1370 | 627 | 353 | 799,208 | 458 |
| 5 | 1352 | 602 | 335 | 622,106 | 426 |
| 6 | 1370 | 623 | 369 | 595,414 | 453 |

Syscalls move 4.7% across the range, faults 18%, switches 21%, and the syscall
minimum itself 41%. Nothing here can carry a tight ratchet, and a threshold
written from the first sample would have been wrong.

This is the memory manager's P2 mistake caught from the other side: that phase
was judged for a day and a half against "48 boots with no failure", a criterion
measured against a background nobody had characterised. Taking the second
measurement *before* writing the criterion is what this project's own rule asks
for, and it is the only reason the false claim did not ship.

## The trend that was not one

Worth recording in full, because it happened while this file was being written.

After five boots the counts were decreasing monotonically — 1416, 1381, 1383,
1370, 1352, and faults 396, 378, 366, 353, 335. A monotone run of five is not
what scatter looks like, and this file said so: it named a suspect
(`qemu-cli-logdisk.img`, the one piece of state I5b deliberately keeps across
boots, whose attach scan grows with it) and called for a five-boot sweep with
the image deleted to settle it.

The sixth boot went back up. Faults 335 → 369, switches 426 → 453.

So it was scatter, and the paragraph explaining it was a mechanism invented to
fit five points. Nothing was wrong with the reasoning except that it started.
The rule this project already has for detectors — *the first question is whether
the detector is right, and it is never answered by a count* — applies to a
baseline reading its own numbers, and five samples on a machine with 20% spread
is a count.

The log disk remains a reasonable thing to test for drift. It is no longer
evidence of any.

## What is gated

Only that the counts are non-zero.

That is deliberately weak and it is the assertion this project keeps needing.
`VIBEOS_BLK_TIMEOUT` was defined, printed, asserted by the gate and produced by
no driver. A disk-interrupt counter read zero for an hour because its dispatcher
hook had never been added, and sent four measurements in the wrong direction.
**A counter nothing increments and a counter reporting nothing happening are
indistinguishable**, so before any of these numbers can be reasoned about,
something has to establish that they can move at all.

Also gated: cycles accumulated without events, or events without cycles — half a
pair is worse than neither, because a mean built from one of the two reads looks
like a measurement.

The ratchet is deliberately *not* here. It needs a characterised spread, and the
six boots above are the start of collecting one rather than the end.

## Cost of the instrumentation

Two `rdtsc` and one atomic add per event; the minimum uses a compare-exchange
loop, which by construction cannot spin, since each retry means the value
strictly decreased. Under TCG that is far below the cost of the event being
measured.

One boot in the sweep reported `stress_run_did_not_finish` before the minimum
tracking was added, and the boot before it had passed. Two boots decide nothing
on a machine with a known intermittent failure, and the four boots since have
all passed with the instrumentation in — so it is not attributed to the
instrumentation, and it is not attributed to anything else either.

## What this says about the next phases

`architecture.md` states that registration must be resolved once and not per
call, and that an indirect call may not be added to the three hot paths without
a measurement showing what it cost. That measurement now exists, and its first
lesson is about its own limits: on this machine, under TCG, the noise floor is
wide enough that a seam adding a handful of cycles will not be visible. What
*will* be visible is a lock or a per-call lookup, which is the failure the rule
was written for.

The fault minimum is around **700,000 cycles**, which is large. The copy-on-write
fault sends a TLB shootdown IPI and waits for the acknowledgements, so most of
that is likely the round trip. Likely, not measured — under TCG the emulated cost
of an IPI bears no fixed relation to a real one, and this project has a rule
against reasoning about a number like that without a second source.
