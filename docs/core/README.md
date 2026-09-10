# The kernel core

Four requirements drive this plan, and they are not four separate plans:
**maintainable**, **modular**, **observable**, **fast**, with the size of
`arch_hw.c` as a symptom of the first two rather than a goal of its own.

Read them in this order:

- `architecture.md` — the shape, and how each of the four is *measured*.
- `phases.md` — what each phase builds, what proves it, what turns it red.
- `invariants.md` — the properties that must survive every phase, and the
  counter or assertion that watches each one.

## The state of things, measured rather than remembered

`kernel/arch/x86_64/arch_hw.c` is **10,087 lines in 19 sections**. The five
largest are the socket syscalls (1,654), the task table and scheduler entry
(1,363), the C-runtime syscalls (1,185), the kernel log (724) and the page pool
(685). The Linux ABI, counted across its four sections, is **4,169 lines — 41%
of the file**.

The portable half is about 19,000 lines across thirteen directories, and it is
no longer uniformly dead. That claim, which the first version of this plan was
built on, is now wrong in an important place: `hw_pick_next` calls
`vibeos_sched_policy_pick` and `vibeos_runq_pick`. **The scheduler phase this
plan was going to do has already happened.** What is still unreached is
`kernel/core/syscall.c` and `kernel/proc/process.c` — 2,592 lines of second
dispatcher that the arch layer names zero times.

## The three things that are actually wrong

**1. Adding to the monolith is the cheapest thing to do.** Twice, a session
whose stated purpose was shrinking that file grew it — by 633 lines once, by
2,800 the next time — while the document saying its length was the completion
criterion sat unchanged in the repository. A file grows until adding to it
stops being easiest. That is why this plan's success criterion is not a line
count. It is blast radius: how many files you must edit to add one thing.

It is **21 across six extension points** today, measured rather than asserted —
and the first measurement contradicted this plan, which had claimed two of those
points already cost one file. Nothing costs one file. The one driver that uses a
registry costs the same as the four that are hardcoded, because it still needs a
bring-up call. See `architecture.md`.

**2. Almost nothing reports whether it is healthy.** The kernel prints 25
distinct report tags and has **five** must-be-zero counters in the entire tree.
Every hard defect in this project's history was found by a counter or a
detector, and most subsystems have neither.

**3. Nothing here has ever been measured for speed.** Not once. The seams this
plan adds are indirect calls on paths that run millions of times a boot, and
there is no number to compare against — so any claim about their cost, in
either direction, would be invented.

## The trap, restated because it is the likeliest way to fail

`kernel/core/syscall.c` and `kernel/proc/process.c` look like the portable
kernel this plan wants. They are what a portable kernel looks like when nobody
runs it, which is the state this plan exists to end, and they carry three
uncorrected defects: thread creation with no check on the caller, a process
slot that is never freed, and a caller identity read from an argument the
caller supplies.

> **Nothing there may become reachable from ring 3 before those are fixed. Not
> afterwards. Not in the same change.**
