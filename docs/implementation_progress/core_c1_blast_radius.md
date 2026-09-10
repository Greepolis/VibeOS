# C1's last two checks, and what measuring cost the plan

`check-subsystem.py` and `check-blast-radius.py` are the two C1 asked for and
nobody had written. Both are wired into `check.sh all`, both are ratcheted at
today's measurement, and both have been broken on purpose and seen red
(`scripts/dev/cases/core-contract.txt`).

The interesting part is not that they exist. It is that **each one contradicted
the document that asked for it, on its first run.**

## check-blast-radius.py: the plan's progress metric was folklore

`docs/core/` says, in three separate files, that blast radius is the real
measure of this refactor — chosen precisely because line count had been the
stated criterion twice and failed twice. And the table it gave was:

> a filesystem — **1**, `vibeos_storage_register`
> a block driver — **1**, `vibeos_blk_register`
> everything else — `arch_hw.c`

That table was written from the *existence of two registries*, not from a
measurement. Here is the measurement:

| extension point | radius | the files |
|---|---:|---|
| filesystem, the way four of five arrived | 4 | `storage.c`, `storage.h`, `io_bringup.c`, source list |
| filesystem through the registry (FAT) | 4 | `arch_hw.c`, `arch_x86_64.h`, `io_bringup.c`, source list |
| block driver | 4 | `arch_hw.c`, `arch_x86_64.h`, `kmain.c`, source list |
| network interface | 4 | `arch_hw.c`, `arch_x86_64.h`, `kmain.c`, source list |
| display | 3 | `arch_hw.c`, `serial.c`, source list |
| input device | 2 | `arch_hw.c`, source list |

**Nothing is 1. The floor is 2.**

### The four filesystems were never evidence of anything

The claim rested on "four filesystems arrived without their layers being
touched". They did not. ext2, ntfs, exfat and iso9660 are named directly in
`g_probes[]` in `kernel/fs/storage.c`, each with a mount wrapper beside it and a
member in `vibeos_volume_t`. `vibeos_storage_register` has **one** caller in the
whole tree, and it is FAT.

### And the registry buys nothing yet, which is the finding worth keeping

FAT — the one driver that does use the seam — measures **4 as well**. Somebody
still calls the register function from `arch_hw.c`, declares it in
`arch_x86_64.h`, and mounts through it in `io_bringup.c`.

> A registry whose members still need a bring-up call has the same blast radius
> as no registry at all.

That is a much sharper target for C6 than "move code out of `arch_hw.c`", and it
was not sayable before the number existed.

### The check corrected its author too

FAT was hand-measured at 2 while the check was being written, by grepping
`vibeos_x86_64_fat_vfs` — which misses `_ops` and `_register_driver`, the two
symbols that *are* the seam. The first run said 4 and the declared number was
wrong. The same run also caught a matcher bug: a shared symbol prefix is not a
module, and `vibeos_x86_64_fat_` matches both the VFS driver and the raw FAT
reader, two different things, giving a radius of 7 for a module whose answer is
4. The matcher is a regex per point now.

## check-subsystem.py: one property was already perfect, and three are debts

50 modules, four mechanical properties out of the seven in
`docs/core/architecture.md`.

| property | today |
|---|---:|
| `exported_state` — state has a name another file can write | **0** |
| `no_header` | 15 |
| `state_without_lock` | 3 |
| `no_case` | 35 |

`exported_state=0` is worth stating rather than assuming. It is the one part of
the module contract this tree satisfies everywhere without having been told to,
and it is the property the other three rest on. It is also measured with `nm`
rather than a regex — lowercase symbol class is a `static`, uppercase is not,
and the linker's opinion is the only one that counts. That is why its sabotage
case is the one worth having: a property green across the board is
indistinguishable from a property nothing measures.

### The lock property had to be narrowed, and the first version was noise

It began as "does the module register a lock", which is what
`architecture.md` lists. That reported **29 of 50**, including `blkdev.c` —
which holds its own lock. A detector that flags a module doing the right thing
is a detector people learn to ignore, and this file has already recorded one
that reported 3,019 use-after-frees in a boot with none.

Narrowed to the shape that actually cost months — *mutable file-scope state and
no mention of a lock anywhere in the file* — it reports **3**:

- `kernel/fs/vfs.c` — the mount table
- `kernel/mm/anon.c` — a clock hand
- `kernel/sched/forkguard.c` — the fork budget

None of these is claimed to be a defect; each may be correct under a caller's
lock. They are three places where "is this serialised?" has no answer in the
file. `backing.c` was exactly this shape, was correct for months on the accident
of having one caller, and the second caller returned one file's pages under
another file's key.

### Two of the seven parts are deliberately absent

The **must-be-zero counter** is C2's step 3, which adds the counters before the
check that demands them. Writing it now would ship a check red against thirty
modules, and this project already knows what that produces: `check-mm-layering.sh`
sat failing and unread for a phase. **An init that names its reason** stays in
review, and the script says so instead of implying coverage.

## What this does to the plan

Three documents were corrected rather than appended to: `README.md`,
`architecture.md` and `invariants.md` all claimed a radius of 1, and
`phases.md`'s C1 step 2 asked for a baseline it had already guessed wrong.

The pattern is the one this project keeps producing and has now caught in its
own planning documents: **a number that is written and never read**. The plan
named blast radius the progress metric, gave it a value, and nothing measured it
for the entire time it was the metric.
