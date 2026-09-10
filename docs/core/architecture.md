# The shape of the core

The first version of this document said where code should live. That is a
tidying plan, not an architecture, and it would have produced a smaller
`arch_hw.c` extended the same way — by editing it.

This version is organised around the four requirements, and each is stated with
the number that decides whether it has been met. A requirement without a
measurement is a preference, and this project has a rule about those.

---

## 1. Maintainable

### The definition, because the word on its own means nothing

`CLAUDE.md`, read as a whole, is a catalogue of maintenance failures. Not one
of them was code that was hard to *read*. Every single one was a change that
was **wrong for months with nothing saying so**:

- a field written on one path and read on all of them (`interp_base`);
- a struct built member by member that later gained a member (`read_many`);
- a layer serialised by the accident of having one caller (`backing.c`);
- a counter defined, printed, asserted, and produced by nothing
  (`VIBEOS_BLK_TIMEOUT`);
- a check that had been red since before anybody arrived
  (`check-mm-layering.sh`);
- a mechanism configured and consulted by nobody, three times in one session.

So maintainability here is **the number of ways a change can be silently
wrong**, and it decomposes into three quantities that can each be measured:

| Property | Question | Measured by |
|---|---|---|
| **Blast radius** | How many files must I edit to add one thing of a known kind? | `check-blast-radius.py` |
| **Time to red** | How long between making that mistake and something saying so? | wall clock on `check.sh all`, and whether a sabotage case exists at all |
| **Locality** | Is there exactly one place that decides this? | `check-reachable.py`, `check-chokepoints.py` |

### Blast radius is the one that matters most

This section used to open by saying blast radius was "already excellent in one
place and terrible everywhere else", and gave this table:

> | a filesystem | **1** — `vibeos_storage_register` |
> | a block driver | **1** — `vibeos_blk_register` |
> | everything else | `arch_hw.c` |

**That was wrong, and it was wrong in the document that calls this the plan's
real progress metric.** It was written from the existence of two registries, not
from a measurement. `check-blast-radius.py` was then written to enforce it and
contradicted it on its first run.

| To add... | Files edited today | which |
|---|---:|---|
| a filesystem, the way four of the five arrived | **4** | `storage.c`, `storage.h`, `io_bringup.c`, the source list |
| a filesystem through the registry (FAT, the only one) | **4** | `arch_hw.c`, `arch_x86_64.h`, `io_bringup.c`, the source list |
| a block driver | **4** | `arch_hw.c`, `arch_x86_64.h`, `kmain.c`, the source list |
| a network interface | **4** | `arch_hw.c`, `arch_x86_64.h`, `kmain.c`, the source list |
| a display | **3** | `arch_hw.c`, `serial.c`, the source list |
| an input device | **2** | `arch_hw.c`, the source list |

Three things follow, and none of them was visible before the number existed.

**Nothing measures 1.** The floor across the whole tree is 2.

**The registry buys nothing yet.** FAT is the one driver that registers, and it
costs the same 4 as the four filesystems that do not: somebody still calls the
register function, declares it, and mounts through it. *A registry whose members
need a bring-up call has the same blast radius as no registry at all.* That is
the sentence C6 has to falsify, and it is a much sharper target than "move code
out of `arch_hw.c`".

**The four filesystems were never evidence.** ext2, ntfs, exfat and iso9660 are
named directly in `g_probes[]` in `kernel/fs/storage.c`, each with a mount
wrapper there and a member in `vibeos_volume_t`. They did not arrive without
their layer being touched; they arrived by editing it, four files at a time.

What survives from the old paragraph is the observation underneath it: there are
eight registration seams in this tree (`vibeos_frame_set_lock`,
`vibeos_rmap_set_lock`, `vibeos_cache_set_lock`, `vibeos_blockcache_set_lock`,
`vibeos_swapmap_set_lock`, `vibeos_task_view_set_source`, and the two
registries), each invented separately by somebody solving one problem, and never
named as a rule.

> **A subsystem is extended by registering into it, never by editing it** — and
> registering must not require an edit either, or the seam is decoration.

