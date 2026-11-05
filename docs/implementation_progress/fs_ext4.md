# ext4

The native root's format, decided 2026-11-03; the plan and its five steps are
[filesystem-design/ext4.md](../filesystem-design/ext4.md). The driver is
`kernel/fs/ext2.c`, which reads ext2 and ext4 through one set of operations and
registers under both names: a volume with extents or 64-bit numbers is
mounted as `ext4`, any other as `ext2`. Since step 2 `kernel/fs/ext4_write.c`
writes an `ext4` volume it can keep true (the table below the step), and
`kernel/fs/ext2_internal.h` is what the two files share.

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

## Step 2: write ext4, without the journal - done 2026-11-05

- **What is written**: a volume mounted as `ext4` with extents, filetype and
  `metadata_csum`, on a device that takes writes, and with no ro_compat or
  compat feature the writer does not keep (`ext4_w_mount`); 256-byte inodes,
  4 KiB blocks or less. Anything else, and the boot's `EXT4.IMG` (whose loop
  device has no write at all now - `hw_fsimage_write` is gone, the field is
  NULL, which is what blockdev.h calls a read-only device), gets the reading
  operations only. ext2 stays read-only.
- **The operations**: `write_at`, `truncate`, `create`, `mkdir`, `unlink`,
  `rmdir`, `rename` (with NOREPLACE, a directory over an empty one, ".." and
  link counts across parents, refusing to move a directory below itself),
  `link`, `symlink` and `readlink` (under 60 bytes in i_block, longer in a
  block), `setattr` and `setattr_node` (mode, owners with the high halves,
  atime, mtime), `getattr`, `statfs`, `sync` (a flush of the cache) and
  `write_file`. Times come from `vibeos_fs_now_ns`, with the nanosecond words.
  The table is `.serialize = 1`: the VFS holds the mount's lock, so the driver
  has none, and the reader's inode cache is dropped on every inode written.
- **What is kept exact** (all of it checked by `e2fsck -fn`, which does not
  check the superblock's totals, so the script sums the groups' own): the
  superblock's free block (64-bit) and inode counts and its checksum; each
  group's free blocks, free inodes, directories, unused inode tail, flags, both
  bitmap checksums (lo and hi) and its descriptor checksum; each inode's
  checksum (both halves) and `i_blocks` (data, tree blocks, attribute block);
  extent leaf checksums; directory block tails. A group `mkfs` left
  `BLOCK_UNINIT` or `INODE_UNINIT` is initialised on first use: the block
  bitmap is built from the superblock copy and descriptors the group carries
  (sparse_super), the reserved descriptor blocks, whatever of any group's
  bitmaps and inode table lies inside it, and the padding past the last block;
  the inode bitmap is zero with its padding set.
