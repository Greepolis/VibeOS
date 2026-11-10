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
  (`RECOVER`) was refused; step 3 replays it.
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

## Step 5: the intensive test - done 2026-11-09

- **What it does** (`scripts/dev/ext4-torture.py`): for each seed, a random
  sequence of operations - create, write at an offset, writes with holes
  between them, truncate, mkdir, rmdir, unlink, rename and rename without
  replacing, link, symlink, chmod, the whole-file write, a directory filled
  past a block - is run by the host's own kernel on a directory, and whatever
  each call returned, success or an errno, becomes the line the driver must
  match (`fail ERRNO ...`). Names come from a small pool, so calls collide.
  The model is Linux; nothing in this project decides what an operation
  should have done.
- **Judged by**: `e2fsck -fn` on the volume the driver leaves; the superblock's
  free counts against the groups' sum and its checksum, from `dumpe2fs` (e2fsck
  mends both without calling a volume unclean); the tree read back through the
  driver against the host's, line for line. For the seeds with cuts, the
  device stops after a random write: the driver's replay and e2fsck's must each
  be clean, must agree, and the tree must be one the host's passed through.
- **Volume shapes**: the seed picks 1 or 4 KiB blocks, 16, 64 or 128 MiB (one
  group or several, groups mkfs left uninitialised), a journal or none (one
  seed in five: step 2's writer, which keeps the superblock itself), and in one
  seed in three a starting tree with a directory of 300 entries that `e2fsck
  -fD` indexes, so the driver changes an htree directory it did not make.
- **What a cut may leave that is no state**: ordered mode writes data in place
  ahead of the commit, so a write over bytes a file already had, cut before its
  commit, leaves the old tree with some of those bytes new. Linux leaves the
  same. It is accepted in exactly that shape - the state before that write, the
  old size, and every byte, read by `debugfs`, the old one or the new one - and
  counted as `torn_writes`.
- **Found while writing it**, none a driver defect: `mkdir` and `unlink` answer
  0 or -1 by the VFS contract (the errno is the Linux handler's), so for those
  the script asks only for a refusal (`refuse`); the exerciser's commit check
  counted refused lines, which commit nothing; and a file past the writer's
  four leaves of extents gets a short write and then EFBIG where Linux grows a
  level (E4-008, E4-010), so the generator keeps each file under half the cap.
- **Proved**: the 43 sabotage cases of `ext4-write.txt`,
  `ext4-journal-writer.txt` and `ext4-journal.txt`, verified by
  `verify-ext4-torture.sh` (40 seeds, 10 with 6 cuts): 36 red. Not red, and
  red under the checks written for them: extents not joined and an unwritten
  extent (no correctness a tree can show, and `fallocate` is not in the
  exerciser - `verify-ext4.sh`); the five journal cases whose window is a few
  writes wide (a free not told to the journal, a truncate tail zeroed in place,
  data over a freed block, an ignored revoke, the commit before the log) -
  random cuts land in them rarely, the every-write sweep of step 3 always.
- **Runs**: `check.sh` runs `verify-ext4-torture.sh` (60 seeds, 12 with six
  cuts, about a minute and a half); the nightly's `ext4-torture` job runs 1000
  seeds and 150 with ten cuts, sized from the measured cost (3000 and 400 ran into the job's timeout on the first night), on a range that moves with the run number, and
  uploads a failing seed's volume and script.

### Open after step 5

| ID | What | Status |
|---|---|---|
| E4-026 | Operations the torture does not make: chown, utimes, xattrs, fallocate, and writes past 2 MiB in one call; and none that Linux's VFS decides before the filesystem (rename into its own subtree, across kinds, onto another name of the same inode) - those are the handlers' and are tested there | open |
| E4-027 | A cut is judged only for the tree, not for modes, owners and times: the listing the two sides compare carries paths, types, sizes, link targets and contents | open |
| E4-028 | The root's files belonged to the uid that built the image (1000 in WSL) and were 0777 when the build tree was on a Windows disk: `mkfs.ext4 -d` copies both from the host | **closed 2026-11-10**: `make-boot-disk.py` gives every inode to root with `debugfs`, and `make-root-tree.py` sets the modes - directories 0755, `/tmp` and `/var/tmp` 1777, `/root` 0700, programs and libraries 0755, the rest 0644 |
| E4-029 | No wall clock: files the kernel writes are dated 1970, and `CLOCK_REALTIME` is the time since boot | open |

## Step 4: ext4 as the root - first half done 2026-11-08

- **The disk**: the gate's boot disk is a disk now, not a bare ESP:
  `scripts/make-boot-disk.py` puts an MBR in front of the ESP that
  `make_esp_image.py` builds (partition 1, type 0xEF) and an ext4 partition
  beside it (partition 2, type 0x83), made by the host's `mkfs.ext4 -L
  vibeos-root -d` from the hierarchy's tree. OVMF boots the 0xEF partition,
  the FAT driver takes the first partition as the boot volume, and the storage
  scan finds the second.
- **The mount**: the scan's volume whose label is `vibeos-root` is attached
  at `/sysroot`, writable and journalled (`hw_is_native_root`, by label, not
  "the first ext4": no wrong-volume fallback). The scan also finds the boot
  partition again; the mount table gets the boot mount for it, not a second
  FAT over the same sectors - the first boot with the partition lost every
  exec to that.
- **Proved**: the guest reads the marker the build put in the root and writes
  a file there; after the boot the gate cuts the partition out of the disk,
  replays and checks it with `e2fsck`, and reads the file back with `debugfs`
  (`root_volume_not_clean`, `root_volume_lost_the_guests_file`,
  `native_root_not_mounted`, `native_root_not_read`). **E4-015 closed.**
  Without `mkfs.ext4` the disk is the bare ESP and the verdict says
  `root_absent=`; `VIBEOS_SMOKE_ROOT=0` asks for that layout on purpose.
- **Sabotage**: `ext4-root-boot.txt` (the label not recognised: red,
  `native_root_not_mounted`) and `ext4-root-write-boot.txt` (a partial-block
  write dropped: red, `root_volume_lost_the_guests_file`; a first version that
  dropped whole-block writes went NOT RED, because the guest's file is
  thirteen bytes).
- **The root at `/`** (2026-11-08): the ext4 partition is attached at `/`
  and the ESP at `/boot/efi`. The kernel keeps two mounts where it had one:
  `g_rootfs`, the root - exec, the page cache, relative paths - and
  `g_bootfs`, the ESP, which the kernel itself reads (INIT.ELF, the swap
  file, the loop images, the write proof). The self-test's FAT exercises -
  the large file, the long name mtools reads back, the truncate, rename and
  rmdir the consistency check judges - write under `/boot/efi`, or they
  would have been testing ext4. For this part the root image carries a copy
  of the ESP's tree, so every `EFI/BOOT/...` path still resolves; the gate
  asserts `native_root_not_mounted` and `esp_not_at_boot_efi`, both with a
  sabotage case that goes red.
- **The programs in `/usr/bin`** (2026-11-08): the build puts every program
  of `EFI/BOOT` in the root's `/usr/bin`, and init's service table and
  `PATH`, the native shell, the self-test, the test programs that exec
  themselves or BusyBox, and the corpus scripts name `/usr/bin/...`. The root
  no longer carries `EFI/`: what stays on the ESP alone is what the firmware
  and the kernel read themselves - the loader, the kernel, INIT.ELF, the loop
  images, the swap file. A boot runs every program from `/usr/bin`, `/corpus`
  or `/win`. **Step 4 is done**, with the open items below.

### Open after step 4

| ID | What | Status |
|---|---|---|
| E4-021 | The programs keep their ESP names (`BUSYBOX.ELF`, `SVC_OK.ELF`): the gate recognises many of them by those names. Lower-case command names without the extension are a rename across the gate | open |
| E4-022 | The ESP still carries the programs, so that the bare-ESP layout (`root_absent`, `VIBEOS_SMOKE_ROOT=0`) boots; once nothing needs that layout, the ESP holds only what the firmware and the kernel read | **partly closed 2026-11-09**: the VM images' disk has an ESP of the loader, the kernel, INIT.ELF and the swap file only. The gate's disk keeps the full ESP for the bare-ESP layout and its filesystem test images (the kernel mounts those from the ESP); the `.iso` is the full ESP by necessity |
| E4-023 | `/lib`, `/lib64`, `/corpus`, `/win` and `/etc` are copied to the root as the ESP has them: `/lib` is a directory, not the alias of `/usr/lib` the hierarchy asks for, the test fixtures are in the production tree (F1), and the NT files are not yet in `/usr/lib/windows` | **closed 2026-11-10**: init reads `/etc/vibeos/services` when the root has one and runs its built-in self-test table only when it does not; the kernel types the self-test's script into the console only then too. `make-root-tree.py --production` - the VM images - leaves out the test programs (a list of what to keep, so a new test program is left out by default), `/corpus` and the test `.exe`s, and names the shell as the only service: a VirtualBox VM is at its prompt about ten seconds after power-on, against 166 s of self-test before. CI boots the `.vdi` and requires `NATIVE_INIT_MANIFEST file 1 0`. `/lib`, `/lib64`, `/bin`, `/sbin` and `/usr/sbin` are links into `/usr` since 2026-11-10 (the loaders and libraries live in `/usr/lib`; exec follows the link to a program's interpreter, `root-lib-alias-boot.txt` red). The Windows system files are in `/usr/lib/windows` (C:\Windows) and the hives in `/var/lib/windows` since 2026-11-10 (WO-106). Nothing of E4-023 is left |
| E4-024 | INIT.ELF is read by the kernel from the ESP, not from the root: init is the one program that does not come from `/usr/bin` | **closed 2026-11-09**: with a native root the kernel reads `/usr/bin/INIT.ELF` from it; the ESP's copy is read only when there is no root (the `.iso`). Gated (`init_not_from_root`, `ext4-root-init-boot.txt`, red) |
| E4-025 | The VM images (`scripts/make-vm-image*`) are still a bare ESP: only the gate's disk has the root partition | **closed 2026-11-09**: the root's tree is `scripts/make-root-tree.py`, used by the gate and the images alike, and the `.vdi`/`.vmdk`/`.ova` carry the whole disk - MBR, ESP, ext4 root. CI boots the `.vdi` and requires the root mounted at `/`; the same check on the bare ESP fails. The `.iso` stays the ESP alone: a CD holds no second partition |

## Step 3: the journal (jbd2, ordered mode) - done 2026-11-07

- **What it does** (`kernel/fs/ext4_journal.c`): every operation that changes
  the volume is one transaction. A metadata block it changes is copied into the
  next block of the log on its first write, and every later read and write of
  it - the writer's and the reader's helpers' - is answered there
  (`ext2_sec_read`, `ext4_j_meta_write`): the block cache is write-back, so a
  block written in place can reach the disk before the commit. A file's data is
  written in place first (ordered mode), except over a block this transaction
  freed, which the last commit may still call metadata: that is logged
  (`ext4_j_freed`). At the end, five flushes: RECOVER and the descriptors and
  logged copies; the commit block and the journal superblock pointing at it;
  the checkpoint; the journal emptied; RECOVER cleared. jbd2's format with v3
  checksums, which the driver turns on (and 64-bit tags) the first time it
  writes a journal mkfs made with none, as Linux does.
- **Replay** at mount, for a volume that says RECOVER, on a device that takes
  writes (one that does not is still refused): the scan, revoke and replay
  passes jbd2 has, the log wrapping, 64-bit and 32-bit tags with and without
  checksums, escaped blocks; a tag whose checksum does not match is skipped and
  counted (`bad_tags`, as jbd2 does), a commit whose checksum does not match
  ends the log. **E4-006 and E4-007 closed.**
- **The oracle**, by tools that are not ours:
  - `scripts/dev/verify-ext4-powercut.sh`: a 16 MiB volume `mkfs.ext4` made,
    a script of 29 single operations (create, append, mkdir, rename, link,
    symlink, chmod, unlink, truncate into a block, rmdir, whole-file replace of
    a file with an extent leaf, which frees a leaf and writes data in one
    transaction) run on a device that takes the first n sector writes and drops
    the rest silently. For every n: the driver replays what is left and
    `e2fsck -fn` must call it clean; `e2fsck -fy` replays the same image from
    the format alone and `e2fsck -fn` must call that clean; both trees, listed
    with every file's crc, must be the same, and must be the state before or
    after the operation the cut fell in - never a mixture, never a state
    undone. 4,600 writes; `check.sh` runs every 23rd, the nightly
    (`ext4-powercut`) every one.
  - `verify-ext4.sh` replays journals debugfs wrote (`jo`, `jw`, `jc`), with
    the tags it writes by default and with v3 checksums: a transaction of two
    blocks, one revoking the second, one never committed. The replay must write
    the first block, leave the revoked one and the uncommitted one, be clean,
    and agree with `e2fsck -fy` on the same image.
- **What the sweep found**, each before it was written down:
  - the logged copy of the superblock went home without RECOVER, so the
    checkpoint cleared the flag under a full journal and e2fsck refused it
    ("needs_recovery flag is clear, but journal has data"); setting the flag
    again after the checkpoint was not enough, because the cache evicts the
    checkpoint's sectors when it likes. The logged copy carries RECOVER now;
  - the superblock's two sectors reached the disk in slot order, not together,
    and a cut between them left a superblock with a checksum neither half
    matches. The cache flushes in order of sector now, and the superblock is
    written last before every flush;
  - a truncate zeroed the tail of the block it keeps in place, ahead of the
    commit, so a cut left the old size over zeros. It goes through the log.
- **The device model**: what a disk writes whole is its physical sector, 4 KiB
  on anything current, so a cut finishes the 4 KiB run the last write was in.
  With 512-byte atomicity the superblock could be torn by any writer - E4-017.
- **Not atomic, by design**: an overwrite in place. Ordered mode writes data
  where it lives, so a cut in the middle of one leaves some of each, as on
  Linux; the sweep's script appends and replaces and never overwrites, and
  that is said in the script.
- **Sabotage**: `scripts/dev/cases/ext4-journal.txt` (8) and
  `ext4-journal-writer.txt` (2), all red - data over a freed block in place,
  a read not answered from the log, a block logged without its contents, a
  revoke ignored, a tag checksum wrong, the logged superblock without RECOVER,
  the journal emptied before the checkpoint is durable, the commit before the
  log, a free not told to the journal, the truncate's tail in place. One more,
  the superblock put home among the other blocks rather than last, went NOT
  RED: the sorted flush already covers it at this size, and it is kept in the
  case file as a comment saying so.

## Open after step 3

| ID | What | Status |
|---|---|---|
| E4-016 | One operation is one transaction with five flushes, emptied at once: no group commit, no asynchronous checkpoint, no reuse of the log across transactions. Correct and slow; a write-heavy load pays a flush per call | open (perf phase) |
| E4-017 | The block layer writes one 512-byte sector at a time, so a structure across two sectors (the superblock, whose checksum is in the second) is atomic only if the device writes a 4 KiB run whole. The flush's order makes it one run; a multi-sector write in the blockdev interface would make it one command | open |
| E4-018 | A transaction logs at most 384 blocks (the volume state's table). An operation that needs more is refused with ENOSPC part-way, and what it had done is committed, as a failed operation was left before the journal (counted, `log_full`). Nothing in the script or the boot reaches it | open |
| E4-019 | No orphan list: a file whose last name is removed while it is open, and a truncate cut short, are completed by nothing after a crash. The truncate is one transaction, so it is never half done; the open-unlinked file is the gap | open |
| E4-020 | An external journal, a block-mapped one (ext3's), v2 checksums and fast commits are not written - the volume is mounted read-only - and a volume needing a replay of one is refused | open |

## Open after step 2

| ID | What | Status |
|---|---|---|
| E4-007 | No journal: a power cut between the steps of an operation (inode before entry, entry before count) leaves a volume e2fsck must repair; the order is chosen so that the damage is a leak or a count, not a dangling name | **closed in step 3** |
| E4-008 | A tree Linux grew to depth 2 (more than 1360 extents) is read, and a write that would change it is `EFBIG` (a write that reaches the cap is first short, then `EFBIG`; the torture of step 5 keeps its files under it); a block-mapped file on an ext4 volume is read, and writing or removing one is `EOPNOTSUPP` | open |
| E4-009 | An `ext_store` that finds no room for a leaf after the data blocks were written leaks those blocks (the tree is not updated); a write that fails half-way through an allocated run leaves the unwritten rest of that run reading as stale data. Both only on ENOSPC or an I/O error | open |
| E4-010 | The rule that a `BLOCK_UNINIT` group's bitmap holds other groups' tables that lie inside it (flex_bg) is coded and untested: this mke2fs puts every table in group 0, which is never `BLOCK_UNINIT`. Block sizes below 4 KiB and 128-byte inodes are refused or untested (1 KiB blocks are coded for, the extent cap there is 336) | open |
| E4-011 | Allocation is first-fit from the parent's group: directories are not spread (Orlov), no preallocation, no delayed allocation; no `fallocate`, hole punching, xattrs, ACLs, `mknod`, project quotas | open |
| E4-012 | A node's id is an inode number and the node carries no generation: one held across the last unlink may name another file later (`getattr` says ENOENT while the inode is free). The same gap as FAT's | open |
| E4-013 | `s_wtime`, `s_mnt_count`, `s_kbytes_written`, the state word and the backup superblocks and descriptors are not maintained (Linux keeps the backups stale too, until resize or e2fsck); atime is not updated by reads | open |
| E4-014 | Stack: a rename is a 1.2 KB frame above the reader's two 4 KiB ones, about 10 KB of the four-page kernel stack. It is the same nest a read of a path makes, plus the frame; making the reader's block buffers the volume's (under `.serialize`) would end it | **closed 2026-11-10**: the extent descent first (2026-11-09); then the directory scan - list and lookup each held a 4 KiB block, nested, and once `C:\Windows` mapped onto ext4 every DLL an NT program loaded listed a directory that way: a Clang Release kernel ran into its guard page (CI, run 42888574428, reproduced locally with the old reader). The scan reads each entry and name a piece at a time and checks the leaf checksum a sector at a time; a file read copies straight into the caller's buffer. Six Clang and six GCC Release boots clean |
| E4-015 | The boot gate does not write to an ext4 volume: the guest's `/ext4` is a read-only loop device. Step 4 mounts a writable one and gates the image the guest leaves with `e2fsck`, as `fat-fsck.py` is for FAT | **closed in step 4** |

## Open after step 1

| ID | What | Status |
|---|---|---|
| E4-001 | A symbolic link is read as a file: a fast link keeps its target in i_block, which read_at maps as block pointers. Pre-existing in the ext2 reader; the fixture has no links | **closed in step 2** |
| E4-002 | A directory leaf block's checksum is checked, but no damaged copy proves it: debugfs has no command that breaks one | open |
| E4-003 | Uninitialised extents read as zeros, and no fixture has one: mkfs.ext4 -d never writes them | **closed in step 2** |
| E4-004 | The high 16 bits of an extent's start are read, and no fixture is past 2^32 blocks to need them | open |
| E4-005 | htree is read linearly: a lookup in a directory of a million entries reads all of it | open (perf phase) |
| E4-006 | A volume with RECOVER set is refused, not replayed | **closed in step 3** |
