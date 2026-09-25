# munmap used to hand back a frame other cores could still reach

The source admitted this for as long as it existed. The comment at the end of
munmap said a shootdown "belongs here too, and is deliberately absent", gave the
reason, and stopped there. An external review found the admission and was right
to call it a defect rather than a note — the first review of six to land in the
live path.

Its line numbers pointed at the copy-on-write fault handler rather than munmap.
The mechanism it described was exactly right, which is why the location did not
matter:

```
CPU 0                         CPU 1 (sibling thread, same address space)
munmap(P)
  clear PTE
  invlpg          <- this core only
  frame_put       -> allocator
  ...reallocated                 still holds VA -> F in its TLB
                                 writes through it -> corrupts the new owner
```

## Verified before being believed

`vibeos_vmspace_unmap` calls `g_be.invlpg(va)` — one core — and then
`vibeos_frame_put`. Both confirmed by reading, not remembered.

One thing the review did not have: the harm is not entirely silent. Freed pages
are poisoned and the poison is checked at hand-out, and `poison_hits` is a
must-be-zero the gate asserts. So the *free → stale write* window is covered.
What is not covered is **free → reallocate → stale write**, where the poison
check has already passed. Narrower than the review's diagram, and real.

## Why the obvious fix stays rejected

A synchronous shootdown was tried here and measured: **two runs in twenty-four**
failed with `tlb_acks below shootdowns` — a core that never answered. `syscall`
clears IF (SFMASK is 0x200), so a core inside a system call cannot take the IPI
until it returns to ring 3, and munmap runs far more often than fork; the stress
run alone calls it 120 times. Trading a rare correctness gap for a frequent
stall is the wrong trade.

## The third option, which the comment never considered

The comment weighed *synchronous shootdown* against *nothing*. The review
proposed deferred reclamation, and the constraint that killed the shootdown —
IF masked during a syscall — does not apply to it, because nobody waits for
anybody.

The frame is parked and released once every **other** core has loaded CR3. Two
facts make that a valid quiescence point, and both were checked rather than
assumed, because the argument dies silently if either changes:

- this kernel enables **no PCID** and marks **no page global**, so writing CR3
  flushes that core's whole TLB;
- every context switch loads CR3 unconditionally in `hw_task_load_cpu_state`.

Both are now named in `hw_cpu_t.cr3_generation`'s comment, where somebody
enabling PCID would have to read them.

This is a pattern here rather than an invention: `dead_kstack_base` already
parks the kernel stack of an exited task until its core is provably running on
another.

## The hole in the first version, in exactly the case that matters

`hw_schedule` returns early when the next task is the current one. So a core
running a single thread never reloads CR3 on its own — and a sibling thread
spinning on another core is **both** the thing holding the stale translation
**and** the thing that would never advance.

Frames would have piled up until the quarantine overflowed and fell back to the
racy release, precisely under the workload this was written for. The easy case
would have been fixed and the hard case would have degraded to the old
behaviour, and no green boot would have told them apart.

Closed by having every core flush on its next timer tick while anything is
parked — the cost is paid only while frames are waiting, and a TLB flush per
tick is what a context switch already does on a kernel with no PCID.

| | before | after |
|---|---:|---:|
| live peak, slots of 512 | 175 | ~45 |

## Measured

Ten boots, four vCPUs:

| | |
|---|---|
| frames deferred | ~320 per boot |
| released | **equal to deferred, every boot** |
| overflow | **0, every boot** |
| live peak | 35–66 of 512 |

`frames_leaked` stays zero, which is a separate assertion and the one that would
catch the quarantine holding on to something.

## What is gated, and what is not

*Superseded by M-061 below: the assertion described here is gone, because since
then whether a boot parks anything depends on timing.*

`munmap_never_deferred_a_frame` asserts `deferred` is **non-zero**. The defect is
a race that a handful of boots cannot produce on demand, and every earlier claim
in this project that such a thing was fixed rested on a handful of green boots.
What a boot *can* assert is that munmap took the safe path at all — a mechanism
that silently stopped running would otherwise leave every boot green with the
defect exactly as it was. Same argument that put `tlb_shootdowns` under the gate.

`overflow` is deliberately **not** asserted zero. Falling back is precisely what
the kernel did before the quarantine existed, so it is never worse than the
status quo; the number says how much of the window is still open, which is worth
reporting and wrong to fail on.

Sabotage: unregistering `release_deferred` gives
`reason=invariant_failed:munmap_never_deferred_a_frame,tlb_quarantine_never_released_anything`.

And it caught an omission of its own on the way: the three new reasons had no
case file, so `check-assertions-covered.py` went red at 111 against a baseline
of 108 — the check that matches every reason the gate can emit against the
sabotage cases, doing its job on the commit that added three.

# M-061: parked only when somebody could still hold it