- **Extents**: a file's tree is read into a list in the volume's state
  (`fs->ext`, 1360 entries - four leaves of 340 - which is the most a depth-1
  tree under the inode's four index entries can hold, so the cap costs
  nothing the format could do at this depth), changed there, and written back:
  in the inode up to four extents, else as leaf blocks under index entries.
  New leaves are written before the inode names them and the old ones freed
  after. Adjacent runs merge (an allocation asks for the block after the
  file's last); a write into an unwritten extent splits it around the block
  and makes only that block written; a tree that would pass 1360 extents is
  `EFBIG`, and one Linux made deeper than depth 1 is read and not rewritten.
- **Directories**: a record goes into slack - an emptied record or the tail of
  a live one - or into a block added to the directory; a removal merges into
  the previous record (or empties the first of a block); every block keeps its
  12-byte checksum tail. The first change to an htree directory turns it into
  the linear directory every reader accepts: the root block becomes a leaf, each
  interior index block an empty leaf, and the index flag goes (`e2fsck -D`
  rebuilds an index when someone wants one). `dir_nlink`: a directory's count
  stops at 65000 and says 1.
- **Per-volume state**: `VIBEOS_FS_STATE_BYTES` 4608 -> 32768. `vibeos_ext2_t`
  is 29,992 bytes now (the superblock as on the volume, two 4 KiB buffers - the
  operation's and the allocator's - and the extent list); no allocator, no
  static buffer shared by two volumes, and two volumes are written at once.
  The cost is static: 14 states (8 in `vibeos_storage_t`, 6 in the loop-device
  image table) go from 64.5 KB to 458.8 KB, **+394,240 bytes of .bss**. The
  kernel stack is 4 pages and nothing here puts a block on it; the deepest
  nest is the reader's own (`ext2_dir_find` into `ext2_map_extent`, 4 KiB each,
  under a rename's 1.2 KB frame) - about 10 KB, and the reader's descriptor and
  inode reads now take a sector instead of a block (`ext2_read_part`) to give
  some of it back.
- **E4-001 closed**: `ext2_inode_is_fast_symlink` (Linux's rule - no blocks of
  its own, attribute block excepted, and no extents flag) makes `read_at` serve
  a short link from i_block. **E4-003 closed**: the fixture has an extent
  `debugfs fallocate` made, the blocks under it are junk, and it reads as zeros;
  the writer's split of one is held against a host file.
- **The oracle** (`scripts/dev/verify-ext4.sh`, run by `check.sh`; everything
  below is judged by tools that are not ours - `e2fsck`, `debugfs`, `dumpe2fs`,
  and the host's own file calls):
  - a 768 MiB volume of six groups, `mkfs.ext4 -d` from the host tree (which now
    has the unwritten extent, a file of 400 extents - two leaves under an index -
    and the 40-extent file), `e2fsck -D` for an htree directory; groups 1, 3 and
    4 are the `BLOCK_UNINIT` ones mkfs leaves (the script requires exactly
    those, and fills their on-disk bitmaps with ones so that reading one
    shows); the free space of the first two groups and the blocks under the
    unwritten extent are filled with 0xA5, because a volume that is zero
    wherever nobody wrote forgives every missing memset;
  - the exerciser's `apply` mode runs a script (`ext4-tree.py script`, 96
    steps) through the VFS wrappers: creates, appends, writes past the end and
    into holes, overwrites with unaligned ends, truncates down (into a block)
    and up, an unwritten extent written into, trees that change depth both ways
    (40, 400 and 600 extents), directories made four deep and removed, a
    directory renamed in place and to another parent, a file over a file, a
    directory over an empty one, refusals (EEXIST, ENOTEMPTY, EINVAL below
    itself, ENOTDIR, ENAMETOOLONG, EPERM for a directory link), hard links,
    symlinks of 12, 111 and 1999 bytes, a 255-byte name, the htree directory
    changed three ways, 300 files into one directory (more than a block holds,
    inodes spilling into the next group) and two of them unlinked and the hole
    reused, `chmod`/`chown` (owners past 16 bits)/`utimes`, `write_file` over
    itself, and 200 MiB written, overwritten in the middle, cut to 120 MiB and
    written again - which leaves the first group and takes blocks from the
    `BLOCK_UNINIT` ones;
  - then `e2fsck -fn` must be clean; the superblock's free counts must be the
    groups' sum; `/big.bin` must be at most six extents (merging is not a
    format rule, so e2fsck cannot ask for it); `debugfs rdump` reads the whole
    volume out and `ext4-tree.py compare` holds it against the host's copy,
    to which the same steps were made by `os.*`: names, types, sizes,
    contents, modes, link targets, explicit mtimes, and owners (`debugfs stat`,
    the host user cannot chown); and the driver's own reading of what it wrote
    (the reading operations on the volume as `walk`) must list the same tree;
  - a fresh 320 MiB volume whose second group is `INODE_UNINIT` gets 460 files
    and must end clean with that flag cleared; a 24 MiB volume is written until
    the driver says `ENOSPC` (exactly that, the file keeps what was written),
    must be clean, and after a truncate must take a new file.
