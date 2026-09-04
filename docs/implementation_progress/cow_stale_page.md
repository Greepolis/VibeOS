# The copy-on-write page that keeps the parent's contents

The oldest open defect here, and the one the memory-manager rewrite was written
for. This file records what is now *known* about it, because four earlier
investigations each ended at a plausible pointer with no mechanism behind it,
and two of them were wrong in ways that cost days.

## The symptom

`svc-stress`, round 5 or 6, reports:

```
STRESS_FAIL: the child's own copy-on-write page at offset 0: found 0x33 expected 0xcc
STRESS_FAIL: verdict=the page was private and changed anyway
```

`0xcc` is `~0x33`, and the program computes `after = ~before`, so the child is
reading back exactly the value the page held *before the fork*.

## What has been established

A reproduction came first, and everything below rests on it rather than on
reading the code.

**It reproduces on demand.** `check.sh all build-clang-Release` fails three
times out of three. Local gcc builds boot 8/8 clean, which is why this went
unreproduced for so long - and it is the second time here that a defect lived
in the configuration nobody ran by hand.

**It is not a race.** Forced to a single core with `-smp 1`, it still fails
three times out of three. That removes every concurrency explanation, including
the TLB-shootdown family that had been the leading theory, and makes the defect
deterministic enough to reason about.

**The copy does happen.** The four page samples say so:

| Sample | frame | owners |
| --- | --- | --- |
| parent, before the fork | 3005 | 1 |
| child, after the fork | 3005 | 2 |
| child, after its write | 3020 | 1 |
| child, at the check | 3020 | 1 |

The frame changes on the child's first write, which is copy-on-write working,
and it does not change again.

**The frame is private and stays private.** `owners == 1` at the check, and the
frame is the same one the write went to.

**The whole page is stale, not one byte.** 4096 bytes of 4096 differ. This is
the measurement that mattered most and it was the last one taken: the check
reports the *first* byte that differs, which is always offset 0, and that was
read for an entire session as "only offset 0 is wrong". It is equally
consistent with every byte being wrong, and every byte is. Those are different
defects - a few stale bytes is a copy racing a store; a wholly stale page is a
write that went somewhere else entirely.

## What has been ruled out, each by a detector rather than by argument

- **A frame freed while still mapped.** The existing free-side watch was turned
  up from one free in sixty-four to *every* free. Silent for the whole boot.
- **A frame handed out while still owned.** A new counter in `frame_take`
  (`double_allocs`, printed in `MM_STATS` and asserted zero by the gate)
  watches the allocation side, which nothing watched before. Zero.
- **A write to a freed page.** `poison_hits` is zero.
- **Global TLB entries surviving a CR3 reload.** There is no global bit
  anywhere in this kernel and CR4.PGE is never set.
- **The two user windows having different mapping rules.**
  `hw_map_low_user_page` is a thin wrapper on `hw_map_page`; they are the same
  code.

## Where the next attempt should start

The child writes 4096 bytes and reads back 4096 bytes of the pre-fork value,
from a frame that the page tables and the ownership count both say is its own,
on a single core, deterministically. Either the stores did not land in frame
3020, or the reads did not come from it.

The next measurement should therefore not be about ownership or lifetime - both
are now instrumented and clean - but about *which frame the CPU actually used*:
have the child write one byte, then ask the kernel to read that page through
the identity map and report what it sees. If the kernel sees `after` and the
child sees `before`, the mapping is right and the child's translation is stale.
If the kernel also sees `before`, the store never reached the frame at all.

Note also that the write loop is compiled by clang at `-O2` and the defect is
clang-only so far; whether that is a vectorised store pattern or simply a
different allocation order has not been established, and should not be assumed
either way.

## The sharpest fact so far: mappers=2, owners=1

A `repeat-boot` run caught the release-side watch firing, and its message is
the most precise statement this defect has produced in five investigations:

```
[MM] FREE_WHILE_MAPPED frame=0x2355000 still mapped by pid=0xb
     during destroy mappers=0x2 owners=0x1 owners_now=0x1
```

Two address spaces map the frame; the ownership count knows about one. That is
not an arithmetic error in the count - it is a **mapping that was installed
without taking a reference**, which is exactly the failure the memory-manager
rewrite was proposed to make impossible.

It is reported during a `destroy`, and specifically a destroy of an address
space that is *not* the current task's - the arch-level short circuit for "the
dying task legitimately still maps what it is freeing" did not apply. So the
path is a task tearing down somebody else's address space: exit being reaped,
or execve discarding an old image.

`vibeos_vmspace_map_raw` takes its reference before publishing the entry, and
`clone_one` goes through it, so neither fork nor an ordinary map is the
uncounted one. The next attempt should look for a page-table entry written
without going through that function.

## Why these counters exist

The condition appears about one boot in sixteen. A message alone therefore
makes every run a lottery in which "nothing reported" and "fixed" are
indistinguishable - which is how three earlier fixes here were believed. Two
counters now make each boot a measurement:

- `double_allocs` - a frame handed out while somebody still owns it. Nothing
  watched the allocation side before, and it is the blind spot that matters:
  a double allocation frees nothing, so every release-side watch stays silent.
- `free_while_mapped` - more mappers than owners, counted rather than only
  printed.

Both are on the `MUSTBEZERO` line and the boot gate fails if either is
non-zero. The assertion was confirmed to go red by making `frame_take` count
unconditionally: `mm_double_allocs=4813`.

The free-side walk now samples one release in eight rather than one in
sixty-four. A counter that only sees one free in sixty-four cannot be compared
between two states, which is the whole point of having it. Eight clean boots
out of eight at the higher rate.
