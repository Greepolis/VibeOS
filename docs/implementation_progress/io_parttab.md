# I4c: writing a partition table

**Status: the MBR writer is in, with four safety rules and a round trip on a
scratch device. GPT writing and `format` are not done.**

The plan's own section opens by saying this is the one phase that can lose a
user's data, and asks for confirmation before starting. That was asked and
given, and the shape of the answer is why there is a scratch device at all.

## The four rules

**A stale view may not overwrite a newer one.** Every write states the checksum
of the table it believes is on the disk and is refused if the disk holds
something else. Without it, two things that both read the table and one of
which writes silently discards the other's work — and here that work is where
somebody's filesystem begins.

**A partition in use is not touched.** Mounted, or named by the swap area. The
caller supplies what is in use rather than this layer guessing, because
guessing wrong is the whole failure mode. Two mistakes are checked separately
because they have the same consequence by different routes: a new table that
*overlaps* a live range, and one that has simply *dropped* it.

**Entries may not overlap, and none may start at sector 0.** Sector 0 is the
table; a partition starting there is overwritten by the write that creates it.

**Sector 0 is edited, not rebuilt.** It holds boot code as well as the table. A
writer that replaced the whole sector would make a disk unbootable while doing
exactly what it was asked.

GPT's backup-before-primary rule is stated in the header and not yet
implemented, because there is no GPT writer yet. It is written down where the
writer will go rather than in a plan file, so whoever writes it finds it.

## The scratch device, and the plan it replaced

A second QEMU disk was the intended answer and it needs something else first:
every piece of virtio-blk's state is a global — the queue, the descriptors, the
request struct, the lock — so a second device means making **the driver the
machine boots from** per-instance. That is a real refactor, and doing it inside
a phase about writing partition tables would mean two risky changes verified by
one result.

So the scratch device is RAM-backed and registered like any other driver: it
gets a device number, goes through `vibeos_blk_register`, and is bounds-checked
by the same code. The write path under test — the partition writer, the block
layer, the cache — is the real one. Only the medium differs, and the medium is
the part I4 already proves.

What that does not cover is a table surviving a power cut, which was never in
reach of a single boot anyway. It is the same gap as I4 step 3.

The boot reports:

```
[IO] PARTTAB scratch_device=0x1 round_trip=OK
```

It writes two partitions, reads the sector back through the ordinary
`vibeos_partition_parse_mbr` with no shared state, checks the entries, and
checks that the 446 bytes the table does not own are untouched.

## The sabotage that got through

Removing the guard overlap check **did not turn the tests red**. Two of the
three guard cases fell through to the other check — "no new entry covers this
range" — and were refused by that one instead, so the overlap check could be
deleted and nothing noticed. One check was masking the other.

The case that catches it grows a mounted partition in place: the new entry
starts exactly where the guard does and is larger, so the coverage rule is
satisfied and only the overlap rule can refuse it. Refusing is right — a
mounted filesystem has an opinion about where it ends, and changing that
underneath it is how a volume comes to read past its own data.

It was found by running the sabotage and watching green, which is the third
time this week that has been the only thing separating a working test from a
decorative one.

## What is left in I4c

**Step 3, `format`.** Putting a filesystem on a volume, through the
filesystem's own operation — the volume layer must not know what a FAT boot
sector looks like, or it becomes a second place that has to be right about FAT.
There is no `format` op yet.

**GPT writing**, with the backup written before the primary.

Until both exist, the phase's "done when" — partition, format, mount, write a
file, unmount, re-read — is not demonstrated, and the boot proves the first and
last steps of it.