- **Sabotage**: `scripts/dev/cases/ext4-write.txt`, 33 cases, all red -
  bitmap checksums (block, inode), group free blocks (allocation, free), free
  inodes, directory count, the unused-inode tail, the superblock's free blocks
  and inodes, superblock and descriptor checksums, the inode checksum's high
  half, the leaf checksum, extent merging, the split into leaves, `i_blocks`,
  the directory tail, the htree flag, removal merging, a new directory's
  parent link count and ".." and a moved directory's, a last-link unlink,
  NOREPLACE, a `BLOCK_UNINIT` group read from disk, built without the
  superblock copy, left flagged, an `INODE_UNINIT` group left flagged and its
  padding, a write into an unwritten extent, the zeroing of a new block's
  uncovered part and of a truncated block's tail, and a directory that does
  not grow. Two were NOT RED on the first run - both superblock totals, which
  e2fsck does not hold the superblock to - and the script now sums the groups'.
  One HUNG: a directory whose ".." names itself sent `rename`'s walk to the
  root round in a circle; the walk is bounded now (and EIO). `ext4-read.txt`
  gains the two reader cases for E4-001 and E4-003 and lost its stale anchor.
  One earlier reader case ("an index node always descends into its first
  child") had stopped being red when the fixture changed; the 400-extent file
  made it red again.
- **Gate**: `check.sh all` rc=0, warnings=0, clang-warnings=0, the static checks
  ok, host tests pass, `ext4=ok entries=621 written=936`, the kernel without the
  NT personality clean, boot `cli_and_network_verified` with `/ext4` mounted
  through the reading operations. Six boots, two at a time: 6/6 clean. Six
  boots three at a time (`boots.sh`'s default) measured 3/6 and the three
  failures were one signature - `guest_still_talking` at the 300 s budget in
  `busybox_cat`, no fault, no trap, no wedge report - on a host where a boot
  alone reaches `kernel_cli_ready` in 128 s (this change, single boot) against
  148 s for the parent's last recorded one, and where the passing boots of the
  same run reached it at 215-237 s: the host's, not a regression. The failing
  logs were kept, not cleared.
- **Harness note**: `sabotage.py` splits `SABOTAGE_VERIFY` without a shell, so
  the `a && b` form that `ext4-read.txt` documented scored every case red with
  no output. The lines say `bash -c '...'` now.

## Open after step 2

| ID | What | Status |
|---|---|---|
| E4-007 | No journal: a power cut between the steps of an operation (inode before entry, entry before count) leaves a volume e2fsck must repair; the order is chosen so that the damage is a leak or a count, not a dangling name | open (step 3) |
| E4-008 | A tree Linux grew to depth 2 (more than 1360 extents) is read, and a write that would change it is `EFBIG`; a block-mapped file on an ext4 volume is read, and writing or removing one is `EOPNOTSUPP` | open |
| E4-009 | An `ext_store` that finds no room for a leaf after the data blocks were written leaks those blocks (the tree is not updated); a write that fails half-way through an allocated run leaves the unwritten rest of that run reading as stale data. Both only on ENOSPC or an I/O error | open |
| E4-010 | The rule that a `BLOCK_UNINIT` group's bitmap holds other groups' tables that lie inside it (flex_bg) is coded and untested: this mke2fs puts every table in group 0, which is never `BLOCK_UNINIT`. Block sizes below 4 KiB and 128-byte inodes are refused or untested (1 KiB blocks are coded for, the extent cap there is 336) | open |
| E4-011 | Allocation is first-fit from the parent's group: directories are not spread (Orlov), no preallocation, no delayed allocation; no `fallocate`, hole punching, xattrs, ACLs, `mknod`, project quotas | open |
| E4-012 | A node's id is an inode number and the node carries no generation: one held across the last unlink may name another file later (`getattr` says ENOENT while the inode is free). The same gap as FAT's | open |
| E4-013 | `s_wtime`, `s_mnt_count`, `s_kbytes_written`, the state word and the backup superblocks and descriptors are not maintained (Linux keeps the backups stale too, until resize or e2fsck); atime is not updated by reads | open |
| E4-014 | Stack: a rename is a 1.2 KB frame above the reader's two 4 KiB ones, about 10 KB of the four-page kernel stack. It is the same nest a read of a path makes, plus the frame; making the reader's block buffers the volume's (under `.serialize`) would end it | open |
| E4-015 | The boot gate does not write to an ext4 volume: the guest's `/ext4` is a read-only loop device. Step 4 mounts a writable one and gates the image the guest leaves with `e2fsck`, as `fat-fsck.py` is for FAT | open (step 4) |

## Open after step 1

| ID | What | Status |
|---|---|---|
| E4-001 | A symbolic link is read as a file: a fast link keeps its target in i_block, which read_at maps as block pointers. Pre-existing in the ext2 reader; the fixture has no links | **closed in step 2** |
| E4-002 | A directory leaf block's checksum is checked, but no damaged copy proves it: debugfs has no command that breaks one | open |
| E4-003 | Uninitialised extents read as zeros, and no fixture has one: mkfs.ext4 -d never writes them | **closed in step 2** |
| E4-004 | The high 16 bits of an extent's start are read, and no fixture is past 2^32 blocks to need them | open |
| E4-005 | htree is read linearly: a lookup in a directory of a million entries reads all of it | open (perf phase) |
| E4-006 | A volume with RECOVER set is refused, not replayed | open (step 3) |
