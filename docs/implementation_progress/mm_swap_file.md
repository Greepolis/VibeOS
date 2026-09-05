# Swap runs on the real machine

**Status: the swap path is live and gated. One gap remains and is named below.**

The memory manager's P5 — swap map, page-out, page-in, reclaim's anonymous tier
— had been built, host-tested and sabotage-verified for a phase, and had never
executed a single instruction on a booting machine. Every boot said so:

```
[MM] SWAP_AREA slots=0x0 (none configured on this media)
```

That is the state this project distrusts most: "the driver nobody runs is the
one that ships broken". It now says:

```
[MM] SWAP_AREA slots=0x800 first_lba=0x4bb8 sectors=0x4000 contiguous=0x1 (swap file accepted)
[MM] SWAP_ROUNDTRIP swap round trip OK
```

Two thousand and forty-eight slots, eight megabytes, on a file.

## A file first, a partition second

A contiguous file on the volume already mounted, because that is the shortest
path to making the stack *run*, and because a file is what a machine with no
spare partition has to fall back on anyway. The partition follows and is
described by the same structure — `vibeos_swap_area_t` has carried both kinds
since it was written.

`scripts/make-swapfile.py` stages eight megabytes of zeroes on the boot medium.
Written in one go rather than grown: the contents do not matter, the *extent*
does, and allocating the whole length at once is what gives a filesystem the
chance to lay it down as one run. It is only a chance, which is the next point.

## Contiguity is checked, not hoped for

`vibeos_x86_64_fat_file_extent` resolves a path, walks its chain, and reports
the first sector, the length in whole clusters, and whether the walk ever
jumped. `hw_swap_bringup` refuses a fragmented file outright.

This is the one place where being approximately right is not an error but
corruption. The swap file shares a volume with everything else, so an area that
spanned a gap would not fail — it would write a page of some process's memory
over another file's data, and the damage would surface at the next boot as a
program that is quietly wrong, with nothing to point at.

Two smaller decisions in the same spirit. The length comes from the clusters
the chain actually covers, not from the size in the directory entry — those
differ, and this project has already had one defect from trusting a declared
size. And a chain walk that hit a table error reports failure rather than a
short contiguous file, because `fat_next_cluster` returns the end-of-chain
marker for a failed read, which is the same value a healthy last cluster
returns.

## The round trip, and why half of it would have been useless

Everything below the area was host-tested against a memory-backed device. That
proves the arithmetic and proves nothing about this machine's disk, so the boot
now writes a page through the area, reads it back, and compares.

**That check alone is weak, and saying so is the point: a write and a read that
use the same wrong address agree perfectly.** An area whose first sector is off
by a cluster passes it while quietly writing over another file.

So the bytes are then looked for *through the filesystem* — `SWAPFILE.BIN` is
opened by name, which resolves its own chain and never consults the area's
stored first sector. If the pattern is not at the front of the file, the area
is not pointing at the swap file, whatever the round trip said.

Confirmed by breaking it. Shifting `area.first_sector` by eight sectors:

```
[MM] SWAP_ROUNDTRIP swap round trip FAILED: area is not the file
reason=invariant_failed:swap_roundtrip:swap_round_trip_FAILED:_area_is_not_the_file
```

The first half passed. Only the second half caught it.

## What was actually missing from the machine

Nothing in `kernel/mm/` needed changing. What was absent was every connection
to it:

* `vb.swap_write` and `vb.swap_read` were null, and `vibeos_vmspace_swap_out`
  checks for a null hook and gives up quietly — so the absence was not an error
  anywhere, it was a subsystem that could not be reached;
* the page-fault handler had no idea what a swapped entry was, so a page sent
  to swap could never have come back;
* `vibeos_reclaim_set_anon_source` was called by nobody, and no code anywhere
  decided *which* page should go.

The last one is `kernel/mm/anon.c`, new here: a resuming hand over the frame
table that takes allocated, unpinned frames with exactly one owner and one
mapping. The hand matters — scanning from zero every time would examine the same low
frames on every reclaim and reach the rest only under severe pressure, which is
the shape of a mechanism that looks like it works and covers a fraction of
memory.

The page-fault change is small and easy to get wrong: the swapped-entry test
runs **before** the copy-on-write test, because a swapped entry has no present
bit and the present-and-write condition would otherwise reject it — killing a
task for touching memory it legitimately owns.

## The gap, stated plainly

**`freed_anon` is zero on every boot, because nothing runs the machine short of
memory.** The anonymous tier is registered, its counters are printed, and the
boot gate asserts the ones that must be zero — but no boot has yet driven a
page out through `vibeos_anon_reclaim` and faulted it back in.

So what is proved today is: the area is real, it points at the file, and a page
survives the trip to the disk and back. What is *not* proved is the eviction
path under pressure. `svc-press` is the workload that would do it and it is not
started at boot, because at twenty megabytes it trips a separate defect that
predates all of this ([mm_reclaim.md](mm_reclaim.md)).

Naming that gap rather than letting `freed_anon=0` sit unremarked is the whole
of the difference between this and what came before it.
