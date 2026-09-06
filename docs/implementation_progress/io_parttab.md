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

## Step 3: format

An empty FAT16 volume — boot sector, two copies of the table, a root directory
of zeroes — written by the *filesystem's* own operation. The volume layer never
learns what a FAT boot sector looks like, which is the rule the plan states and
the reason this project has spent whole phases removing second places that have
to be right about the same thing.

**FAT12, FAT16 and FAT32 are the same header.** A reader tells them apart by
cluster count and by nothing else: under 4085 is FAT12, under 65525 is FAT16.
So a formatter that picks its geometry carelessly produces a filesystem of a
different kind from the one it intended, with every field still looking
correct. This one solves for sectors-per-FAT — the table's size depends on the
cluster count which depends on the table's size — and **refuses** a volume too
small to reach FAT16 rather than quietly producing FAT12. The scratch device
grew from 1 MiB to 4 MiB for that reason: sized by the format test, not the
partition one.

No boot code is written. The first three bytes are a jump to nothing, which is
what every tool writes for a data volume; doing otherwise would put a
bootloader on a partition nobody asked to boot from.

```
[IO] FORMAT fs=fat first_lba=0x40 result=OK
```

**Checked by probing, not by mounting**, and the reason is the limit recorded
in [io_mounts.md](io_mounts.md): this driver's state is a single global, so
mounting the scratch volume would unmount the one the machine is running from.
Probing proves the bytes on the medium are a FAT16 volume a reader will
recognise, which is what a format op is responsible for. The probe used is the
one the volume scan uses, with no shared state with the formatter — a formatter
checked by its own idea of what it wrote proves nothing.

## What is left in I4c

**Mount, write a file, unmount, re-read.** Blocked on the same thing twice
over: FAT's state is one global `g_fat`, with 77 references across 1200 lines.
Making it per-instance is the change that unblocks both this and I4b's "mounts
more than one", and it is a wide edit to the driver the machine boots from — so
it belongs on its own, not folded into a phase about partition tables. Doing
both here would be two risky changes verified by one result, which is exactly
what the scratch device was chosen to avoid.

**GPT writing**, with the backup written before the primary. The rule is stated
in `parttab.h` where the writer will go, rather than only in a plan file, so
whoever writes it finds it.

**A format that survives a reboot.** The scratch device is RAM, so its cache
and its medium are the same memory and an unflushed format is still there. Same
gap as I4 step 3, and it needs the same thing to close: a medium that survives
a boot.
