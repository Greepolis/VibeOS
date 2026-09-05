# Filesystem Layer Progress

Status: In Progress (five filesystems behind one VFS; journalled writes verified
against power loss; the kernel's own root is still FAT)
Last review: 2026-08-26

## Shape of the layer

Four levels, each testable on the host without a kernel, which is where their
bugs are actually findable:

1. **Block device** (`include/vibeos/blockdev.h`) - a pair of functions plus a
   flush. Nothing above knows whether it is virtio; nothing inside knows what
   the bytes mean.
2. **Block cache** (`kernel/fs/blockcache.c`) - fixed slots, write-back. The
   only barrier that exists is a flush, and that flush now empties the
   *device's* cache too. Without it the ordering everything above believes in
   is fiction, because a drive acknowledges a write when it reaches its own
   volatile cache.
3. **Filesystem drivers** - one `vibeos_fs_ops_t` each.
4. **Mount** (`kernel/fs/vfs.c`) - `lookup`, `read_at`, `write_file`, `list`,
   `unlink`, `mkdir`, with no filesystem named above it.

Partition tables (MBR and GPT) sit between the device and the drivers in
`kernel/fs/partition.c`.

## Implemented

| Filesystem | Access | Notes |
| --- | --- | --- |
| FAT16/FAT32 | read/write | the boot volume; also reachable through the VFS adapter |
| ext2 | read | direct, indirect, double and triple indirect blocks; holes |
| ISO9660 | read | including Joliet names |
| exFAT | read | |
| NTFS | read | fixups, resident and non-resident attributes, run lists |

- Journal (`kernel/fs/journal.c`): write-ahead, one transaction in flight,
  recovered at attach. A transaction lands entirely or not at all.
- The syscall layer no longer names FAT: it goes through `vibeos_fs_*`.
- VFS runtime/service scaffold in `user/fs/vfs_service.c` and
  `user/fs/vfs_ops.c`, with the policy-aware secure open path.
- x86_64 `getdents64` now rejects non-directory descriptors and bounds each
  directory stream to 4096 entries, preventing malformed or cyclic metadata
  from wedging a user process indefinitely.
- The QEMU CLI harness records phase-specific BusyBox progress, and filesystem
  changes are covered by a dedicated Clang Release runtime workflow with
  serial/stderr artifacts. The Linux/WSL gate still must pass before this area
  can be marked runtime-complete.

## How it is verified

Host tests, plus a sabotage case per guard: each one removes a single check and
must turn a named test red. Cases live in `scripts/dev/cases/`, run with
`scripts/dev/sabotage.py`. Several tests in this project have passed while
proving nothing, so a guard without a red case is not considered covered.

The durability claim is tested as the claim itself, not as its mechanism: a
fake drive with a volatile cache of its own, told to stop accepting writes
after exactly N of them, swept across every N a transaction performs and across
several flush orderings. The volume must come back holding either the old
contents or the new ones. That sweep is what found two real defects - a
sequence number that cannot identify a commit record because the counter
restarts at every mount, and a magic word that is load-bearing because the
region is reused by transactions of different lengths.

## Pending
- The kernel mounts FAT as its root; the other four drivers are reachable
  through the VFS but nothing boots from them yet.
- Writing is FAT-only. The journal exists and is tested, but no filesystem
  driver routes its metadata updates through it yet - that connection is the
  next piece of work, and until it is made "crash-safe" describes the journal
  and not the volume.
- ext2/ISO9660/exFAT/NTFS are read-only by design for now.

## Next checkpoint
- Route FAT metadata updates through the journal, then re-run the power-cut
  sweep against a real FAT volume rather than a synthetic target set.

---

## What runs, and what only compiles

Written while planning the I/O refactor, from a survey rather than from the
file names — and it corrects a claim this file's own table row was making.

**Only FAT is mounted on a booting machine.** ext2, NTFS, ISO9660, exFAT, the
journal, the partition reader and the block cache are host-tested and have
never touched a real disk. That is about 2400 lines with no runtime evidence,
and this project already has the lesson written down from the AHCI gap: the
driver nobody runs is the one that ships broken.

**FAT bypasses the block cache.** It reads sectors directly through
`vibeos_x86_64_blk_read` into two static buffers of its own, so the only
filesystem that runs is the one that does not use the only cache there is.
There are three parallel read paths and they share nothing.

**Nothing writes.** `vibeos_fs_write_file`, `unlink` and `mkdir` exist and are
host-tested; no booting machine calls any of them. The journal's power-loss
recovery is verified the same way - a host test, not a machine that lost power.
Both claims are true and neither is about the running kernel, which is the
distinction the table row was losing.

**An error has no reason.** `blk_read` returns int. A timeout, an absent
device, a medium error and a request never issued are indistinguishable at
every layer above.

**There are no I/O counters at all.** Not few - none.

The plan that addresses all of this is [docs/io/](../io/README.md).
