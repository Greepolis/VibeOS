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

## Wired, and audited

Four call sites, chosen by one rule: wherever a reference is taken by
publishing an entry, a holder is recorded; wherever a reference is given back
by clearing one, the holder is forgotten. The two lines sit next to each other
in every case, so an edit that moves one has to look at the other.

- `map_raw`, next to the `vibeos_frame_get` that publishes the entry - and the
  previous holder forgotten next to the `put` that retires the old one.
- `unmap`, next to its `put`.
- The copy-on-write fault's winning side, in **both** directions: the page's
  physical address changed, so this address space holds a different frame at
  the same address. Recording the new holder without forgetting the old one
  would leave the shared frame looking like it has one holder more than it
  does, and compaction would then move a page on behalf of an address space
  that had stopped pointing at it.
- `destroy`, as one sweep rather than one removal per entry - which would be
  quadratic in the size of the address space, and which also means a teardown
  that failed half way leaves nothing behind for the next tenant of a recycled
  root.

`clone_cow` needs no call of its own: it goes through `map_raw`.

### The audit

Every audited fork now compares, for each owned entry, the holder count against
the owner count. They are the same quantity counted two ways, so a disagreement
means somebody holds a page nothing is counting - `mappers=2, owners=1`, the
defect four investigations chased. Until now it could only be discovered at a
release, one boot in sixteen and a long way from whatever lost the reference.

It is skipped when the node pool has been exhausted at all, because a truncated
list is shorter than the truth by design and reporting that as a mismatch would
turn a reported degradation into a false defect.

`rmap_mismatch`, `rmap_cycles` and `rmap_missing_remove` are on the MUSTBEZERO
line and the gate fails on any of them. Verified in both directions: the clean
tree passes, and removing the single `vibeos_rmap_add` from `map_raw` gives
`mm_rmap_mismatch=58, mm_rmap_missing_remove=392`.

### What a boot measures

Zero on every counter, and `rmap_peak=1710` nodes against a pool of two per
frame - so the pool is oversized by a wide margin and `nodes_peak` is the
number to size it from if that ever matters. 8/8 clean boots.

## The wiring was wrong first, and the wedge report named it

The node pool was carved *after* `vibeos_frame_init`, and the frame table is
carved before it. What protects those pages from being handed out again is the
reserve of everything the physical allocator had already given away when the
frame layer started - so anything carved afterwards is memory the frame layer
believes is free.

It did exactly what that sounds like. User pages were allocated on top of the
node pool, the lists were overwritten with whatever the new tenant wrote, and
one of them closed into a cycle. The boot stopped with `CPU#0` inside
`vibeos_rmap_add` and every other core outside the kernel - a spin with nobody
to wait for, which is what walking a circular list looks like from outside.

Two things worth keeping from that. The wedge report named the function on the
first failing boot, which is the whole argument for it existing. And the fix is
one line moved, not a workaround: the pool is carved beside the frame table
where the reserve covers it.

A bound on the list walk was drafted as a safety net and then not kept -
bounding the symptom would have made the real defect survivable and quiet,
which is the opposite of what this subsystem needs. `rmap_cycles` exists for a
cycle that arrives some other way.
