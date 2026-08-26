# Storage: from one filesystem to several

## Where this starts

There is no filesystem abstraction. `kernel/arch/x86_64/arch_hw.c` calls
`vibeos_x86_64_fat_open`, `fat_read_at`, `fat_list`, `fat_write_file` and their
siblings directly, from twenty places across the syscall layer. In the kernel
as it stands, "filesystem" and "FAT" are the same word.

There is also no block cache: every read reaches the device. FAT survives that
because its structures are small and mostly sequential. Nothing more complex
would.

So the work is not "add filesystems". It is "introduce the layer that makes a
second filesystem possible", and then add filesystems as clients of it. The
order below is chosen so that each stage is verifiable on its own, and so that
the abstraction is designed against two real clients rather than one imagined
one.

## Stage 1 - a block layer with a cache

A device is a function pair: read this sector into this buffer, write this
buffer to this sector. Nothing above it should know it is virtio, and nothing
in it should know what the bytes mean.

On top of that, a cache of fixed-size blocks with write-back and an explicit
flush. Write-back rather than write-through because the point is to make
metadata updates affordable; explicit flush because durability has to be
something a filesystem asks for at a moment it chooses, not a side effect.

Portable and host-testable: a fake device backed by an array exercises
eviction, dirty write-back, flush ordering and read-after-write without
hardware. That matters - `kernel/fs/fat_chain.c` already showed that moving
logic out of the arch file is what let its bug be found by a test sweep rather
than by a boot.

## Stage 2 - the VFS interface

Two objects, kept deliberately small:

- a **mounted filesystem**: given a path, resolve it to a node; list a
  directory; create and remove
- a **node**: read at an offset, write at an offset, report size and kind

Everything the syscall layer needs today - `open`, `read`, `write`, `close`,
`getdents64`, `unlink`, `mkdir`, `fstat`, `newfstatat`, `execve` - is expressed
in those terms. The twenty direct calls in `arch_hw.c` become calls through a
mount table.

The interface gets designed here and proven in stage 3, but it is not
considered settled until stage 5, when something that is not FAT has to fit
into it.

## Stage 3 - FAT becomes a client

Port the existing driver behind the interface with no behaviour change. The
boot gate is the check: BusyBox reads files, lists directories and execs
programs, so if FAT still works through the new layer, the layer carries the
existing contract. Nothing new is added here on purpose - a refactor and a
feature in the same step means neither is verified.

## Stage 4 - partition tables

MBR and GPT. Today the volume is mounted directly, which works only because
QEMU hands us a bare FAT image. A real disk has a partition table, and every
filesystem after this one needs to be found before it can be mounted.

## Stage 5 - ext2, read-only

The first client that is not FAT, and the reason it comes before the easier
ones: it has a genuinely different shape - superblock, block groups, inodes,
indirect blocks - so it is what tells us whether the stage 2 interface was
designed or merely extracted from FAT. Expect to change the interface here.
That is the point.

## Stage 6 - the cheap ones

ISO9660 and exFAT, in that order. ISO9660 is small and immediately useful: it
is what an installation CD carries. exFAT is conceptually FAT with a 32-bit
table and an allocation bitmap, and its specification has been published since
2019.

## Stage 7 - NTFS, read-only

Feasible and worth doing: everything is a file, including `$MFT`, and the
records have a regular structure. The work is in sector fixups, resident versus
non-resident attributes, and run lists. Read-only keeps it bounded.

## Stage 8 - durability (roadmap M16)

Ordered metadata updates and a journal, with the exit criterion already written
in the roadmap: cutting power during a write must not corrupt the volume. This
is the same work as M16, not a separate track, and the block cache from stage 1
is where the ordering is enforced.

## What is deliberately not planned

**NTFS write.** Not because it is uninteresting, but because it is a different
size of problem: `$LogFile`, `$Bitmap`, an `$MFT` that grows, compression,
ACLs, and formats Microsoft never fully documented. Reliable NTFS writing on
Linux arrived after two decades and by way of a driver donated by a company
that did it professionally. If a concrete need appears, it gets planned then,
on evidence.

## How each stage is verified

Stages 1, 5, 6 and 7 are mostly parsing and bookkeeping, so they belong in
`kernel/fs/` as portable code with host tests - the same arrangement that let
`fat_chain.c` be swept exhaustively against a reference implementation.

Stages 2, 3 and 4 change what the kernel does at runtime, so the boot gate is
the check: it already asserts that a real program reads a real file from the
volume.

Stage 8 needs a test that does not exist yet: kill the machine mid-write and
mount the result. That test is the milestone, more than the journal is.

## When the plan completes

The documentation has to catch up, and in more than one place:

- `docs/implementation_progress/filesystem_layer.md` - the block cache, the
  mount interface, and each driver, with what a gate actually defends
- `docs/implementation_progress.md` - the status line for the filesystem area
- `docs/filesystem.md` - it describes a VFS as future architecture; there is
  one now
- `roadmap/milestones.md` - M16 is durable storage, and stages one to seven
  are the road to it
- `README.md` - the "What runs today" list says FAT

Written here rather than left to memory: this project has already shipped
documentation claiming that things were pending which had been done for weeks,
and the fix for that is to name the files while the work is fresh.

Agreed at the same time, and recorded for the same reason:

- fold `roadmap/phase2_technical_backlog.md` into the roadmap and delete it.
  Two documents planning the same work is worse than one, because someone will
  follow the wrong one; the roadmap has exit criteria and this does not.
- the same for `roadmap/development_phases.md`, whose Phase 1 closed months ago
- correct `docs/filesystem.md`, `memory_management.md`, `scheduler.md`,
  `architecture.md` and `software_architecture_specification.md`, which between
  them still describe finished work in the future tense. The specification
  matters most of the five: it is the longest document here and the one a
  newcomer reads first
- keep the two files dated `2026-04-03` exactly as they are. The date in the
  filename already marks them historical, and deleting them would lose the
  record of how the project got here
- write the two documents that do not exist at all: the graphics stack (mouse,
  framebuffer, compositor, on-screen terminal) and process semantics (signals,
  copy-on-write, pipes, descriptor inheritance). Both currently live only in
  code comments and commit messages
