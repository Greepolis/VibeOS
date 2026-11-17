# I4b steps 3 and 4: a probe, and somewhere to put a second filesystem

**Status: done.** The scan mounts what it finds, the machine says what it has
mounted and where, and the mount table has five sabotage cases of which three
turn the host tests red today.

## Step 3: the probe, and the gap it closed

Steps 1 and 2 ended with a measurement: the scan found a 504 MB volume,
identified it correctly as FAT, and reported `fs=none`. **The only FAT driver
on the machine was invisible to the code doing the identifying** — it lives in
the arch layer, and `kernel/fs/` naming it would invert the layering the whole
storage refactor exists to establish.

So drivers that live outside `kernel/fs/` register instead:
`vibeos_storage_register`. The boot now says

```
[IO] VOLUMES table=mbr partitions=0x1 volumes=0x1 mounted=0x1 disk_sectors=0xfc000
[IO] MOUNT at=/ type=fat
[IO] VOLUME idx=0x0 first_lba=0x3f sectors=0xfbfc1 kind=fat fs=fat
```

`mounted=1`, `fs=fat`.

**Probe and mount are separate now**, and they were the same function: each
driver's probe *was* its mount, and a volume went to whoever mounted it first.
A driver that gets half way through a mount and fails leaves state nobody
unwinds, and every probe paid a full mount — which for ISO9660 is a real read a
long way into the volume. A probe reads, answers, and keeps nothing.

**The order is a decision.** NTFS and exFAT are probed before FAT because both
live in a boot sector that *is* a FAT boot sector with different fields. A FAT
probe that checks only the jump and the 0xAA55 signature says yes to an exFAT
volume — and a FAT driver that mounts one does not fail, it produces files full
of nonsense. So the FAT probe checks the two fields exFAT leaves at zero
precisely so this cannot happen: sectors-per-cluster and the FAT count.

**One limitation, stated rather than hidden.** `fat_scan_mount` takes a
`first_lba` and ignores it: the driver finds its own partition by parsing the
MBR itself. It therefore mounts the volume it would have chosen anyway — the
same one here, and the wrong one on a disk with two FAT partitions. Threading
the offset through `fat.c`'s globals is a change worth making on its own.

## Step 4: the mount table

There was one global mount, and **that was the structural reason only one
filesystem could run** — not a missing driver, a missing place to put a second
one.

A fixed array of eight, no allocation: these paths are reached from syscalls
and from the boot, and a table that allocates fails exactly when the machine is
short of memory, which is when programs are most likely to be opening files.

Three properties, each with a sabotage case that turns the tests red:

**Longest prefix wins.** `/usr/lib` beats `/usr` beats `/`. Taking the first
match instead makes resolution depend on the order things happened to mount in,
so the same path answers differently on two boots — the kind of defect that
appears months later on the first machine with an extra volume.

**A prefix must end on a boundary.** Without that, `/usrlocal` resolves to a
mount at `/usr`. The answer is wrong and reads perfectly reasonably in a log,
which is the worst combination this project knows.

**Two mounts may not claim one path.** Refused, not overwritten: overwriting
leaves the first filesystem mounted and unreachable, files open and blocks
dirty, with nothing able to name it to unmount it.

The tests also check what happens when a parent detaches — the table is flat
and the entries independent, so `/usr/lib` survives `/usr` going away, and what
was under `/usr` falls back to the root.

## What is deliberately not done

**The syscalls still take `g_rootfs` directly.** Moving them onto the resolver
is what makes a second mount *reachable*, and doing it in the same step as
building the table would mean neither is verified. What the table gives today
is the ability to say what is mounted, which is the phase's "done when" and
which a machine with one global mount could not do at all.

**"Mounts more than one" is not demonstrated.** This medium has one partition
and one driver that can claim it. Asserting two mounts would be asserting a
disk image this build does not produce — that is a change to the image, not to
the kernel, and it belongs with I4c where a partition table gets written.

## An unrelated finding, recorded so it is not lost

The intermittent `bad-args` execve refusal fired again during this work, and
gave the same reason as last time:

```
[EXEC] refused reason=bad-args path=EFI/BOOT/SVC_STRS.ELF at=argv:pml4_absent_or_not_user
```

Twice now, identically. Not a leaf permission problem: the **top-level** page
table entry for the argv vector's address is absent, in the child of a fork
reading its own `.bss` a moment after writing it through a copy-on-write fault.
That is a stable signature and the next thing to chase in that subsystem.


---

## The singleton is gone, and both "done when"s close

`fat.c` had one `fat_fs_t g_fat` with 84 references across 1200 lines, and that
was the second structural reason only one filesystem could run — the first was
the VFS mount table, one layer up. The boot now says:

```
[IO] MOUNTED at=/ type=fat
[IO] MOUNTED at=/vol1 type=fat
```

Two volumes, on two devices, at once. `/vol1` is the scratch device that I4c
partitions and formats, and the cycle the plan asks for — partition, format,
mount, write a file, unmount, re-read — runs on every boot. The read back goes
through the mount table's **resolver**, not through the mount handle, so what
is proved is that a path under `/vol1` reaches that volume and not the root.

## How it was done, and what it costs

A current-volume pointer, not a `fat_fs_t *` threaded through thirty static
functions. Both are correct; this one is a change that can be read in an
afternoon and verified by a boot, and the alternative is a wide edit to the
driver the machine boots from.

The pointer is safe for exactly one reason, and it is worth stating rather than
assuming: **every public entry point takes `fs_lock` for the whole operation**,
so there is never more than one in flight and never a moment when the current
volume is ambiguous.

What it costs is real: two volumes cannot be read at the same time. This driver
already serialised everything through that lock, so nothing got slower — but it
is the limit to lift if a second volume ever carries traffic, and lifting it
means threading the parameter after all.

## The defect this design produces, produced immediately

**An entry point that does not select operates on the wrong volume, silently.**

`vibeos_x86_64_fat_file_extent` took the lock and did not select. The moment a
second volume existed it read *that* one, and the swap area reported that this
medium has no swap file — a failure that points at swap and has nothing to do
with it.

Three entry points were missing a select. They are all fixed, and the audit is
one grep over the file: every function that calls `fs_lock` must call
`fat_select`. There is a sabotage case for it, because this is the failure mode
the design *has*, and a design with a known failure mode and no check for it is
worse than the parameter it avoided.

## Also on the way

The definition-order trap three times in one change — `g_fat_cur` used above
its declaration, `fat_select` used above its definition, and the driver table
naming a formatter defined after it. This file's own comments warn about it,
and it is still the thing that costs the most time in a 1200-line C file.

And a second set of `extern` declarations in `fat_vfs.c` had drifted the moment
the entry points grew a volume argument. They are gone: the header declares
them, and a second place to keep true is a second place to be wrong.
