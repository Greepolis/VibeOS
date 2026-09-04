# P6 step 1 - the reverse map

The question every other structure in this subsystem cannot answer: given a
frame, which page-table entries point at it?

Nothing could ask it before. The kernel's only way to find out was to scan
every address space and trust the result, which is the "reconstruct the truth
from the hardware bits" mistake the whole memory-manager rewrite exists to end -
and the same mistake that, done twice in slightly different ways, leaked 211
frames a boot in one direction and handed the kernel's own page tables back to
the allocator in the other.

## Why it is the first item of P6 and not the last

The plan originally had it inside compaction, at the end. It moved because two
phases are blocked on it:

- **Compaction (P6.5)** moves a frame to recover contiguous space. Moving one
  means finding every entry that points at it and repointing them. A frame
  whose holders cannot be enumerated cannot be moved.
- **Swap (P5)** evicts a frame, which means unmapping it from everyone. After a
  fork a frame belongs to several address spaces, so an eviction knowing only
  one of them leaves the others pointing at a slot that no longer holds their
  page. P5's own correction note says it waits for this rather than restricting
  itself to singly-mapped frames - which would be a swap that cannot evict the
  pages a forking workload actually accumulates.

## What it is, and what it deliberately is not

It is not the ownership count. `owners` says *how many*, which is what lifetime
decisions need and is cheap to keep exact on the hot path of every map and
unmap. The reverse map says *which*, which is what moving and unmapping need
and is not on that path. Keeping them separate is the point.

One list head per frame - a direct index rather than a hash, one 32-bit word
per frame, a quarter of what the frame descriptor already costs. That removes
every question about collisions from a structure whose entire job is to be
trustworthy about identity.

**The layer never allocates.** It is called from inside the address-space
layer, which is itself called with other locks held, and a layer that can call
back into the allocator from there is a deadlock waiting for the first time
memory is tight. It is given a pool once.

**Running out is reported, not fatal.** `exhausted` counts it and `nodes_peak`
says what the pool should have been. Losing the ability to *move* a frame
degrades reclaim; failing the mapping that wanted to be recorded would break a
program that has done nothing wrong.

**It has its own lock**, supplied by registration rather than by a weak symbol -
the fourth layer here to need one, and for the reason CLAUDE.md gives: a layer
with mutable statics and more than one caller locks itself. Its own and not a
borrowed one, because sharing the frame lock would deadlock on the first map.

## The invariant it exists to make checkable

For a frame the address-space layer owns, the number of holders equals its
owner count. The defect this subsystem keeps producing is precisely a mapping
that no count knew about - `mappers=2, owners=1` - and until now that could
only be discovered at a release, one boot in sixteen, a long way from whatever
lost the reference.

## Verified

- Nine host-test groups, checking identity rather than quantity: a list that is
  the right *length* while naming the wrong mapping is worse than no list,
  because compaction would repoint an address space that never held the frame
  and leave the one that did pointing at freed memory.
- Four sabotage cases in `scripts/dev/cases/mm-rmap.txt`, each confirmed red,
  and the tree confirmed green again afterwards.

One of those tests was wrong first and is worth recording: the pool was sized
by guessing the node at 32 bytes when it is 24, so it held a third more than
intended and the exhaustion case passed without ever exhausting anything. It is
sized in bytes now, with an attempt count that is a multiple of the pool rather
than a number tuned to it. Same family as the vacuous NTFS cases.

## Not yet wired into the kernel

The layer is built, tested and sabotage-verified, and nothing calls it yet. The
next step is the callers - map, unmap, fork and teardown - and then the audit
that compares holder count against owner count, which is what turns the
invariant above into a boot assertion. That order is deliberate: wiring it in
without the audit would add cost and answer nothing.
