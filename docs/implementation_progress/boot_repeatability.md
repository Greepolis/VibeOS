# Boot Repeatability Progress

Status: **Open defect, pre-existing and untriaged.** Roughly one boot in twelve
fails, in two signatures. It predates the memory-manager rewrite by weeks and is
not caused by it.
Last review: 2026-08-31

## What fails

Two signatures, both intermittent, on an otherwise green build:

- **An instruction fetch from a page that is not present**, with `cr2` equal to
  `rip`, inside the same page the process was already executing from. Seen in
  `svc-flap`, a service whose whole body is a write and an exit - so the page
  vanished under a running process rather than the program jumping somewhere
  wrong. The boot gate reports it as
  `deliberate_ring3_faults=2_expected=1,unexpected_cpu_fault`.
- **A wedge**, at varying phases, reported as `missing:CLI_READY` after 45
  seconds of silence.

## Why it is filed separately

It was found while trying to certify phase P2 of the memory-manager rewrite,
and for a day it was assumed to be that phase's debt. It is not, and the
evidence is unambiguous in two independent directions.

**Locally**, the same 24-boot measurement was taken at three revisions:

| revision   | what it is                        | clean |
|------------|-----------------------------------|-------|
| `f45ab1d`  | the commit *before* P2 started     | 22/24 |
| `a714dbe`  | P2 step 2                          | 22/24 |
| `6510827`  | P2 complete                        | 21-22/24 |

**In CI**, the nightly `Boot Smoke x5 (flakiness gate)` job - five boots, all of
which must pass, which is this same criterion in miniature - has failed 27 times
out of 31 since the workflow was created on 2026-08-02, including nineteen
consecutive nights on `19ebeb74`, a commit that predates any of this work.

So the failure is older than the subsystem it was being blamed on, and older
than the gate that reports it.

## What it is not

Not the premature-free family. Across these runs the free-while-mapped detector
is silent, the stress service reports no defect, and `frames_leaked`,
`frames_double_put` and `poison_hits` are all zero. Those were the symptoms of
the three concurrency defects fixed during P2 (`e02d9dd`, `6510827`), and they
are gone: the boot rate went from 27/48 while those were live back to the ~90%
that every revision measured before them.

## What this cost, and the lesson

Phase P2's completion criterion was written as "48 boots with no failure". With
a background failure rate near 8% per boot, the probability of 48 consecutive
clean boots is about 0.02% - so that criterion could never have been met, by
this revision or by any revision in the project's history. It was measuring the
background, not the phase.

Two days were spent treating an unreachable number as a verdict on the work in
front of it. The rule worth keeping: **a criterion has to be checked against the
baseline before it is used to judge a change.** One measurement of the parent
commit would have said so immediately, and it was not taken until somebody asked
for it.

The number was not wasted, though, and that is the other half of the lesson.
Insisting on 48 boots surfaced three genuine concurrency defects that a 16-boot
gate would have passed over. The mistake was in what the number was allowed to
*conclude*, not in running it.

## Next

- Establish when this started. The nightly has never been reliably green, so the
  search goes back before `2026-08-02`; `scripts/dev/bisect-boot.sh` takes a
  revision and a count and always restores the tree.
- The `svc-flap` signature is the more tractable of the two: a page disappearing
  under a running process is a narrow claim, and the crash records
  (`crash` on the kernel console) already keep the registers and the executable
  name at the moment of the fault.
- `repeat-boot.sh` keeps the serial log of every failed boot, so a run leaves
  evidence rather than a count.
