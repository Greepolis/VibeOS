# I2: one cache, not three

**Status: done.** Every sector the boot filesystem reads goes through
`kernel/fs/blockcache.c`. The hit ratio is 93% and the boot gate asserts it as
a ratio; twenty-four consecutive boots agree.

## What was found first, and it is bigger than the phase

**Nothing created a block cache on a booting machine.** `vibeos_storage_scan`
is defined and called by nobody; `blockcache.c`, `exfat.c`, `ext2.c`,
`iso9660.c`, `ntfs.c`, `partition.c` and `journal.c` are host-tested and had
never executed a line at boot. The whole of `kernel/fs/` was in the state the
swap stack was in this morning — built, tested, sabotage-verified, and never
run — except that it is a subsystem rather than a phase.

So this is not an optimisation. It is what makes any of that code real.

## The lock

Its own, by registration. The sixth layer here to need one and the reason has
not changed: a layer with mutable state and more than one possible caller locks
itself. Not the page cache's and not the frame layer's — those sit at different
levels, and a lock shared across levels is exactly what would have deadlocked
the page cache on its first miss, since a miss there allocates a frame.

One thing the lock had to move: the copy out of the slot. `blockcache_read`
copied after `fetch`, and a slot can be evicted and refilled the instant the
lock is released — which is the page cache's old defect wearing different
clothes, a lookup that hits and hands back somebody else's bytes. The copy is
inside now.

## Write-through, deliberately, and only for now

The cache can do write-back and this does not use it.
`vibeos_x86_64_blk_read_many` stays a direct bulk transfer, because reading a
two-megabyte image one sector at a time through a cache would undo the
multi-sector path — and that path is what turned a FAT read from something
indistinguishable from a hang into an ordinary read.

A bulk read that bypasses the cache is safe only while the device is
authoritative, which is precisely what write-back stops being true. So writes
go to the cache *and* to the medium. Write-back belongs with I4, where writes
are read back and proved; turning it on here would trade a verified property
for an unverified one.

## Two caches in series, and what that cost

The first measurement was 7%, on twenty-four consecutive boots, exactly.

`fat.c` still had its own one-sector FAT cache. It absorbed every repeated
table read and passed on only the misses, so the block cache was being asked to
serve the traffic that by definition could not be served. **Two caches in
series where the first is a hundredth the size of the second is not belt and
braces — it is the small one deciding what the large one is allowed to see.**

Removing it took the ratio from **7% to 93%**: 220,375 hits against 14,616
misses. That is the whole content of the phase's name, and routing the calls
without removing the old cache was doing half of it and getting the worse half
of both.

## The floor, and how it was got wrong first

Set to 20 from a single measurement of 38%. Twenty-four boots then measured 7%
every time — the 38% came from a stale image.

That is the rule this project writes down and I broke while quoting it:
*check a criterion against the baseline before using it to judge a change.* The
floor is 50 now, chosen after seeing twenty-four boots at 93%, comfortably
under a healthy boot and far above anything a cache that had stopped caching
could reach.

It stays a ratio and not a presence check, for the reason the plan gives: the
mm page cache shipped at 36% while "non-zero hits" passed happily.

## Verification

`repeat-boot.sh` at 24, which is what the plan demands for this phase, because
a cache's failure mode is *a hit that returns the wrong data* and that looks
like a working boot until a program is handed somebody else's bytes.

21 of 24 clean — the same 87.5% this machine measures without the change (6/6,
7/8, 6/6 before it). Of the three failures, two are the `bad-args` execve
refusal named yesterday and one is the known `busybox_cat` wedge. None is new.

## A diagnostic that erased its own evidence

The two `bad-args` failures printed `at=argv:-` — no reason. `hw_copy_user_argv`
cleared the reason on entry, and execve calls it twice: argv failed and set it,
envp then ran, cleared it, and succeeded. The refusal printed what was left.

It is set on failure and never cleared now. Worth recording because the fix I
shipped yesterday was specifically to stop this failure being silent, and it
was still silent about the one thing that mattered.
