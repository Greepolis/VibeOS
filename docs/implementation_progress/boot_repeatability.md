# Boot Repeatability Progress

Status: **24 local boots clean out of 24** - but CI still reproduces the copy-on-write stress defect on a build containing every fix below, so this number is not the whole picture. See the correction dated 2026-09-02, from a long-standing ~1 in 8.

Six defects fixed. The first three were in the diagnostics built to hunt the
failure rather than in the kernel they were watching. The last three came from
CI and nightly logs rather than from another local sweep: a copy-on-write fast
path deciding on a fact its compare-exchange could not guard, a kernel address
handed to ring 3 as AT_BASE, and - the one that explains the rest - a panic that
stopped one core instead of the machine.

**24/24 is a good number and not a proof.** At the rate this measured
immediately before (23/24), a clean run of 24 happens about a third of the time
by luck alone. What is stronger than the count is that two of the three new
fixes are asserted deterministically - a host test for the copy-on-write window,
a ring-3 assertion for AT_BASE - and the third turns any future kernel fault
from a silence into a named panic.
Last review: 2026-09-01
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

## What it actually was (2026-08-31)

Three defects, found in one afternoon by reading failure logs instead of
re-running boots. Every one of them was in the instrumentation.

**1. The "who freed it" tag was a use-after-free.** `hw_free_page_why` released
a frame and then wrote the freeing function's name into word 1 of the page.
Between the release and that store another core can allocate the frame - and
word 1 of a page is **slot 1 of a PML4**, the user window. A diagnostic meant to
say who let go of a page was reaching into a live address space and replacing
its entire user half with a pointer to a string literal.

That is the signature recorded above and never explained: a ring-3 instruction
fetch faulting inside the page the program was already executing from, the walk
stopping at level one, a kernel pointer where a page-directory pointer belonged.
The dump that finally caught it printed the missing half - `cr3_state=page-table`,
`cr3_owners=1`: the frame was not free at all. A live, owned PML4 with
garbage in one slot.

The comment that had justified the write is worth keeping in mind. It reasoned
carefully about the poison check never probing word 1, and not at all about the
frame being reallocated in between. **Being careful about the wrong hazard reads
exactly like being careful.**

**2. The free-while-mapped walk was panicking the kernel.** It reads every live
task's page tables without the scheduler lock - deliberately, because it runs on
the frame layer's release path - so it can read a table another core has just
freed. A freed frame handed out again holds arbitrary bytes, some with the
present bit set and an address field pointing anywhere. Following one faulted
the machine, twice in twenty-four boots, and those failures were being counted
as the defect it was hunting. Every step is bounded now.

**3. The region pool had no lock.** Eight refused inserts in a boot whose peak
usage was twenty-nine descriptors out of two thousand - a pool nowhere near full
that had shredded its own free list. Two allocations racing both take the head
and both advance it. `fork` then failed with ENOMEM and the stress service
reported "fork for cow". The same mistake the frame layer had already made and
fixed, repeated in a layer written the day before.

## What remains

One wedge in twenty-four boots, and it has a lead rather than a mystery:

    #GP at vibeos_x86_64_task_enter (isr.S:294)
    then #GP at rip=0xe4e4e4e4e4e4e4e4

A task's saved register context contains a repeated fill byte, so restoring it
faults - and the panic handler then runs on the broken state and faults again.
`0xe4` appears nowhere in the source as a constant, so the next question is
what writes it: an uninitialised context, a recycled slot scheduled before its
creator finished filling it in, or a kernel stack being used after it was freed.
The task state machine's `SETUP` state and its tenancy counter exist for
exactly this shape and should be the first things consulted.

## The remaining wedge was virtio-net transmitting without a lock (2026-09-01)

Named by the wedge report rather than guessed at, which is what that tool is
for. A boot that went quiet at `busybox_cat` left this:

    CPU#0 rip=... vibeos_x86_64_virtio_net_send virtio_net.c:276
    CPU#2 rip=... hw_spin_lock  <- from hw_sys_munmap
    CPU#3 idle

and QEMU said the rest on its own stderr:

    qemu-system-x86_64: Guest says index 65535 is available

`virtio_net.c` had no lock on the transmit path. Every send uses one
staging buffer, one descriptor - `g_tx.desc[0]` - and one
`g_tx.last_used`, and publishes with a plain `g_tx.avail->idx++`
on a 16-bit field.

**This is the same defect virtio-blk had, in the same shape, never applied
here.** Two cores sending at once do not race over a window, they overwrite each
other: one core's frame lands in the other's descriptor, the available index
skips or repeats - 65535 is what a lost increment on a `uint16_t`
produces - and whichever core reads the used index first leaves the other
spinning on a completion that has already been consumed. The spin has a bound,
but 50 million pauses is far longer than the gate's quiet budget, so it presents
as a wedge rather than as the timeout it eventually becomes.

Interrupts stay on, as in the block driver: nothing in an interrupt handler
transmits, so this lock can never be wanted from one. The timeout path releases
it before returning, because holding it there would turn one late frame into a
permanently dead network.

