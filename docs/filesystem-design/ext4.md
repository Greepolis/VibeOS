# ext4 as the Native Root

**Decided 2026-11-03:** the writable root - `/`, `/var`, `/home` - is ext4.
It is the format F3 of [implementation-phases.md](implementation-phases.md)
asked to be chosen.

Why ext4 rather than a format of our own or ext2 alone: it is what Linux
programs and administrators expect; every host can make, check and repair one
(`mkfs.ext4`, `e2fsck`, `debugfs`), which is the oracle this project's rule asks
for any external format - a test volume made by somebody else's tool; and its
journal answers the durability question the hierarchy's design leaves open.
The driver grows out of `kernel/fs/ext2.c`, which reads ext2 today: portable,
every byte through the block cache, host-tested on a fabricated image.

## What is supported

The feature set `mkfs.ext4` (e2fsprogs 1.47.0, the build host's) sets by
default, and nothing else - as `dumpe2fs -h` lists it: `has_journal`,
`ext_attr`, `resize_inode`, `dir_index` (read; a write keeps the index valid or
clears it), `filetype`, `extent`, `64bit`, `flex_bg`, `sparse_super`,
`large_file`, `huge_file`, `dir_nlink`, `extra_isize`, `metadata_csum` (crc32c,
verified on read and written on write); 4 KiB blocks, 256-byte inodes.
Extended attributes are kept intact, not interpreted, until security needs
them. A volume with an incompat feature outside that list is refused
by name; one with an unknown ro_compat feature is mounted read-only, as Linux
does. Not planned: inline data, encryption, casefold, quotas, bigalloc.

## Steps

Each step is closed as the NT ones are: host tests, sabotage cases, `check.sh
all`, six boots, and the open items written down.

1. **Read ext4.** Extent trees at any depth, 64-bit group descriptors,
   flex_bg, checksums verified (a bad one is `EUCLEAN` and counted, never
   silently used), htree directories read as linear ones. The test volumes
   are made by `mkfs.ext4` and filled by `debugfs` on the host, never by our
   own writer. ext2 stays readable through the same driver.
2. **Write, without the journal.** Block and inode allocation with their
   bitmaps, group descriptors and checksums; create, unlink, rename, mkdir,
   rmdir, link, symlink, truncate; extents that split and grow a tree;
   owners, modes and times; the free counts kept true. The oracle is
   `e2fsck -fn`, run after every host test on the volume it left, and by the
   boot gate on the image the guest leaves behind, as `fat-fsck.py` is for FAT.
3. **The journal (jbd2).** Ordered mode: data before the metadata that names
   it, metadata through the journal, a commit block with its checksum, and
   replay at mount. Proved by a power cut at every write: a host test that
   stops the device after write *n*, for every *n*, mounts what is left and
   asks `e2fsck` - the real driver against the real format, not a model.
4. **ext4 as the root.** The hierarchy of [hierarchy.md](hierarchy.md) on an
   ext4 volume the build makes, the ESP mounted at `/boot/efi`, the boot
   gate run from it; FAT stays the ESP and the bring-up root until this step
   is green.
5. **The intensive test** the nightly asks of every module: random
   operations against a model of the tree, thousands of seeds, the volume
   checked by `e2fsck` after each, and the power-cut sweep at random points.

## What this does not decide

The installer, A/B slots and rollback (F4) and whether `/var` and `/home` are
separate volumes: one ext4 volume holds all three until F4 says otherwise.

## Progress

Step 1, reading ext4, done 2026-11-03; what it does, how it is proved and what
is left open (E4-001 onward) are in
[implementation_progress/fs_ext4.md](../implementation_progress/fs_ext4.md).
