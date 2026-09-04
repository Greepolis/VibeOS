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

## What is not done

Steps 2 to 4: page-out, page-in on fault, and the stress operation. Those touch
the fault handler and the region descriptors, and they need reclaim to choose
candidates — which is where the anonymous tier that `skipped_no_swap` currently
counts will finally have somewhere to go.