**Fifth layer in this project to have shipped without a lock**, after the frame
allocator, the region pool, the page cache and the block driver.

Worth separating from it: the crash record that appears in every boot is
`svc-crash` dereferencing null **on purpose**, to prove a ring-3 fault
kills the task and not the machine. It is a gate, not a defect, and its absence
would be the failure.

## The copy-on-write failure is a copy that was replaced (2026-09-03)

Deduced from four CI logs, without a new run, and it narrows the search from
three causes to one.

**The child always reads exactly the parent's value**, never garbage:
`found 0xd7 expected 0x28` with
`after = 0x28` means `before = 0xd7`. Four logs, four
times, the same relationship.

And `op_cow` checks the *parent's* page separately, after reaping the
child - "the child's writes reached the parent". **That check has never fired.**

Put together: the parent's page is intact, so the child was not writing into it,
so the child did have a private frame. And the private frame holds a clean copy
of the original, so the child's writes are gone from it.

**That is not "the copy never happened". It is "the copy was made and then
replaced."** One of the three causes, chosen by evidence already in hand.

What that implicates: only `clone_one` - fork - sets the
copy-on-write bit, and the child of `op_cow` never forks. So something
is putting that bit back on a page that had already been made private, and the
next fault copies the original over the child's work.

`vibeos_pageinfo` answers this directly: svc-stress now samples the
frame before the fork, after it, after the child's write and at the check, and
prints which of the three cases it was. **That output needs a push to reach
CI** - the artifacts analysed here predate it.

## The 0xe4 fault is not attributed to anything (2026-09-02)

**Nothing in this file explains it, and three attempts to say otherwise were
wrong.** Recorded plainly so the next attempt does not start from a conclusion.

    rip=0x57da77  cr2=0xe4  err=0x5   (user read, present, supervisor-only)

What is known:

- It appears in **two different programs**, `BUSYBOX.ELF` and
  `THREADS.ELF`, at the **same rip**. Both are static musl binaries, so
  that address is almost certainly a musl internal function rather than program
  code - which is why the same number turns up in unrelated programs.
- The faulting access is a read of a near-null pointer plus 0xe4. So a pointer
  that should have been valid came back as zero, or nearly.
- It is **deterministic in its path** - the same rip, the same offset, every
  time - and **rare in occurrence**, a few times in twenty-four boots.
- It appears with the X-P2 cache mapping **on and off**.

What is not known: anything about the cause.

The three wrong conclusions, kept because the pattern is the lesson:

1. "The signature is gone" - from grepping the rip in a boot that had wedged and
   had no crash record at all, so the rip could not have appeared.
2. "The cache mapping causes it" - from 21/24 twice against a baseline reported
   as better than it was.
3. "Turning the mapping off removes it" - from one clean run of twelve, against
   a defect that appears a few times in twenty-four.

Every one of them read an absence over too short a run as proof. The next
attempt needs the faulting instruction disassembled out of the binary the task
was running, not another ratio.

## A panic stopped a core, not the machine (2026-09-01)

**This is the explanation for the silent-wedge family, and it had been visible
in every log all along.**

`hw_panic` ended with `for (;;) { cli; hlt; }` - which stops the
calling core. Its own message said so: *"halting on cpu 2"*. For weeks that line
was read as though it said "halting".

The other three cores carried on. The log kept moving. The machine went quiet
some seconds later, at a place with **no connection to the fault**, and every
investigation started from that place. That is why so many of them ended at a
plausible-looking pointer with no mechanism behind it: the evidence and the
failure were separated by however long it took the survivors to need the core
that had died.

A nightly log showed it plainly once the question was asked: a ring-0 #GP on
cpu 2 with `action=PANIC`, followed by four hundred more lines of healthy
boot, then silence.

**The fix.** A panic sets a flag and every core parks itself on its next
interrupt. Deliberately not an IPI: every core already takes a timer interrupt a
hundred times a second, so this needs no new vector, no acknowledgement protocol
and no timeout to get wrong. A core inside a syscall with interrupts masked
parks when it returns to ring 3; a core that never returns was stuck either way.

Proved by causing one. cpu 3 panics, and cpus 1, 2 and 3 report parking.

**The gate now calls it a panic, not a wedge**, and carries the reason:

    reason=missing:<why> (waiting for CLI_READY) verdict=guest_panicked

Calling a panic a wedge sent the next person looking for a hang. That mattered
much more than it sounds, for exactly as long as a panic left three cores
running.

## A kernel address handed to ring 3 as AT_BASE (2026-09-01)

Found from the nightly's artifacts, which carry the kernel ELF and the guest
binaries - so a faulting address could be symbolised against **the binary the
task was actually running**, which is the rule this file has been repeating
without being able to follow it.

PIE.ELF faulted at `0x4046220` in `_start_c` (`rcrt1.c:139`),
musl's self-relocation for a static position-independent binary. `nm` on the
kernel places that address inside `g_kernel_log`.

