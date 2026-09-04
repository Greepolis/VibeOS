# P5 step 1 — the swap map

Swap has two halves, and they fail differently. Deciding *which* page to evict
is a judgement that can be wrong without being dangerous — the machine is
slower. Deciding *where* it went cannot be wrong at all: a slot handed out
twice, or read while free, gives one process another process's memory at some
later fault, with nothing anywhere reporting an error at the moment it goes
wrong.

So the bookkeeping is a layer of its own, and it knows nothing about pages,
address spaces or pressure. It allocates a slot, gives it back, and moves 4 KiB
in either direction.

## What it guarantees

- a slot that is handed out is not handed out again until it is freed;
- a read returns what the last write to that slot put there;
- a failed write leaves the slot allocated and reports, rather than quietly
  losing a page something has already stopped mapping.

That last one is the least obvious and the most important. Freeing the slot
after a failed write puts it back in the pool while a page still believes its
contents are there; reporting success loses the page outright. **Losing a page
loudly is recoverable; losing it quietly is not.**

## Choices worth the words

**A bitmap and a cursor, not a free list.** A free list stores its links inside
the thing it tracks, and this tracks space on a disk, so the links would have
to live somewhere else regardless. A bitmap over a few thousand slots is one
cache line of scanning at worst, and "is this slot allocated?" is answerable
without walking anything — which is what makes the refusals cheap enough to
keep on every operation instead of only in debug builds.

The cursor matters on the path that runs when memory is short: always scanning
from zero would be O(allocated) per allocation with a full-from-the-front area.
Once round and no further, so "full" is answered rather than spun on.

**The device is a pair of callbacks.** The block layer is not portable and this
is — the same reason the frame layer takes a map function. Everything in this
subsystem that could not be host-tested is where the defects were.

**The transfer happens outside the lock.** It is a device operation and may be
slow, and every other layer here has learned that lesson. The slot is allocated
and stays allocated for the duration, so the bit does not need holding.

## Verified

Seven host-test groups, of which six are refusals. Five sabotage cases in
`scripts/dev/cases/mm-swap.txt`, each confirmed red with the tree confirmed
green again.

One test is arranged deliberately: "a slot is never handed out twice" marks the
slots it has seen rather than counting allocations, because a map that returned
the same slot every time would still report the right number. The sabotage case
that deletes `bit_set` is what that arrangement exists for.

Another asserts that a refused transfer never reached the device at all — a
refusal that still issued the read would leak the previous tenant's page
through the driver's buffers rather than through the caller's.

# P5 steps 2-3 — page-out and page-in

## The order is the whole design

Page-out **unmaps first, then writes**. The other order loses a store: a
process writing between the copy and the unmap would put its value into a frame
that is about to be freed, and the value is simply gone — silently, in a page
somebody reads back later.

Unmapping first turns that window from narrow into correct. A store in the
window *faults*; the fault finds an entry marked swapped, waits for the write
and reads back exactly what was written. Nothing is lost, only delayed.

That order is also why the write-failure recovery has to exist. When the write
fails the entry has already changed and there is nothing on disk, so leaving it
swapped loses the page for good. Putting the mapping back loses nothing — the
frame still holds the contents — and the next fault finds a normal page.

## Where the slot lives

In the entry, in the address field, with `VIBEOS_PTE_SWAPPED` (bit 10) saying
so. Only meaningful when the entry is not present, which is what makes bit 10
safe: the hardware never looks at a non-present entry, so the whole of it is
the kernel's.

Putting the slot number where the frame number would be is deliberate. The two
are never both true, and sharing the bits means a walk that forgets to check
the marker is looking at an address that cannot be valid rather than at a
plausible one.

## What it refuses, and why the refusals are counted

- **Pinned.** The same list as compaction and reclaim.
- **Shared.** After a fork a frame belongs to several address spaces, and
  evicting it means changing all of their entries — a different operation.
  Counted, because a swap that cannot touch shared pages will never reclaim
  what a forking workload accumulates, and that should be a number rather than
  a silence. It is the reason the plan says P5 waits for the reverse map.

## Verified

Five more host-test groups (fourteen in `compact_tests.c` now) and five more
sabotage cases, each confirmed red.

One of them walked through a green test first, and the reason is worth keeping.
"Page-in accepts a present entry" did not fail: with the guard removed, page-in
derives a slot number from the frame address, which is far outside the swap
area, so the transfer failed anyway — the test passed for the wrong reason. It
now asserts that no read was *attempted*, which is the same property the swap
map asserts for an unallocated slot. That is twice in this phase that a test
has been right about the outcome and wrong about the mechanism.

The round-trip test scribbles over the old frame before paging back in, so a
page-in that read from memory instead of from the slot would be caught rather
than flattered by the frame happening to still hold the right bytes.

## What is not done

Step 4: the stress operation, and the wiring that makes reclaim choose
anonymous pages and call this. `skipped_no_swap` still counts every anonymous
page reclaim was not allowed to take — that number is what should start moving
when the two are connected.
