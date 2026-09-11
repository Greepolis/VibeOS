# ext2, mounted from an image this project did not write (I5)

The ext2 driver has been in this tree for months. It had never parsed anything.

That is the whole finding, and it is the third time this session it has been
the finding: the swap stack, the block cache and the volume scan were each
correct code that nothing called, and each was declared working on the strength
of having been written. A driver nobody runs is indistinguishable from a driver
that works, right up to the moment somebody tries it.

## What now happens on every boot

`hw_ext2_bringup` attaches `EFI/BOOT/EXT2.IMG` through a loop device, gives it
a block cache, mounts it, reads a file out of it, compares the bytes, and
attaches it to the mount table at `/ext2`. A boot says:

```
[IO] EXT2 sectors=0x0000000000001000 result=OK
[IO] MOUNTED at=/     type=fat
[IO] MOUNTED at=/vol1 type=fat
[IO] MOUNTED at=/ext2 type=ext2
```

Two filesystem implementations, mounted at once, on one machine. That was the
point of I4b's mount table and of the FAT singleton refactor; this is the first
thing that could not have been done before either of them.

## The image is built by mke2fs, deliberately

`scripts/make-fs-image.py` shells out to the host's `mke2fs`. An image this
project wrote itself would only prove that the driver and the writer agree with
each other - and they would, because the same misreading of the layout would go
into both. The one artefact neither side of the test controls is the one worth
mounting.

It is asked for revision 0 with `-O ^resize_inode,^dir_index,^ext_attr,^has_journal`,
because a modern mke2fs turns on extents, 64bit and metadata_csum by default,
which is ext4 wearing an ext2 name. Turning them off is not making the test
easy; it is making it a test of ext2.

The file inside carries its own offset in every byte. A file full of a constant
survives a read that returned the wrong block, as long as that block had been
written too - which for a multi-block file is exactly the failure worth
catching.

## Why a loop device rather than a second disk

`kernel/arch/x86_64/loopdev.c` presents a contiguous file on the boot volume as
a read-only block device, reusing the extent resolution the swap area already
needed. The honest alternative - a second physical disk - requires virtio-blk
to stop being a singleton, which is a refactor of the driver the machine boots
from, and it is not what I5 is about.

The device refuses writes rather than dropping them. A device that accepts a
write it does not perform tells the caller its bytes are safe.

It also refuses a fragmented file. This is the sharpest edge in the change: a
loop device that spanned a gap would hand the filesystem above it another
file's bytes, and ext2 would parse them - a mount that succeeds and is wrong.
The staged image comes out as one run on this medium, so that refusal cannot be
sabotage-tested here; it is written up in the case file as a case this
environment cannot decide rather than a case that does not exist.

## What is gated

`ext2_exercise_missing` if the bring-up stops running, and the verdict itself if
the mount or the read fails. Confirmed by deleting the call: the gate goes to
`reason=invariant_failed:ext2_exercise_missing`.

A missing image is reported, not failed. A machine without e2fsprogs still
boots, and the build says why rather than dying somewhere a developer has to
guess about.

## What this does not prove

Reading. Nothing writes to an ext2 filesystem here, and the loop device could
not carry it if it tried. Directory traversal is one level deep. Neither is a
gap in the phase - I5 is "these filesystems have never been mounted from a real
image" - but neither should be read as more than it is.

The remaining filesystems (NTFS, ISO9660, exFAT) get the same treatment, one at
a time. One that cannot be given a real image in CI should be deleted rather
than kept: an unrunnable driver is the state this file exists to record.

## H-013: a block pointer past the volume (fixed 2026-09-11)

`ext2_read_block` turned any block number into `part_lba + block * sectors`, and
the mount did not keep `blocks_count`, so nothing could have bounded it. Every
block number the driver reads comes from the volume: direct and indirect
pointers in an inode, and - not in the report - the inode table's location in a
group descriptor, so a lookup alone could read past the volume. The block layer
bounds a read by the device, not by the partition, so a pointer past the volume
but inside the device returned another partition's sectors to user space.

The mount keeps `blocks_count`, and `ext2_read_block` refuses a block at or past
it. Every read goes through that function, so one check covers every path.

`test_ext2_block_pointer_outside_volume` declares a volume half the size of its
device and points a file's block into the other half, filled with a marker; the
read must return nothing. Red on the old code, green now. The half-size volume
is what makes the test honest: the host image's volume fills its device, and a
pointer past it would have been refused by the device and passed for the wrong
reason.

Host tests green, `check.sh all` green.
