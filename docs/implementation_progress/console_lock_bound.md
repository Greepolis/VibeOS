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


---

## The reporter that could not report

Written after the bound above found its first real failure, and after the tool
built to explain it turned out to have the same defect it was looking for.

`hw_lock_deadlock` announced a stuck spinlock by calling `serial_puts`, which
takes the console lock. Under `svc-press` the console lock is one of the jammed
ones, so the core that had *detected* the deadlock sat down in
`vibeos_x86_64_serial_lock` and the message never arrived. The wedge report
caught it in the act: `CPU#3 rip=0x4000cb5 hw_lock_deadlock`.

A lock reporter that can only speak while the locks are healthy reports nothing
on exactly the boots it exists for. It writes raw bytes now, taking nothing.
The output can interleave, which is the correct trade for the same reason the
console bound takes the lock rather than waiting: interleaving is caught by the
gate's first check, and silence is not caught by anything.

## What svc-press actually does

Not a wedge, or not only one. With the reporting unjammed the boot gate reports
`guest_still_talking` for thirty-nine minutes with `quiet_for=0`: the machine
emits 7.5 MB of `hw_ctx_check` dumps and never stops. Both verdicts have been
seen from the same workload, and which one appears depends on whether the
console lock jams before or after the flood starts. That is why this failure
was written up for so long as "the serial log fills with binary" - it is not
binary, it is one diagnostic repeating for ever, cut into fragments by cores
writing over each other.

The repeated fragments are the tail of that dump, and they are not random:
`rax=0x10a030c050e07000`, `rbp=0x10a030c010e07200`,
`rax=0x60f08010a030c050`. The high nibbles step by 0x20 - `10, 30, 50, 70,
90, a0, c0, e0` - so whatever the failing context is being read from holds
*structured* data, not the leftovers of a freed page. That is a different
question from "a context was zeroed", which is what the earlier, garbled
capture suggested, and both readings are recorded here because the difference
decides where to look next.

## Where this stands

The characterisation is much sharper than it was and the instruments now work,
which is the whole of what this stretch bought:

* the console lock cannot silence the machine any more, and says when it was
  taken by force;
* the spinlock bound fires and can speak while the locks are jammed;
* the trap dump reports what it did rather than what the model proposed;
* an execve that fails names its reason.

The defect itself is not fixed, and it predates all of this. It is outside the
memory-manager plan - it is not any of P0 through P7 - and what it blocks is
narrow and stated in [mm_swap_file.md](mm_swap_file.md): `freed_anon` stays
zero because nothing else runs this machine short enough of memory to make
reclaim evict a page.
