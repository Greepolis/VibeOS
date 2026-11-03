# ext4

The native root's format, decided 2026-11-03; the plan and its five steps are
[filesystem-design/ext4.md](../filesystem-design/ext4.md). The driver is
`kernel/fs/ext2.c`, which reads ext2 and ext4 through one set of operations and
registers under both names: a volume with extents or 64-bit numbers is
mounted as `ext4`, any other as `ext2`.

## Step 1: read ext4 - done 2026-11-03

- **What it reads**: extent trees at any depth up to ext4's own five levels,
  uninitialised extents as zeros, 64-bit group descriptors and block numbers,
  flex_bg, whole 256-byte inodes, and htree directories read as linear ones -
  which they are designed to be.
- **Checksums** (`metadata_csum`, crc32c with ext4's seed): the superblock,
  every group descriptor, every inode (both halves), every extent index or
  leaf block and every directory leaf block are checked on read. A mismatch is
  refused, counted (`vibeos_ext2_csum_failures`) and reported to the
  must-be-zero registry; nothing it covers is used.
- **Features**: the incompat word is checked against what the driver reads; a
  volume with any other bit is refused and the bits are kept for the caller
  (`vibeos_ext2_last_refused_features`). A volume whose journal needs replay
  (`RECOVER`) is refused until step 3 replays it.
- **The oracle**: `scripts/dev/verify-ext4.sh`, run by `check.sh`. The host
  builds a tree (`scripts/dev/ext4-tree.py`), `mkfs.ext4 -d` puts it on a
  320 MiB volume with three groups, `e2fsck -D` indexes the large directory,
  `e2fsck -fn` must find it clean, and `vibeos_ext4_exercise` reads every file
  through the driver - in pieces that start and end inside blocks - and lists
  the tree with each file's crc32, held line by line against the host's
  listing of the tree itself. The script also checks the fixture did its job:
  the sparse file's extent tree has an index level, and the directory is htree.
  Then two damaged copies, made with debugfs: an inode whose checksum is zeroed
  must not be read, and a volume claiming `inline_data` must not mount and must
  name bit 0x8000.
- **At boot**: `EXT4.IMG`, made by `mkfs.ext4` with its defaults, is mounted
  at `/ext4` and its marker file read back; the gate asserts it beside ext2's.
- **Sabotage**: `scripts/dev/cases/ext4-read.txt`, seven cases, all red - an
  index node that always descends into its first child, a block read at its
  extent's start, an inode checksum not checked, an inode checksum over 128
  bytes only, an unknown feature mounted, group descriptors read at 32 bytes,
  and the wrong checksum seed.

## Open after step 1

| ID | What | Status |
|---|---|---|
| E4-001 | A symbolic link is read as a file: a fast link keeps its target in i_block, which read_at maps as block pointers. Pre-existing in the ext2 reader; the fixture has no links | open (step 2) |
| E4-002 | A directory leaf block's checksum is checked, but no damaged copy proves it: debugfs has no command that breaks one | open |
| E4-003 | Uninitialised extents read as zeros, and no fixture has one: mkfs.ext4 -d never writes them | open (step 2 writes them) |
| E4-004 | The high 16 bits of an extent's start are read, and no fixture is past 2^32 blocks to need them | open |
| E4-005 | htree is read linearly: a lookup in a directory of a million entries reads all of it | open (perf phase) |
| E4-006 | A volume with RECOVER set is refused, not replayed | open (step 3) |
