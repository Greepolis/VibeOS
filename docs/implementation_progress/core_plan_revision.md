# Revising the kernel core plan

The plan in `docs/core/` was written, challenged four times, and rewritten end
to end against four requirements: maintainable, modular, observable, fast, with
the size of `arch_hw.c` treated as a symptom rather than a goal.

This file records what the revision changed and, more usefully, **what
measuring found that reading had not**.

## The criterion was wrong, and had already failed twice

The first plan's completion criterion was the length of `arch_hw.c`. That
criterion has been used twice in this project and failed both times: once the
file grew by 633 lines during a session whose stated purpose was shrinking it,
and once by 2,800 during the I5/I5b work — both times while the document naming
its length as the criterion sat unchanged in the repository.

A file grows because adding to it is the path of least resistance. Cutting it
does not change that. So the criterion is now **blast radius**: how many files
you must edit to add one thing of a known kind.

It is already a controlled experiment rather than a theory:

| To add… | Files edited today |
|---|---|
| a filesystem | 1 (`vibeos_storage_register`) |
| a block driver | 1 (`vibeos_blk_register`) |
| a character device, input device, network interface, display, syscall, or ABI | `arch_hw.c` |

Four filesystems and four block drivers arrived without their layers being
edited. Everything else arrives by making the monolith bigger. The rule that
follows was rediscovered independently **eight times** — there are eight
registration seams in the tree, each invented separately by somebody solving one
problem — and never named until now: *a subsystem is extended by registering
into it, never by editing it.*

## Three things measurement found that the previous plan had wrong

**The scheduler phase is already done.** `hw_pick_next` calls
`vibeos_sched_policy_pick`, and falls back to `vibeos_runq_pick` rather than
idling — deliberately, because a task the policy was never told about is an
admission bug, and refusing to run it would turn that bug into a hang instead of
a slower machine. The plan had it as a future phase. It is recorded as done
rather than deleted: a plan that quietly loses a finished phase cannot be
audited.

**Observability is much thinner than the tooling suggests.** The kernel prints
**25** distinct report tags and has **five** must-be-zero counters in the whole
tree: `frames_leaked`, `medium`, `double_free`, `bad_unlocks`,
`illegal_transition`. Against 48 sabotage case files and a 2,020-line boot gate,
that is not a tooling gap, it is a coverage gap — most subsystems report what
they did and nothing about whether it went wrong. Every hard defect in this
project's history was found by a counter or a detector, so observability got its
own phase, placed ahead of every phase that moves code.

**Performance has never been measured. Not once.** No benchmark, no timing, no
budget, no regression gate. The architecture adds indirect calls to paths that
run millions of times a boot, and with no baseline the refactor could not be
shown *not* to have cost anything — in either direction, since an unmeasured
reassurance is worth exactly what an unmeasured alarm is. The baseline turns out
to be cheap to start: the boot gate has been recording `phase_history` since it
existed and nothing has ever read it.

## The structural claim that reordered the plan

`arch_hw.c` measured: **10,087 lines in 19 sections**. Every section now has a
destination in `docs/core/architecture.md`. Summed, **7,726 lines leave** and
about 2,360 stay — the parts where a second architecture would need a different
answer, not the same answer implemented differently.

The largest single block that leaves is the Linux ABI: the syscall layer (679),
the socket syscalls (1,654), what a program needs once running (651) and what a
C runtime asks for before it runs (1,185). **4,169 lines, 41% of the file** —
and it is exactly what the next refactor on the roadmap has to move anyway.

So the file cut and the multi-ABI syscall layer became one phase instead of two.
Splitting them was the previous plan's worst decision: cutting first produces
four smaller files with the same shape, which all get rewritten when the ABI
interface arrives. Building the interface first and moving the code through it
does the work once, and is the only version in which the cut is *verifiable* —
code that moves behind an interface either satisfies it or does not compile.

An ABI is a registered translator (`classify`, `marshal`, `result`), never a
branch in the dispatcher. Three ABIs as three branches is three copies of every
permission check, and the checks are where every security defect in this project
has been found.

## What is now watched that was not

`check-reachable.py` is written and running: for every function in the portable
kernel, is it named anywhere outside the file that defines it? It reports
`unreached=12 baseline=27`, ratcheted downward only.

It is a deliberately weaker question than "is it reached from the boot path". A
real call graph would have to understand function pointers, which every
registration seam uses, so it would report the frame layer's lock and the task
view as unreachable — the kind of confidently wrong answer that teaches people
to ignore a check. "Nobody names it" is coarse, almost never wrong when it
fires, and catches every case this project has actually had: a portable
dispatcher no process reaches, a scheduler policy the boot path did not consult,
a page cache with no caller, four filesystem drivers that had never parsed a
byte.

The two entries that matter are `kernel/core/syscall.c` (1,477 lines) and
`kernel/proc/process.c` (1,115). The arch layer names `vibeos_syscall` **zero**
times. That code carries three uncorrected defects — thread creation with no
check on the caller, a process slot that is never freed, and a caller identity
read from an argument the caller supplies — and the plan's hardest gate says
nothing there may become reachable from ring 3 until they are closed. Not
afterwards; not in the same change.

Three more checks are specified and not yet written: `check-subsystem.py` (the
seven parts), `check-blast-radius.py` (the table above, ratcheted) and
`check-chokepoints.py` (each security check declares its call-site count, so a
second site is noticed rather than forbidden).

## The rule this revision is itself an instance of

Every correction above came from running something, not from reading. The
scheduler was assumed unreached until `grep` said otherwise; observability was
assumed adequate until the must-be-zero counters were counted; the ABI's share
of the file was a guess until the section banners were measured against line
numbers.

That is the same rule the case files keep arriving at from the other direction:
*when a detector fires, the first question is whether the detector is right, and
it is never answered by a count.* A plan is a detector for what is wrong with a
codebase, and this one was firing on the wrong thing.
