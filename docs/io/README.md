# Storage and I/O: plan

Status: Proposed
Author of record: written after the memory-manager rewrite closed, and grounded
in a survey of what is actually there rather than in what the file names
suggest.

Split across five documents, the same rule this plan asks of the code — each
covers one concern:

| Document | Covers |
| --- | --- |
| [architecture.md](architecture.md) | the layers, their contracts, and where the boundaries go |
| [observability.md](observability.md) | every counter and event, including for layers that do not exist yet |
| [maintainability.md](maintainability.md) | the structural rules, how they are enforced, and how each phase is verified |
| [phases.md](phases.md) | I0 to I7, specified to where implementing them is mechanical |
| [decisions.md](decisions.md) | the risks, and the questions reserved to the maintainer |

## 1. What is actually there

A survey, not an impression. These are the facts the plan is built on.

**About 2400 lines of filesystem code have never touched a real disk.**
`kernel/fs/` holds ext2, NTFS, ISO9660, exFAT, a journal, a partition table
reader, a storage layer and a block cache. Exactly one filesystem is mounted at
boot, and it is not any of them: it is FAT, which lives somewhere else
entirely, in `kernel/arch/x86_64/fat.c`.

This project already has the lesson written down — *"the driver nobody runs is
the one that ships broken"* — from the AHCI gap, where the kernel could only
talk to virtio-blk and every appliance imported, booted, and then failed every
exec because there was no way to read the disk. That was one driver. This is
seven components.

**There are three parallel read paths, and they share nothing.**

| path | used by | cache |
| --- | --- | --- |
| `blk_read` → `fat.c` statics | the boot filesystem | two static sector buffers |
| `blk_read` → `kernel/fs/blockcache.c` | ext2, NTFS, ISO9660, exFAT, journal, storage | a real block cache, unreachable at boot |
| `hw_read_file_cached` → the mm page cache | `execve` | the page cache from mm P4 |

FAT — the only filesystem that runs — is the one that bypasses the block cache.

**There is no I/O observability at all.** Not one counter. The memory manager
now carries roughly twenty, most of them asserted by the boot gate; the storage
path carries none, so "the disk is slow", "the disk is retrying" and "the disk
is fine" are the same silence.

**Writes happen and nothing checks them.**

This entry originally said no write is ever exercised. That was wrong, and the
counter added in I1 disproved it within minutes of existing: a boot performs
**thirty sector writes**. The shell's `mkdir DOCS` reaches `vibeos_fs_mkdir`,
which reaches FAT, which writes both copies of the table.

The accurate statement is narrower and still worth acting on: writes reach the
medium, and **nothing verifies that what was written is what comes back**. No
read-back, no comparison, no assertion, and nothing at all across a reboot -
which is the only check that distinguishes a write that reached the disk from
one that reached a cache. I4 is about that, not about making writes happen.

Worth keeping as a note on method: this was a claim made from reading the code
and grepping for callers, and it survived being written into a plan. What
caught it was a number. That is the argument for the observability this plan
asks for, demonstrated on the plan itself.

**An error has no reason.** `blk_read` returns `int`. Zero or not zero. A
timeout, a device that is absent, a sector the medium cannot return and a
request that was never issued are indistinguishable at every layer above.

**One number means two things**, and it is currently blocking the memory
manager. `hw_read_file_cached` returns a count that is both "how long the file
is" and "how many bytes I put in your buffer". They are equal only because the
buffer is large enough to hold every file, which is why `execve` still carries
6 MiB of staging buffers that nothing needs. mm's P4 step 3 stops there and
says so.

## 1b. Two different things are called a journal, and only one of them exists

`kernel/fs/journal.c` is a **write-ahead journal**: descriptor and commit
blocks, magics `VBJ1` and `VBJC`, the mechanism that makes a filesystem update
survive a power cut. That is journaling in the ext3 or NTFS sense.

It is **not** the operating system writing its own log to disk. That does not
exist at all. The kernel log is a ring in memory plus whatever reaches the
serial port, and none of it survives a reboot.

The distinction is written down here because it has already caused one
misunderstanding, and because the second thing is a real gap in a project whose
entire debugging method is reading logs. Every hard defect in this codebase was
found from a serial capture; a machine that wedges without one leaves nothing
behind. That is phase I5b, and it is not a nice-to-have here in the way it
might be elsewhere.

## 2. The goal

The same four the memory manager was held to.

**Production ready.** A machine that survives a disk that is slow, absent, or
returning errors — and says which. Today a failing read is indistinguishable
from a missing one, and a short read once reported itself as a complete file:
`execve` then parsed whatever the previous program had left in the shared
buffer. That defect is fixed; the *shape* that allowed it is still everywhere.

**Observable.** Every layer counts what it did and what it refused, in numbers
the boot gate asserts. The rule this project arrived at the hard way applies
unchanged: a number that is written and never read is not observability, and
three separate subsystems have shipped one.

**Maintainable.** One cache, not three. One request type, not a different
signature per driver. A boundary that a check can enforce, as
`check-exec-layering.sh` does for the interpreter path.

**Extensible.** Adding a driver should be filling in a table. Adding a
filesystem should be filling in `vibeos_fs_ops_t` and nothing else. Neither is
true today: a driver has to be bound through four function pointers with three
different shapes, and a filesystem has to choose which of the three read paths
it belongs to.

## 3. What this plan explicitly does not do

It does not add features to the filesystems. ext2 gaining symlinks or NTFS
gaining compression is not in scope and would be building on ground that has
not been walked on.

It does not make I/O asynchronous before it makes it correct. Queueing and
completion interrupts are I6, after the contract, the cache and the writes —
and the reason is recorded in `decisions.md`, because doing it earlier is the
tempting order and the wrong one.