The quarantine parked *every* unmapped frame until every other core had loaded
CR3 - including frames of a single-threaded process that no other core had ever
run. Nothing exposed that until svc-press (M-060) unmapped 64 MiB in a loop: the
512 slots filled faster than the other cores flushed, and each frame past that
was leaked on purpose (H-015). `tlbq_overflow=2478`, `userland_frames_lost=2486`
- ten megabytes gone for the rest of the boot.

Only a core that has the address space *loaded* can hold a translation from it.
So each core now publishes what is in its CR3 (`hw_write_cr3`), in two fields,
because the answer must be conservative in both directions:

- `loading_cr3` is written **before** the load, with a full fence. The unmap
  clears the entry (a locked compare-exchange) and then reads this; the loading
  core writes it and then walks the tables. One of the two always sees the
  other: either the quarantine sees the core coming, or the core's walk finds
  the entry already gone.
- `loaded_cr3` is written **after** the load. A core switching *away* is still
  seen as holding the space until its flush has happened.

A frame is released at once when no other core has the space in either field,
and otherwise parked against only the cores that do - the drain no longer waits
for cores that had nothing to flush.

Measured: svc-press's 16,856 unmaps are released immediately apart from a
couple of dozen, `tlbq_overflow=0`, frames lost 8 (ceiling 64). The thread tests
still park 17-36 frames a boot, the case the quarantine was written for.

## The gate had to change, and why

Whether a boot parks anything now depends on whether a sibling thread happened
to be running on another core at the instant of an unmap. Two boots in ten parked
nothing - correctly - and `munmap_never_deferred_a_frame` failed them. An
assertion on a timing property is a gate people learn to ignore, so it is gone.
In its place, two things that do not depend on timing:

- `munmap_bypassed_the_quarantine`: munmap reached the decision at all
  (immediate + deferred + overflow is non-zero). svc-press unmaps 64 MiB every
  boot, so zero means the hook has gone.
- `TLBQ_SELFTEST`: after userland, the kernel releases one frame against a
  space another core has loaded - it must park and then drain - and one against
  a space nobody has loaded - it must go back at once. Both constructed on the
  real per-core fields, so the decision is exercised every boot rather than
  when the scheduler happens to arrange it.

Cases: `scripts/dev/cases/mm-tlbq-holders.txt`.

## What it does not prove

The ordering of the two publications is not gated. Writing `loading` after the
load, or `loaded` before it, opens a window of a few instructions in which a
stale translation is missed - a race no boot produces on demand, which is the
same limit the original defect had. It is argued in the source, not tested.

# A second gap, in the mechanism that was supposed to close the first

The same reviewer came back with a finding that is distinct from the munmap one
and survives its fix. They were right, and it is worse than they stated.

`hw_tlb_shootdown` counted a timeout, logged it, and **returned**. Every caller
has already narrowed a permission or removed a mapping by the time it runs, so
returning quietly means a core keeps writing through a permission that has been
revoked — into a page a fork has just shared, with no copy-on-write fault.

The part the review understated: **no caller could have checked.** The backend
hook is `void (*shootdown)(uint64_t root_phys)`. There is no channel. Its five
call sites in `kernel/mm/vmspace.c` are not failing to check a return value;
there is no return value to check.

## Why the fix that closed munmap does not transfer

Worth stating explicitly, because the two findings look like one:

| | munmap gap | shootdown timeout |
|---|---|---|
| resource in danger | a **frame**, about to be reissued | a **permission**, already narrowed |
| can it be parked? | yes — that is the quarantine | no — there is nothing to hold |
| can the operation be undone? | not needed | no: the entry must be written *before* the shootdown is asked for, or there is nothing to invalidate |

So "do not complete the operation" is not available by the time the timeout is
reached, and deferred reclamation has nothing to defer.

## What was done

The only remaining choices were to keep waiting or to stop. A core that has not
answered in two hundred million spins is not about to, so it panics, naming the
reason. That is this project's own position in its own words — *the difference
between a machine that stops and a machine that says why* — and a silent
memory-corruption path is strictly worse than a named halt.

**Recorded as untested.** Making a core genuinely fail to answer for two hundred
million spins is not something this harness can arrange, and lowering the bound
to force it would test a different machine. What is known: `tlb_timeouts` reads
zero on every boot of this tree, and it is a counter that has *moved*
historically — about three boots in thirty-two, back when the shootdown
broadcast to every core instead of only those running the address space — so it
is not one of those numbers that has only ever been zero.

**If it ever fires, do not soften it.** The fix it points at is the one the
munmap comment already names: stop masking interrupts for the whole of a
syscall, so a target can answer.

## And a mitigation that already existed, which does not change the verdict

The boot gate matches the string `shootdown timed out` and fails the boot. That
is a detector, not a fix — the operation in that boot has already completed
incorrectly — and it only exists in CI. It is worth knowing because it means
this has not been happening silently in the runs measured here.
