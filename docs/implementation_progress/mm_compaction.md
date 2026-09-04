# P6 step 5 — compaction

Reclaiming free memory and making it *usable* are different problems.
`vibeos_frame_alloc_contig` is first fit, so after enough allocation and
release a machine can have plenty free and no run of it long enough for a DMA
descriptor or a staging buffer. Steps 1–3 recover free frames; only moving
frames that are still in use recovers contiguous space.

`largest_free_run` was already measured and printed in `meminfo`, so the
question "is there memory but not in one piece?" could be asked before this
existed. Now it can be answered.

## Why it could not have been done earlier

Moving a frame means finding every page-table entry that points at it. Until
the reverse map, the only way to ask was to scan every address space and trust
the result — the "reconstruct the truth from the hardware bits" mistake this
rewrite exists to end. That is why the plan moved the reverse map from the last
item of this phase to the first.

## The window, and the restriction that closes it

Between copying a frame and repointing what maps it there is an interval in
which a writer would store into the old copy and lose the write — silently, in
a page somebody else reads later. Closing it properly means revoking write
access first, taking the faults, and repointing: a stop-the-world, or a fault
handler that understands migration. Neither exists here.

So **a frame is refused unless no holder maps it writable.** That is not a
placeholder standing in for the real thing. It covers exactly the frames that
fragment this machine in practice — page-cache pages and program text, which
after a boot is most of memory — and it turns a race that has to be reasoned
about into a condition that can be checked. Every refusal is counted by reason,
so what compaction cannot move is a number rather than a silence.

Five refusals, and the order they are checked in is deliberate:

| refusal | why |
| --- | --- |
| pinned | a page table or a DMA buffer must never move; the safety one, checked first |
| untracked | a reference that is not a mapping cannot be repointed — see below |
| writable | the copy-then-repoint window would lose a store |
| too many holders | a partly-moved frame is a state nothing here could recover from |
| raced | the holder list changed, so what was read is not what is there |

## The reference moves before the mapping does

The new frame gains an owner *before* the old one loses it, so the count never
dips through a value that would let a release take the old frame while an entry
still names it. Each entry is repointed with a compare-exchange against the
value that was checked; a holder that changed in between is left alone, because
it now points somewhere the mover does not own and forcing it would be the
mover overwriting somebody else's decision.

## Verified

Nine host-test groups, weighted so that two check it works and the rest check
it refuses — the refusals are the specification here, not the edge cases. Six
sabotage cases in `scripts/dev/cases/mm-compact.txt`, each confirmed red with
the tree confirmed green again, and one more recorded there as unverifiable
(see the last section).

**One of those cases walked straight through a passing test, and that is the
most useful thing to come out of this phase.** The "only the first holder is
checked" sabotage did not turn the tests red: the test mapped the writable
holder last, the reverse map inserts at the head, so the writable holder was
examined first anyway. The test asserted the right property about an
arrangement that happened to hide the defect. It now maps in both orders, and
the sabotage fails as it should.

## The compactor, and the refusal the fragmentation test forced

`vibeos_reclaim_compact(want)` picks the window of `want` frames that needs the
fewest moves and empties it into free frames outside itself. It returns the
largest free run afterwards rather than a success flag: a caller compares that
against what it asked for and decides. "A machine with plenty free and nothing
in one piece" is precisely the state a boolean would hide.

Cheapest window rather than the first that could work, because the first
suitable window is usually occupied by long-lived pages — the most work and the
most likely to be refused.

### The refusal that was missing

The fragmentation test failed on its first run, and for the right reason: the
frames it moved never became free. An allocated frame that nothing has mapped
still has an owner — whoever allocated it and kept the physical address. The
reverse map cannot repoint that holder because it does not know it exists, so
moving the contents leaves it reading the old frame while everything else reads
the new one.

So a frame is now refused unless **every reference is a mapping**:
`owners == rmap_count`. The harmless face of getting this wrong is a compaction
that does not free anything. The harmful face is a page-cache page moved out
from under the entry holding its address.

An earlier test asserted the opposite — that an unmapped frame *should* move,
because it looked like the easiest case. It was wrong, and the test that
disproved it was the one the plan calls the only honest one.

### The tests were arranged differently from the kernel

Once the refusal existed, every move was refused: the tests held the allocation
reference after mapping, so each frame had an owner that was not a mapping —
namely the test itself. The kernel does not do that (decision D9: a caller that
allocates in order to map hands the frame over and lets go), so the tests were
wrong and the layer was right. They go through a helper now that follows the
contract. Arranging a test differently from the code it exercises is how a test
comes to disagree with reality.

## The only honest test

Fragment memory to a comb — allocate until the allocator is empty, unmap every
other page — and then ask for a contiguous run. The occupied frames are mapped
read-only rather than merely allocated, and that is the point rather than a
detail: what fragments a real machine and what compaction can actually move are
both the mapped kind. Fragmenting with unmovable frames would be a test of the
refusal, which is elsewhere.

A second case pins everything, so no window can ever be emptied, and asserts
that the compactor comes back with a measurement that says so rather than a
success. A caller told "done" that then failed to allocate would have no way to
find out why.

## One guard that cannot be shown to matter here

The compactor never takes a target frame from inside the window it is clearing.
The reasoning is sound — a target inside the window is undone by the next move,
and moves chasing each other around one window is a compactor that appears to
work and never finishes — but removing the guard does **not** turn the tests
red: the target scan starts at frame zero and finds a free frame outside the
window first in every arrangement these tests can build.

It is recorded in the case file as unverifiable rather than listed among the
cases that pass, for the same reason the AHCI cases are: "no case exists" and
"the case exists and this environment cannot tell" are different things.
