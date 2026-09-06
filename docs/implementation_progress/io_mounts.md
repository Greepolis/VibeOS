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
