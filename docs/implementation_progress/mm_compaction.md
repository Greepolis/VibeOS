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

Four refusals, and the order they are checked in is deliberate:

| refusal | why |
| --- | --- |
| pinned | a page table or a DMA buffer must never move; the safety one, checked first |
| writable | the copy-then-repoint window would lose a store |
| too many holders | a partly-moved frame is a state nothing here could recover from |
| raced | the holder list changed, so what was read is not what is there |

A frame nobody maps still moves. It has contents worth carrying — a cache page
holds a file's bytes whether or not anything has mapped it yet — and no holders
to repoint, which makes it the easiest and safest case rather than one to
refuse.

## The reference moves before the mapping does

The new frame gains an owner *before* the old one loses it, so the count never
dips through a value that would let a release take the old frame while an entry
still names it. Each entry is repointed with a compare-exchange against the
value that was checked; a holder that changed in between is left alone, because
it now points somewhere the mover does not own and forcing it would be the
mover overwriting somebody else's decision.

## Verified

Seven host-test groups, weighted so that two check it works and the rest check
it refuses — the refusals are the specification here, not the edge cases. Five
sabotage cases in `scripts/dev/cases/mm-compact.txt`, each confirmed red with
the tree confirmed green again.

**One of those cases walked straight through a passing test, and that is the
most useful thing to come out of this phase.** The "only the first holder is
checked" sabotage did not turn the tests red: the test mapped the writable
holder last, the reverse map inserts at the head, so the writable holder was
examined first anyway. The test asserted the right property about an
arrangement that happened to hide the defect. It now maps in both orders, and
the sabotage fails as it should.

## Not yet driven by anything

The move exists, is tested and is safe to call. Nothing calls it: choosing
*which* frames to move to open a run of a wanted size is a separate decision,
and wiring a policy before the mechanism is trusted would make both hard to
judge. The next step is a compactor that takes a target run length, picks
candidates from the fragmented region, and reports `largest_free_run` before
and after — which is the only honest test of whether any of this helps.