`check-blast-radius.py` turns that from advice into a number: for each extension
point it names the files a new implementation touches, and the count may only go
down. It is ratcheted at today's measurement — 21 across six points.

### Time to red

A mistake that turns something red in ninety seconds costs ninety seconds. The
same mistake caught by a reader six weeks later has, in this project's own
history, cost between a day and a full phase. Three properties follow:

- **Every module has a sabotage case file.** 48 exist; the modules that lack one
  are exactly the modules whose defects were found by reading.
- **A check that fires must be suspected before it is believed.** Three
  detectors in one session reported defects that were their own bugs, and one
  reported 3,019 use-after-frees in a boot that had none.
- **A check red since before you arrived is a check nobody reads.** A limit is
  raised as a decision, in the change that earns it.

### Locality

The one-definition rule, and the property this refactor is most likely to
break: moving handlers out of a single file is precisely the operation that
turns one choke point into two, and the second one always forgets a case.

---

## 2. Modular and well-structured

### What a module is — seven parts, none optional

Every subsystem here that works has the same seven. Nobody wrote them down, so
each new one was assembled from memory and several arrived missing a piece —
which is how a page cache with no lock ran for months on the accident of having
exactly one caller.

1. **One header** declaring the types and the entry points.
2. **One `.c` owning all of its mutable state.** No other file names its
   globals — what `check-mm-layering.sh` already enforces for page tables,
   generalised.
3. **Its own lock, registered rather than assumed.** *Its own*, because the page
   cache allocates frames and handing it the frame layer's lock would deadlock
   on the first miss. *Registered*, because a weak symbol does not resolve
   across objects under PE/COFF and the Windows job proved it.
4. **A statistics struct with at least one must-be-zero counter**, so that "this
   is broken" is a number rather than a log line somebody has to read.
5. **An init that can fail and names the reason.** A refusal naming a mechanism
   instead of a situation costs a session — `pml4_absent_or_not_user` held one
   open from end to end.
6. **A registration point**, if anything can be plugged into it.
7. **A sabotage case file.** Three checks here have shipped unable to fail.

`check-subsystem.py` enforces the mechanical parts. The parts a script cannot
judge stay in review, and this document says which is which rather than
pretending the script covers everything.

### The layering test, stated as a question rather than a word

"Portable" is the wrong test and produced the current situation: a portable
kernel that is portable and does not run.

> Would a second architecture need a **different answer**, or the **same answer
> implemented differently**?

Same answer, different implementation → the arch layer provides a mechanism and
the core decides. Picking the next task is the same answer everywhere; writing
`cr3` is not. Deciding a signal is permitted is the same answer everywhere;
building the frame it is delivered on is not.

Different answer → the arch layer owns it entirely. There is no core opinion
about what an IDT is.

This is deliberately **not** "no `#ifdef` in core". There is one architecture,
and building for a second that does not exist is how the present portable kernel
became untested and unreachable.

### The module inventory

Every one of `arch_hw.c`'s nineteen sections, measured, with a destination.
This is the structure, and it is also the answer to the monster file.

| Section | Lines | Destination | Why |
|---|---:|---|---|
| socket syscalls | 1,654 | `kernel/net` + `kernel/abi/linux` | same answer everywhere |
| task table + scheduler entry | 1,363 | `kernel/proc` + arch switch | the decision leaves, `cr3` stays |
| C-runtime syscalls | 1,185 | `kernel/abi/linux` | it is an ABI by definition |
| the kernel log | 724 | `kernel/diag` | same answer; the UART is the mechanism |
| page pool + address spaces | 685 | split: policy out, page tables stay | different answer per arch |
| APIC + SMP bring-up | 679 | `arch/smp` | **stays** — different answer |
| Linux syscall layer | 679 | `kernel/abi/linux` | the translator itself |
| what a program needs running | 651 | `kernel/abi/linux` | ABI surface |
| page cache | 588 | `kernel/io` | already has a home there |
| process creation | 510 | `kernel/exec` (116 lines today) | ELF is not architecture |
| IDT | 396 | `arch/trap` | **stays** |
| swap bridges | 316 | `kernel/mm` | already has a home |
| SMP locking | 146 | `arch/cpu` | **stays** |
| networking bring-up | 145 | `arch/net` | **stays** |
| per-CPU state | 133 | `arch/cpu` | **stays** |
| crash records | 56 | `kernel/diag` | same answer everywhere |
| paging | 47 | `arch/mm` | **stays** |
| SYSCALL/SYSRET | 37 | `arch/syscall` | **stays** |
| GDT + TSS | 28 | `arch/cpu` | **stays** |