`p->interp_base` was written in exactly one place - the branch that maps an
interpreter - and read unconditionally when the startup block was built. A
program without an interpreter therefore shipped whatever was already in that
field to ring 3 as `AT_BASE`: uninitialised kernel stack for an
`execve` (`hw_proc_t np` is a local), or the previous tenant's value
for a recycled task slot. A static PIE relocates itself against `AT_BASE`,
so musl took a stray kernel pointer as its load address and dereferenced it.

Intermittent for the honest reason that it depends on what was left on the
stack - which is why it read as "position_independent_binary_did_not_run" some
nights and nothing at all on others. It is also a kernel address disclosure to
ring 3 whatever the program does with it.

Zeroed in `hw_proc_create`, which owns the contract - "every caller must
remember to clear a field" holds until somebody adds a caller. The ring-3 ABI
self-test now **asserts `AT_BASE` is absent** for a program with no
interpreter, every boot, deterministically.

## The copy-on-write defect: one window closed, the defect still open (2026-09-02)

**Correction.** This section previously said "found and closed". It was not.

A nightly on a build that *contains* the fix - its exec line carries
`cache_audit_checked`, which only exists from `d9c0049` -
failed again with the same signature:

    STRESS_FAIL: the child's own copy-on-write page at offset 0: found 0x07 expected 0xf8

`after` is 0xf8, so `before` is 0x07: the child read the
*parent's* value after writing its own across the whole page. And
`COW_STATS` reported `exclusive_lost=0`, so the window fixed
below was never even entered in that boot.

The window was real - the host test proves it deterministically, and it stays -
but it was not this one.

**"Closed" was inferred from 24 boots with no stress failure.** That is an
inference from an absence, on a defect this same file records as appearing about
once in twenty-four. This document exists largely to record what that habit
costs, and it happened again here.

What the evidence says now, across three occurrences: the child writes
`after` to all 4096 bytes and then reads `before` back at
offset 0. That is not a lost write - it is the page being **replaced by a fresh
copy of the original** after the child had already written into it. So the
question for the next attempt is not "which write was lost" but **what resolves
a fault on that page a second time**, and how the entry still looks
copy-on-write when it does.

Replay seeds recorded: `76419299086`, `97300336535`.

## The window that was closed (2026-09-01)

Closed by CI logs, not by another boot sweep. The nightly's clang/Release job
had failed with

    STRESS_FAIL: the child's own copy-on-write page at offset 0: found 0xf3 expected 0x0c
    STRESS_FAIL: replay with EFI/BOOT/SVC_STRS.ELF 5498867...

on a build that predates all of this session's work, which settled that it was
pre-existing rather than a regression. Decoding the two numbers is what pointed
at the mechanism: the stress service writes `after` to every byte of the
page and reads it back, and `found=0xf3` is exactly `before` - the
*parent's* value. The child was reading the parent's frame after writing its
own.

### Two defects, one page of code

**1. The fault handler's exclusivity check rests on a fact its
compare-exchange cannot guard.** `vibeos_vmspace_fault` has a fast path:
if the frame has one owner, take the write permission back in place rather than
copying. It reads `vibeos_frame_owners(phys)`, then compare-exchanges the
page-table entry.

The exchange guards the **entry**. The decision depends on the **reference
count**. The entry does not encode the count, so the two are unrelated, and a
fork on another core can take a reference in the window while leaving the entry
byte for byte identical.

**2. `clone_one` is what walks into that window.** Its already-copy-on-write
branch does no compare-exchange at all - it has nothing to change, since the
entry is already marked - so it takes a reference on the frame and maps it into
the child without touching the source entry. There is nothing for the fault
handler's exchange to notice. It also read the entry with a plain load, in a
file whose entire design is that every page-table word is read once and acted on
atomically.

The result is a process holding a **writable** mapping of a frame another
address space holds copy-on-write: one side's writes landing in the other's
private page. When the frame is later reused, the same defect shows up instead
as a page holding a value from a completely different round - which is the local
failure, `found 0x5b`, a `before` value from another iteration.

### The fix, and why it can stay fast

The fast path keeps its optimisation and re-checks after the store: if the frame
gained an owner while the entry was being widened, the entry is put back and the
copy path taken instead. Undoing is safe precisely because no TLB has seen the
writable form - it is installed and withdrawn before any `invlpg`, and the
faulting instruction has not been retried. `clone_one` now reads its entry
atomically. `cow_exclusive_lost` counts the window being hit, because it
was silently common and must never become unobservable again.

### It is asserted deterministically, not through boots

This is the part worth keeping. A race that appears once in some number of boots
cannot be gated by a boot, and every earlier claim in this file that such a
thing was "fixed" rested on a handful of green runs.

`vibeos_vmspace_set_race_hook` is a seam that lets a host test occupy the
window on purpose: it fires between the ownership decision and the store, and
the test takes exactly the reference `clone_one` would. The assertion is
that the frame shared during the fault is not the frame the address space can
afterwards write. Removing the re-check turns it red with

    FAIL:a frame shared during the fault was made writable in place

every time, on the host, in under a second.

Rate after the fix: **23/24, with no stress failure in any of them**.

## Superseded: the copy-on-write path (2026-08-31)

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
