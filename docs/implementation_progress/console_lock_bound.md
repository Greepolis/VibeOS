# The console lock, and why the machine goes quiet

**Status: the bound is in and the counter is gated. The `svc-press` failure is
now understood well enough to describe, and is not yet fixed.**

## The one lock that cannot report itself

Every other lock in this kernel panics when its bound fires. This one cannot: a
panic prints, and printing goes through the very lock whose state is in
question. So it takes the lock anyway, records that it did and who was holding
it, and lets the machine carry on talking.

That trades a correct line for a possibly interleaved one, and it is the right
trade twice over. Interleaving is *detected* — `interleaved_lines` is the boot
gate's first check, precisely so a garbled line is caught and named. Silence is
not detectable at all, and it is the failure where every piece of evidence is
missing rather than wrong.

`[CONSOLE] MUSTBEZERO bad_unlocks=… stuck=… stuck_owner=…` is the line; the
gate fails on a non-zero count and on the line being absent.

## What it explained

`svc-press` allocates 256 KiB at a time and touches every page. At a hundred
and twelve blocks — twenty-eight megabytes, not the twenty this was written up
as — the machine goes completely quiet.

Four explanations were excluded first: not a deadlock on `hw_spin_lock` (the
bound was lowered to twenty million and still did not fire), not memory
exhaustion (104,572 free frames down to 98,735 of 107,485, with reclaim never
running), not reverse-map exhaustion (seven thousand nodes against two hundred
and fifteen thousand), and no fault of its own.

Lowering *this* bound to ten million changed the machine from silent to
talkative, and what came out was a `hw_ctx_check` report — `cr3_by=`, `exe=`,
`rip=0x0`, `cs=0x0`, `rflags=0x0` — a task whose saved context is **zeroed**,
refused entry by the guard that exists to catch exactly that.

It came out interleaved, because taking a held lock mid-line is what this bound
does. That is the cost, and it bought the first piece of evidence this failure
has produced in its existence.

So the shape is: a task acquires a zeroed context, `hw_ctx_check` refuses to
enter it and reports under the console lock, and the machine stops there — with
the console lock held. Everything else then piles up behind it, which is why
the symptom is silence rather than a panic.

The original wedge report said this and was not believed. It put CPU#2 and
CPU#3 in `hw_ctx_check` and CPU#0 and CPU#1 at an address whose nearest
preceding symbol was `hw_task_slots` — a function that returns a constant.
`hw_ctx_check` was right; the other two are almost certainly the `pause` loop
in `vibeos_x86_64_serial_lock`, which links early and therefore sits at a low
address. The lesson CLAUDE.md draws about nearest-preceding-symbol naming cuts
both ways: a name that looks absurd is worth discarding, and a name that looks
plausible is worth checking rather than dismissing wholesale.

## What is not done

Why a task's context is zeroed under heavy anonymous mmap. That is the actual
defect and it is untouched.

And `hw_ctx_check` prints a dozen fields from the task table, including
`exe_path` and `vibeos_task_last_why(slot)`, while holding the console lock
with interrupts masked. If any of those reads faults — plausible, since the
task it is describing is already corrupt — the machine stops inside the
critical section, which is exactly the observed state. Making the report safe
to run against a task that is known to be broken is the next step, before the
cause is chased.
