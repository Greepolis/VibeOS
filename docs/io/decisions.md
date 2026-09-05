# Decisions, risks, and what is reserved to you

## The risks, in the order they would hurt

**D-R1 — I2 can stop the machine.** FAT is how this kernel boots, and moving it
onto the shared cache touches every sector read during boot. The failure mode
that matters is not a crash: it is a *hit that returns the wrong sector*, which
looks like a working boot until a program is handed somebody else's bytes. The
page cache did exactly that, and it took four attempts to find, three of which
produced confident wrong answers.

*Mitigation:* 24 boots, not one. An audit that compares a cached sector against
a fresh read, on by default during the phase. And the phase lands alone.

**D-R2 — I4c can destroy data that is not this machine's.** Writing a partition
table on a machine somebody uses is the only operation in this plan whose worst
case is somebody else's files. It is separated from reading tables for that
reason, and its case file should carry the warning at the top.

**D-R3 — I5 may delete more than it fixes.** Four filesystems have never run.
Some may be far from working, and the honest outcome for one of them may be
deletion. That is a decision with a cost — somebody wrote them — and it is
yours, not the plan's.

**D-R4 — the refactor touches the boot path throughout.** Unlike the memory
manager, which could be built beside the old code and switched on, storage is
load-bearing from the first sector. Each phase must leave the machine bootable.

## Decisions already taken, with the reason

**D1 — one entry point per driver, not three.** `read`, `read_many` and `write`
are one operation with three shapes, and the asymmetry (no `write_many`) is
evidence that nobody was looking, because nothing writes.

**D2 — the request is a value.** So a result can carry a reason and so I6 can
queue it later without changing every caller.

**D3 — GPT is tried before MBR.** A protective MBR parses as a valid table
describing one huge partition. Trying MBR first and stopping on success gives a
plausible wrong answer, which is worse than an error.

**D4 — the block cache is not merged with the mm page cache.** Settled as D6 of
the memory-manager plan and not reopened. B2 caches sectors of a device, the mm
cache holds pages of a file; after this refactor the second sits on top of the
first rather than beside it.

**D5 — asynchrony is last.** Queueing makes every existing defect harder to
see: a wrong sector returned synchronously is a bug you can print; the same bug
behind a completion queue is one you reproduce one boot in thirty. This is the
tempting order to invert and the wrong one.

**D5b — the journal becomes its own module, and later; its barrier does not.**
A journal is a transaction concern, orthogonal to files and to blocks, with two
customers in two different layers — ext2 and the partition-table writer. That
makes it a module rather than a filesystem feature, and it can wait.

What cannot wait is the ordering primitive it depends on. A write-back policy
designed without a barrier has to be rewritten when the journal arrives, so
`vibeos_blk_barrier` lands in I4 with the writes. A barrier, not a flush: a
flush empties the cache and would write everything out on every transaction.

**D6 — no new filesystem features.** ext2 gaining symlinks is building on
ground nobody has walked on.

## Reserved to you

These are not the plan's to decide.

**Q1 — how far does write support go?** Three defensible answers, and they are
different amounts of work:

- *read-only everywhere except FAT* — enough for a machine that boots and logs;
- *read-write on FAT and ext2* — enough to be a system somebody uses;
- *read-write everywhere* — a year of work on filesystems nobody has run yet.

The plan assumes the second and says so, but I4 is written so the first is a
stopping point rather than a failure.

**Q2 — what happens to the filesystems that have never run?** Mount them in CI,
or delete them. Two thousand lines that nothing runs is not a feature; it is a
liability with a plausible name. But deleting somebody's work is a call I will
not make on my own.

**Q3 — does a swap partition come from this layer now?** mm's swap area
descriptor is already the shape B3 produces. Wiring them means swap runs on the
real machine for the first time — which is worth a lot — and it means the
memory manager depends on a layer being refactored underneath it. Doing it in
I4b is cheap; doing it later is safer.

**Q4 — how many devices, and how many mounts?** Fixed arrays, since this kernel
does not allocate on these paths. Four devices and eight mounts is a guess.
Whatever the number, `*_peak` counters say afterwards what it should have been,
the way `rmap_peak` and `nodes_peak` already do.

**Q4b — how much disk does the log get, and does it survive a reformat?**
A reserved area outside any filesystem survives anything short of repartitioning
and is readable when the filesystem is broken. A file is easier to get at from
a host but is gone the moment the volume is. The plan assumes the reserved area
and says so; the size is a guess until `records_dropped` says otherwise.

**Q5 — is a partition editor a kernel feature at all?** I4c could be a
userspace tool talking to a narrow ioctl instead. That is smaller, safer, and
arguably where it belongs — and it is a design decision rather than an
implementation one.

## What I would do first, if you want a recommendation

**I0, I1 and I3, in that order, before anything else.**

I3 is the one that pays immediately: it unblocks the memory manager's last
open step and deletes 6 MiB of kernel memory, and it is a contract change
rather than a rewrite. I0 and I1 are what make it safe to do.

I2 is the largest structural win and the largest risk, and it should land alone,
on a quiet day, with 24 boots behind it.
