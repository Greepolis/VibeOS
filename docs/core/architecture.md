# The shape of the core

The first version of this plan described where code should live. That is a
tidying plan, not an architecture, and it would have produced a smaller
`arch_hw.c` that was extended the same way — by editing it.

This file is about the shape. It is written from the project's own evidence
rather than from a preference, because that evidence is unusually clear.

## What has actually worked here, and what has not

Two ways of adding something to this kernel have been tried, repeatedly.

**Registration worked.** `vibeos_storage_register` let ext2, ISO9660, NTFS and
exFAT arrive without the storage layer being edited for any of them.
`vibeos_blk_register` did the same for virtio-blk, AHCI, the RAM scratch device
and four loop devices. There are eight of these seams today —
`vibeos_frame_set_lock`, `vibeos_rmap_set_lock`, `vibeos_cache_set_lock`,
`vibeos_blockcache_set_lock`, `vibeos_swapmap_set_lock`,
`vibeos_task_view_set_source`, and the two registries — and each was invented
separately, under pressure, by somebody solving one problem.

**Editing the monolith did not.** Every feature added by editing `arch_hw.c`
made `arch_hw.c` bigger, and it has now happened twice in a way somebody
noticed and wrote down: a session dedicated to shrinking it grew it by 633
lines, and the I5/I5b work grew it by 2,800 while the extraction document sat
in the repository saying the length of that file was the completion criterion.

That is the finding. **A subsystem is extended by registering into it, never by
editing it.** The project discovered this five times and never named it, so it
was re-derived each time and applied only where the pain was sharpest.

## What a subsystem is

Every subsystem here that works has the same seven parts. Nobody wrote this
down, so each new one has been assembled from memory and some have arrived
missing a piece — which is how a page cache with no lock ran for months on the
accident of having exactly one caller.

1. **One header** declaring the types and the entry points.
2. **One `.c` owning all of its mutable state.** No other file names its
   globals. This is what the mm-layering check already enforces for page
   tables, generalised.
3. **Its own lock, registered rather than assumed.** Its own, because the page
   cache allocates frames and handing it the frame layer's lock would deadlock
   on the first miss. Registered, because a weak symbol does not resolve across
   objects under PE/COFF and the Windows job proved it.
4. **A statistics struct with at least one must-be-zero counter**, so that
   "this is broken" is a number and not a log line somebody has to read.
5. **An init that can fail and says which reason.** A refusal that names a
   mechanism instead of a situation costs a session; this project has the
   receipts.
6. **A registration point**, if anything can be plugged into it.
7. **A sabotage case file.** A subsystem with no case file is not finished,
   because three checks here have shipped unable to fail.

A checker enforces the mechanical parts of this — see `check-subsystem.py`.
The parts a script cannot judge stay in review, and the file says which is
which rather than pretending the script covers everything.

## The layering, stated as a question rather than a word

"Portable" is the wrong test and produced the current situation: a portable
kernel that is portable and does not run. The test is:

> Would a second architecture need a **different answer**, or the **same answer
> implemented differently**?

Same answer, different implementation → the arch layer provides a mechanism and
the core decides. Picking the next task is the same answer everywhere; writing
`cr3` is not. Deciding a signal is permitted is the same answer everywhere;
building the frame it is delivered on is not.

Different answer → it belongs in the arch layer entirely. There is no core
opinion about what an IDT is.

This is deliberately not "no `#ifdef` in core". There is one architecture and
building for a second one that does not exist is how the present portable
kernel became untested and unreachable.

## The extension points the next year needs

The roadmap has three refactors after this one: drivers and peripherals, the
network, and a syscall layer that carries Linux, Windows and macOS ABIs. An
architecture that does not make those cheap is not worth the disruption of
adopting it.

### Drivers

`vibeos_blk_register` is the model and it already works. What is missing is
that it is the *only* one: there is no equivalent for a character device, an
input device, a network interface or a display. Each of those is wired
directly into `arch_hw.c` today, which is why adding USB or NVMe means editing
the monolith.

The core provides the registry and the contract; a driver provides a struct.
Nothing in the core is edited to add one. That is already true for storage and
must become true for the rest.

### Syscall ABIs

This is the one the current plan handled worst, and it is the next refactor
after this, so getting it wrong here blocks it.

An ABI is **not** a branch in the dispatcher. Three ABIs as three branches is
three copies of every permission check, and the checks are the whole reason
this file has been where the security defects were found.

An ABI is a **registered translator**:

```
vibeos_abi_t {
    name;                                  /* "linux", "win32", "macho"   */
    classify(frame) -> vibeos_syscall_id;  /* their number to ours        */
    marshal(id, frame) -> vibeos_args_t;   /* their calling convention    */
    result(id, value, frame);              /* our answer in their shape   */
}
```

The kernel keeps **one** vocabulary of syscalls, with the permission checks
attached to that vocabulary and to nothing else. A process records which ABI it
was created under, and the dispatcher translates on entry and on exit. A second
ABI adds a table, not a code path — and it cannot add a second copy of a check,
because the checks live on the far side of the translation.

The Linux table that exists today becomes the first such translator, which is
also the migration: it is not rewritten, it is moved behind an interface it
already has the shape of.

### The GUI, and why it is mentioned in a core document

It runs in the kernel and it is the only subsystem the boot gate does not
assert. Today it is also the prime suspect for the argv corruption — white
pixels turning up in another process's memory. That combination is the argument
for the contract above, in miniature: no registered lock, no counters, no
must-be-zero, no sabotage file, and a bug that has been invisible for months.

## What this costs, and where it must not

Every seam is an indirect call. On the paths that run millions of times a boot
that is not free, and a refactor that quietly adds a call to the syscall entry
path would be a real regression that nothing here would currently notice —
**because this project has no performance measurement at all.**

So the rule is stated before the refactor rather than discovered after it:

- **Registration is resolved once, not per call.** A driver is looked up when a
  device is opened, not on every request. An ABI is bound when a process is
  created, not on every syscall.
- **The hot paths are the syscall entry, the page fault and the context
  switch.** An indirect call may not be added to any of the three without a
  measurement showing what it cost.
- **A baseline exists before any of this starts.** C0 establishes it. Without
  one, every later claim about performance is invented, and this project has a
  rule about numbers nobody measured.

## What this architecture does not do

It does not introduce a module system, dynamic loading, or driver isolation.
Those are real gaps and are on the roadmap; none of them can be built on a
kernel whose subsystems do not yet have a stated shape, and attempting them
first is how the shape gets decided by accident.