Leaves: **7,726 lines**. Stays: about 2,360. And the largest single block that
leaves — 4,169 lines, **41% of the file** — is the Linux ABI, which the next
refactor on the roadmap has to move anyway.

> **Shrinking the monster file and building the multi-ABI syscall layer are the
> same work. Doing them separately means doing the larger one twice.**

That is this document's central structural claim, and it is why the phase order
changed.

### The extension points the next year needs

The roadmap has three refactors after this one: drivers and peripherals, the
network, and a syscall layer carrying Linux, Windows and macOS ABIs. An
architecture that does not make those cheap is not worth the disruption of
adopting.

**Drivers.** `vibeos_blk_register` is the model and it works. What is missing is
that it is the *only* one. The core provides the registry and the contract; a
driver provides a struct; nothing in the core is edited to add one. True for
storage, false for everything else.

**Syscall ABIs.** An ABI is **not** a branch in the dispatcher. Three ABIs as
three branches is three copies of every permission check, and the checks are
where every security defect in this project has been found. An ABI is a
registered translator:

```
vibeos_abi_t {
    name;                                  /* "linux", "win32", "macho"   */
    classify(frame) -> vibeos_syscall_id;  /* their number to ours        */
    marshal(id, frame) -> vibeos_args_t;   /* their calling convention    */
    result(id, value, frame);              /* our answer in their shape   */
}
```

### The shape C4 starts from, lifted out of the code before it was deleted

`kernel/core/syscall_policy.c` held twenty lines that are exactly this idea,
and it was deleted with the dispatcher that was its only caller. The idea is
worth more than the file, so it is here instead:

```c
vibeos_syscall_policy_t vibeos_syscall_policy_for(vibeos_syscall_id_t id) {
    switch (id) {
        case VIBEOS_SYSCALL_EVENT_SIGNAL:
        case VIBEOS_SYSCALL_WAITSET_ADD_EVENT:
            return (policy){ .requires_handle = 1,
                             .required_rights = HANDLE_RIGHT_SIGNAL };
        case VIBEOS_SYSCALL_HANDLE_CLOSE:
            return (policy){ .requires_handle = 1,
                             .required_rights = HANDLE_RIGHT_MANAGE };
        default:
            return (policy){ 0 };
    }
}
```

One function mapping a syscall id to the checks that apply to it, with the
checks named rather than called - which is what lets a test assert that the
declared set is the set actually run. C4 needs that whether or not the code it
came from survives.

**And the thing the live kernel does not have at all.** Grepping the arch layer
for `vibeos_sec_*` returns nothing: there is no capability model, no handle
rights and no policy check on the path that actually serves ring 3. The design
existed only in the half nothing reached. C4 is where it becomes real, and this
is the note that stops it being reinvented from scratch.

The kernel keeps **one** vocabulary of syscalls, with the permission checks
attached to that vocabulary and to nothing else. A process records the ABI it
was created under; the dispatcher translates on entry and on exit. A second ABI
adds a table, not a code path — and it *cannot* add a second copy of a check,
because the checks live on the far side of the translation.

The Linux table that exists today becomes the first such translator. It is not
rewritten; it is moved behind an interface it already has the shape of.

**The GUI, and why it appears in a core document.** It runs in the kernel and is
the only subsystem the boot gate does not assert. It is also the current prime
suspect for the argv corruption — white pixels turning up in another process's
memory. No registered lock, no counters, no must-be-zero, no sabotage file, and
a bug invisible for months. The contract above, in miniature.

---

## 3. Observable

### What exists, counted

