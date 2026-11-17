# ISO9660, and a test that agreed with the bug (I5)

ext2 mounted on the first attempt. ISO9660 did not, and what it found is the
clearest justification this phase has produced for its own premise.

## The defect

`vibeos_iso9660_mount` read the logical block size at offset 128 of the primary
volume descriptor with a 32-bit little-endian load. That field is a both-endian
pair of **16-bit** numbers: four bytes, but two copies of a 16-bit value, not
one 32-bit value. For the 2048 every ISO in existence uses, the four bytes are
`00 08 08 00`, and a 32-bit little-endian read of them is 0x00080800.

So the check `!= VIBEOS_ISO_SECTOR` was true for every well-formed image, and
the driver refused every ISO ever made. It has been in the tree for months.

The comment two lines above the defect warns about the both-endian trap - for
the fields that really are 32-bit - and the code below it walked into the
16-bit version of the same trap.

## Why the host test could not see it

There was a test. `test_iso_lookup_and_read` mounted an image, walked a
directory, read a file and checked the bytes. It passed.

It passed because `iso_build()` wrote the block-size field with `iso_w32both` -
the same misreading, on the writing side. The fixture and the driver agreed
with each other, and agreeing with each other is all a self-built fixture can
ever demonstrate.

This is the fourth time in this project that a test has been right about the
outcome and wrong about the mechanism, and it is the most direct: the property
under test was correct, the arrangement made the defect unobservable, and only
an artefact from outside the project could tell the two apart.

The fixture now writes a 16-bit both-endian pair, per the standard. Confirmed
by putting `rd32le` back: three host tests go red, `test_iso_refusals` among
them. The corrected fixture was verified against the old driver *before* the
driver was changed.

## What runs now

The bring-up became a table. The first version was one function per filesystem,
and the second would have been a copy of it with four names changed - which is
how a family of checks ends up with one member asserted and the rest not.

```
[IO] FSIMAGE name=ext2    sectors=0x0000000000001000 result=OK
[IO] FSIMAGE name=iso9660 sectors=0x00000000000002f0 result=OK
[IO] MOUNTED at=/     type=fat
[IO] MOUNTED at=/vol1 type=fat
[IO] MOUNTED at=/ext2 type=ext2
[IO] MOUNTED at=/iso  type=iso9660
```

Four mounts, three filesystem implementations, one machine.

The image comes from the host's `xorriso`, with no Rock Ridge and no Joliet.
Those are extensions; the driver under test reads plain ISO9660, and an image
whose real names lived in an extension it does not implement would test the
fallback rather than the filesystem.

## What is gated

`fsimage_missing:<name>` per expected filesystem, and the verdict itself when a
mount or read fails. The expected names are listed rather than counted: a count
cannot tell "iso9660 stopped being exercised" from "ext2 ran twice".

## What is left

NTFS and exFAT, the same way. Neither `mkntfs` nor `mkfs.exfat` is installed
here, so both need a decision about tooling before they can be given a real
image - and per this phase's own rule, a filesystem that cannot be handed a
real image in CI should be deleted rather than kept. An unrunnable driver is
exactly the state this file exists to record.
