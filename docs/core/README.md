# The kernel core: making the portable half real

`kernel/core/` is initialised **after** the architecture layer has already done
all of its work, on its own structures. That one sentence is the whole problem,
and it has two consequences that are easy to state and expensive to live with:

- the portable kernel can be host-tested and has **no runtime role**, so every
  test it passes is a test of code the machine does not execute;
- `kernel/arch/x86_64/arch_hw.c` is where the machine actually lives, and it is
  9,995 lines holding the scheduler, the syscall table, exec, pipes, signals,
  the network glue, the interrupt controllers and the console, with no boundary
  between any of them.

Two kernels, one of which does not run. That is not a tidiness complaint: every
security-relevant defect found by review in this project has been in that file,
and the reason is structural. When everything lives in one scope with no
boundaries, a check that should apply everywhere is easy to forget in one place
and nothing notices until somebody goes looking.

## What this plan is not

It is not "split the big file". `arch_extraction.md` is already doing that, has
made three cuts, and continues independently. Moving nine thousand lines into
six files of fifteen hundred would leave the same two kernels, one of them
still dead.

This plan is about **which of the two runs**.

## The evidence this is worth doing

The number that makes the case is not the line count, it is this: between cut 2
and cut 3 of the extraction plan, `arch_hw.c` went from 8,359 lines to 11,166.
Not by anybody's decision - the I5 and I5b work was written into it one
bring-up at a time over three days, while the extraction document sat in the
repository saying the length of that file is the completion criterion.

That has now happened twice, at 633 lines and at 2,800. A file grows because
adding to it is the path of least resistance, and it will keep growing until
adding to it stops being easiest. Cutting it does not change that; giving the
new code somewhere better to go does.

## The shape of the end state

One kernel. `kernel/core/` owns the decisions - what a task is, when it runs,
what a syscall means, what a process may do - and `kernel/arch/x86_64/` owns
the mechanisms those decisions need: how to switch a stack, how to load a page
table, how to return to ring 3.

The test for whether a piece is in the right place is not "is it portable" but
**"would a second architecture need a different answer, or the same answer
differently implemented?"** A scheduler that picks the next task is the same
answer everywhere. A scheduler that writes `cr3` is not.

## Why now, and why not sooner

The roadmap put this after memory, scheduler and exec, "because it sits on top
of all of them". Memory and scheduler are largely closed and exec is stable, so
the timing is right for the first time.

Against that: there is an open memory-lifetime defect - an argv vector that
reads as the free-page poison, about one boot in six - and a large refactor on
a machine with an intermittent memory bug is how a project loses weeks, because
every failure becomes attributable to two things. **Phase C0 exists for that
reason and comes first.**

## The trap this plan must not walk into

There is already a second syscall dispatcher: `kernel/core/syscall.c` plus
`kernel/proc/process.c`. No real process reaches it - the live path is in
`arch_hw.c` - and it carries three known, uncorrected defects: thread creation
with no check on the caller, a process slot that is never freed, and a caller
identity read from an argument the caller supplies.

It is tempting to treat it as the foundation, because it looks like the
portable kernel this plan wants. It is not. It is what the portable kernel
looked like when nobody was running it, which is exactly the state this plan
exists to end. The rule is in `phases.md` as C1's gate and is repeated here
because it is the single most likely way for this work to go wrong:

> **Nothing may become reachable from ring 3 before the defect in it is fixed.
> Not "fixed afterwards". Not "fixed in the same change".**

## Documents

- `phases.md` — what each phase builds, what proves it, what turns it red.
- `invariants.md` — the properties that must hold across every phase, and the
  counter or assertion that watches each one.