25 distinct report tags. **Five** must-be-zero counters in the whole kernel:
`frames_leaked`, `medium`, `double_free`, `bad_unlocks`, `illegal_transition`.
48 sabotage case files. A 2,020-line boot gate. Two checks that watch the checks
(`check-assertions-covered.py`, `check-counters-produced.py`).

The gap is not the tooling, which is unusually good for a project this size. The
gap is coverage: most subsystems report what they did and nothing about whether
it went wrong.

### The three questions every module answers

1. **What did you do?** Counters. Cheap, always on.
2. **What went wrong?** At least one must-be-zero, asserted by the gate. This is
   the one most modules lack, and it is the one that has found every hard defect
   in this project.
3. **What are you now?** A state dump on demand from the kernel CLI. `crash` and
   `logdisk` are the model.

`check-subsystem.py` enforces (2) — a module with no must-be-zero fails. That is
a stronger rule than `check-counters-produced.py`, which only asks that a
declared counter be written somewhere.

### Four rules, each paid for in full

**A counter nothing increments and a counter reporting nothing happening are
indistinguishable.** A new counter needs one deliberate demonstration that it
*can* be non-zero before anything is concluded from a zero. A disk-interrupt
counter read zero for an hour because the dispatcher hook had never been added,
and sent four measurements in the wrong direction.

**When a detector fires, the first question is whether the detector is right,
and it is never answered by a count.** A frame-poison check reported 3,019
use-after-frees in a boot with none. What settled it was making the detector say
*which frame, which word, what value, released by whom* — it answered on the
first boot. A counter says how often; only a witness says what.

**A sampled zero is not evidence.** The free-side mapping check looks at one
free in sixteen, so a defect happening once a boot is very likely to be missed.
A phase relying on those counters raises the sampling first and records what it
raised it to.

**Data nobody reads is not observability.** The boot gate has recorded
`phase_history` — per-phase timings — for its entire existence, and nothing has
ever read it. That is the performance baseline, already collected, sitting
unused. Section 4 reads it.

---

## 4. Fast

### The honest starting point

**This kernel has never been measured.** No benchmark, no timing, no budget, no
regression gate. Every statement anyone could make about its speed today,
including a reassuring one, would be invented — and this project has a rule
about numbers nobody measured.

That is what makes the performance requirement urgent rather than premature: the
seams above add indirect calls to paths that run millions of times a boot, and
without a baseline the refactor cannot be shown *not* to have cost anything.

### The three hot paths

**Syscall entry**, **page fault**, **context switch**. Nothing else is close. An
indirect call may not be added to any of the three without a measurement saying
what it cost.

### The rule that keeps the seams free

> **Registration is resolved once, not per call.**

A driver is looked up when a device is opened, not on every request. An ABI is
bound when a process is created, not on every syscall. A lock is registered at
init, not chased through a pointer on every acquisition. Under that rule a seam
is one predicted indirect call — a few cycles — and the danger is never the call
itself. It is the *lookup*, or the *lock*, that somebody adds beside it.

### The baseline, cheapest to build first

1. **Boot wall time**, `kernel_early_init` to `BOOT_OK`. Already recorded in
   `phase_history`, already unread. Zero new kernel code.
2. **Syscalls per boot**, and total cycles in the dispatcher. One counter and one
   `rdtsc` pair.
3. **A histogram per hot path** — cycles, bucketed, min/median/p99. The one that
   catches a regression a mean would hide.

A `[PERF]` line carries all three, the gate ratchets it, and a regression past a
stated margin turns the boot red. Like every other check here, it is confirmed
by making a hot path slower on purpose and watching it go red.

### What is deliberately not measured

Throughput of anything. There is no workload worth optimising for yet, and a
benchmark chosen before there is a reason to care about it becomes the thing
that gets optimised. Latency on the three paths is structural; everything else
waits for a reason to exist.

---

## What this architecture does not do

It does not introduce a module system, dynamic loading, or driver isolation.
Those are real gaps and are on the roadmap. None of them can be built on a
kernel whose subsystems have no stated shape, and attempting them first is how
the shape gets decided by accident.

It does not make `kernel/core/` portable to a second architecture. There is no
second architecture, and building for one that does not exist is exactly how the
present portable kernel became untested and unreachable.
