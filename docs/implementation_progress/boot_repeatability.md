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

## What the walk dump found (2026-08-31)

A temporary probe printed the four entries of the page-table walk on any fatal
not-present user fault. It answered the question the trap dump cannot:

    va=0x8000001243 cr3=0x231f000 pml4e=0x040326e0 pdpte=0 pde=0 pte=0
    stopped_at_level=1

**The leaf is not missing - PML4 slot 1 is**, so the whole user window of a
running process is gone. And the value sitting in that slot is not anything this
code writes: the present bit is clear and bit 7 (PS) is set, which is illegal at
that level. Read as data rather than as an entry, `0x040326e0` is a pointer
into the kernel image. The same shape appeared at three different cr3 values
across boots (`0x040326e0`, `0x040336c0`, `0x040336f0`).

So a process page table is being freed and handed to some kernel allocation
that writes a pointer into it, while a task still runs on that CR3.

This also explains why every memory detector stayed silent through the hunt:
they watch frames mapped as *leaves*, and a page table is not a leaf. The
free-while-mapped check could not have caught this by construction.

**One candidate was fixed and was not it.** `execve` destroyed the
outgoing address space unconditionally, while `hw_task_exit` has asked
`hw_aspace_still_shared` since the wedge it was written for - so a
threaded process that execs freed tables its siblings were still running on.
That is a real defect and the fix is in (`hw_aspace_shared_by_other`,
asked by address space so exec and exit cannot answer differently), together
with a leak in
fork's error paths, which published a task slot as free without destroying the
address space it had already created. Neither closed this signature.

## Still open in the copy-on-write path (2026-08-31)

Separate from the two signatures above, and the more tractable of everything
here because it has a detector that names it:

    [MM] FREE_WHILE_MAPPED frame=0x237d000 still mapped by pid=0x5a
         during cow-fault mappers=0x2 owners=0x1

Two address spaces hold the frame and one reference is counted, so a `get` was
lost rather than a `put` duplicated. Three concurrency defects in this path were
found and fixed during P2 - publication order, release order, and two cores
resolving one fault with a compare-exchange - and the rate improved from 27/48
to the background. **This one is not closed.** It appears roughly once in
twenty-four boots, and it was reported as fixed once already on the strength of
a clean 48-boot run, which at this rate proves little.

The detector is trustworthy now: it compares mappings against references after
the release, so it is indifferent to a frame being reallocated under it - the
two predicates it had before were wrong in opposite directions.

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
