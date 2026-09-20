# Phases

Read `architecture.md` first. These phases build the shape it describes; on
their own they are a tidying plan, which is what the first version of this file
was and why it was wrong.

Each phase says what it builds, what proves it, what turns it red, and **which
of the four requirements it serves** — maintainable, modular, observable, fast.
A phase that serves none of them does not belong here.

Every phase ends the same way: host tests, a sabotage case file under
`scripts/dev/cases/`, and — where the phase reaches the running machine — a
boot-gate assertion. A phase with no sabotage case is not finished, because
this project has three times shipped a check that could not fail.

## What changed in this revision, and why the order moved

**C5 of the previous plan is already done.** `hw_pick_next` calls
`vibeos_sched_policy_pick` and falls back to `vibeos_runq_pick`. The scheduler
decides and the arch layer switches; the policy is a pure function tested
exhaustively; `hw_task_runnable` remains the single definition of runnable. It
is recorded here rather than deleted, because a plan that quietly loses a
finished phase is a plan nobody can audit.

**The ABI phase and the file-shrinking phase merged.** They were separate, and
that was the plan's biggest mistake: 41% of `arch_hw.c` *is* the Linux ABI, and
the next refactor on the roadmap moves it anyway. Doing them separately means
moving four thousand lines twice.

**Observability got its own phase, early.** Five must-be-zero counters exist in
the entire kernel. Every phase after it moves code, and moving code without
detectors is how this project spends days attributing a failure to two things.

---

## C0 — the machine is measurable before anything moves

*Serves: fast, observable.*

**Two things, and the second has never been done at all.**

### The open defect

A large refactor on a machine with an intermittent memory bug makes every later
failure attributable to two causes, and this project has already lost sessions
to exactly that.

State, from `scripts/dev/cases/mm-argv-poison.txt`: an argv vector reads
`0x00ffffff00ffffff` — two white pixels, `COL_TITLETXT` from `gui.c` — and
separately as the free-page poison. The same white-pixel value turned up
independently as an out-of-range sector in the block layer. Every memory
counter reads zero on those boots, which is the signature the code comment
above the GUI back-buffer already describes: nothing is freed early, frames are
simply handed out twice.

The open question is written in the case file: does `hw_alloc_pages_contig`
reserve every frame in the run it returns, or return a run the frame table
still believes is free? Answer it with an instrument, not a reading — three
careful readings of the exit path missed the kernel-stack defect, and what
named it was the `freed_by` tag.

### The baseline

There is no performance measurement in this kernel of any kind. Build it in the
order given, because each step is useful before the next exists:

1. **Boot wall time**, `kernel_early_init` to `BOOT_OK`, from the gate's own
   `phase_history` — recorded since the gate existed and never read. No kernel
   change at all.
2. **Syscalls per boot** and total dispatcher cycles. One counter, one `rdtsc`
   pair.
3. **A cycle histogram for each of the three hot paths** — syscall entry, page
   fault, context switch. Bucketed, with min/median/p99, because a mean hides
   exactly the regression worth catching.

**Done when.** Sixteen consecutive boots with no `POISON_BROKEN`, no
`free_while_mapped` and no `bad-args` refusal — the number the kernel-stack fix
was held to. And a `[PERF]` line the gate ratchets, with the margin written
down rather than implied.

**Sabotage.** `mm-argv-poison.txt` exists and carries the measurements; add the
case that puts the defect back once it is found. `core-perf.txt` — slow a hot
path on purpose and confirm the ratchet fires. Do that one *first*: a ratchet
nobody has seen go red is a ratchet with no evidence behind it.

**Done (2026-09-17).** The open defect turned out to be two, both closed. The
argv-poison/white-pixel corruption was SSE/XMM state never saved
(`mm_no_fpu_context.md`); the intermittent THREADS crash was a task running with
scheduler state READY after an in-place futex wake, then picked while it exited
(`boot_repeatability.md`, "found and fixed"). `rmap_mismatch` was the audit
bracketing only the owner count, not the holder side - completed, and a real one
now arrives named. The baseline is `core-perf.txt`: the boot gate ratchets
`syscall_min` (cycles, the machine-speed-independent number) against a
characterised ceiling and `boot_wall_s` (kernel_early_init to kernel_boot)
loosely; both were confirmed to fire before being trusted. A p99 histogram was
deliberately not built - the design chose the minimum over the mean and the p99
for the reason argued at `hw_perf_t`, and the minimum is the ratchetable one.

---

## C1 — the contract, and the checks that watch it

*Serves: maintainable, modular.*

**Objective.** The seven parts of a subsystem stop being folklore, and blast
radius becomes a number.

**Why before any code moves.** Every later phase creates or reshapes a module.
Writing the contract afterwards means writing down whatever was built — which
is precisely how the present situation arose: eight registration seams, each
invented separately, none named.

**Steps.**

1. **`check-subsystem.py`** — **done**, for the four parts a script can judge.
   50 modules: `exported_state=0` (every module already confines its state — the
   one part of the contract this tree got right without being told), plus
   baselines `no_header=15`, `no_case=35`, `state_without_lock=3`.

   Two of the seven are deliberately left out and the file says so rather than
   implying coverage: the **must-be-zero counter** is C2's step 3, which adds the
   counters before the check that demands them — writing it now would ship a
   check red against thirty modules, which is the state `check-mm-layering.sh`
   sat in for a phase. **An init that names its reason** stays in review.

   The lock property was narrowed after its first run: asking "has a registered
   lock" called `blkdev.c` unlocked while it holds its own, 29 false positives.
   It asks "has mutable file-scope state and names no lock anywhere" now, which
   is 3 — `vfs.c` (the mount table), `anon.c` (a clock hand), `forkguard.c` (the
   fork budget). `backing.c` was that exact shape, and it was right for months on
   the accident of having one caller.
2. **`check-blast-radius.py`** — **done**, and it corrected the step that asked
   for it. This said "storage is 1 and block is 1 today". Measured: **4, 4, 4,
   4, 3, 2** across six points, total 21. Nothing is 1; the floor is 2; and the
   one filesystem that uses the registry costs the same as the four that are
   hardcoded into `g_probes[]`. The baseline is the measurement, and the target
   it implies is sharper than the one this plan started with: a seam a driver
   joins without anybody editing a bring-up path.
3. **`check-chokepoints.py`** — each security check declares how many call
   sites it has. A second site is not forbidden, it is *noticed*: the failure
   guarded against is one choke point silently becoming two that drift.
4. **`check-reachable.py`** — written, running, `unreached=12 baseline=27`.
   Wire it into `check.sh all` if it is not there yet.

**Done when.** All four run in `check.sh all` and **each has been shown to go
red** by breaking the thing it watches. A checker whose red has never been seen
is in the same category as `VIBEOS_BLK_TIMEOUT`.

**Sabotage.** `core-contract.txt` — a module with no lock; a counter no
must-be-zero covers; a second call site for `hw_signal_permitted`; a function in
`kernel/core/` nobody names; a driver added by editing a registry.

---

## C2 — every module says whether it is broken

*Serves: observable.*

**Objective.** The must-be-zero count goes from five to one per module, and the
gate asserts all of them.

**Why here rather than at the end.** Every phase after this one moves code
between files. The detectors are what make such a move provable rather than
plausible, and this project's entire history says the same thing: the hard
defects were found by counters and witnesses, never by review.

**Steps.**

1. Enumerate the modules with no must-be-zero. Roughly: the GUI, the console,
   the keyboard and mouse, the network stack, exec, the ABI surface. The GUI is
   first, because it is the current prime suspect in C0's defect and has none of
   the seven parts.
2. For each: name the harm, add the counter, print it on the module's tag,
   assert it in the gate, and **demonstrate it can be non-zero** before
   concluding anything from a zero.
3. Extend `check-subsystem.py` to fail a module that has none. Ratcheted.
4. Give the two heaviest detectors a **witness** as well as a count — which
   object, which word, what value, put there by whom. A count says how often; a
   count is what left `found=0 freed_by=0` unexplained for a phase.

**Done when.** `check-subsystem.py`'s must-be-zero baseline is zero violations,
and each new counter has one recorded run in which it was non-zero on purpose.

**Status (2026-09-19): done, with a stated remainder.** `check-subsystem.py`'s
`no_mustbezero` is **0 of 51** modules. That is reached three ways, and the
split is the honest part:

- **Real counters (16 modules).** GUI, console, keyboard, IO, network
  (`sock_stale_parent`, `sock_fd_aba`), mouse (`desync`, proved non-zero at every
  boot), the ABI surface (`unexpected_unimplemented` + `last_nr` witness + a
  deliberate ring-3 probe), and - through the new registry `kernel/core/mbz.c`,
  which keeps a count *and the last value that tripped it* per harm - the ELF,
  ext2, exFAT, FAT-chain, ISO9660, NTFS, GPT and partition-table parsers, the
  journal's recovery, the log sink's torn records and the scheduler's failed
  requeue. The boot prints one `[MBZ] MUSTBEZERO total= first= witness=` line and
  the gate asserts it. `test_mbz_all_demonstrated` runs last in the host suite and
  fails if any id was never seen non-zero; on its first run it named three that
  the suite had never reached, and they got tests.
- **Asserted elsewhere (4).** blockcache, swaparea, lifetime, backing already had a
  gated counter under another name; the ratchet greps the gate for that name, so
  deleting the assertion stops it counting.
- **Exempt by written reason (21).** Modules with no failure mode a counter could
  see: pure decision functions (`policy`, `security`, `net_policy`, `sched_policy`),
  a flag (`event`), backpressure (`channel`), refusals that *are* the behaviour
  (`forkguard`, `handle_transfer`), dispatch tables, read-only reporting. Ratcheted
  as `mustbezero_exempt`, which may only fall. **This is a judgement, and it is
  the weakest part of C2's "done when"**: the plan asked for a counter per module,
  and twenty-one modules have a sentence instead. Each reason is in
  `check-subsystem.py`'s `EXEMPT` table for a reviewer to argue with.

Remaining honest gaps: the scheduler's requeue refusal is unreachable through the
public API (the run queue is sized to the thread table) and is demonstrated by
breaking the invariant by hand; and the ratchet is a textual heuristic - it says
a counter is *declared and, for the registry, asserted*, not that its zero means
the harm cannot happen.

**Sabotage, finally run (2026-09-20).** The plan's `core-observability.txt` said
what would be caught and contained *no cases*: three claims nobody had made fail.
It has them now, and running them found two things the file had been asserting
falsely or hiding.

- *"A counter declared and never incremented"* was **not caught** for the network
  stack: `check-counters-produced.py` watched nine structs and `vibeos_inet` was
  not one of them, so removing the only increment of `sock_stale_parent` left every
  check green - while the file's own comment said the check would catch it. Now
  watched; the case names `vibeos_inet.sock_stale_parent`.
- *"Printed and not asserted"* and *"a witness that says how often, not which"* are
  red (`mustbezero-asserted`, and `mbz_counters_missing` when the witness leaves
  the `[MBZ]` line). The registry itself is broken two ways in
  `core-mbz-registry.txt`, both red.
- The per-module must-be-zeros (network, mouse, ABI) each have two boot cases in
  `core-observability-modules.txt` - non-zero, and the line missing - all nine run
  and red with the intended reason. **Running them found a witness naming the
  wrong object:** the ABI's `last_nr` was overwritten by the deliberate probe, so an
  accidental call for 1998 was reported as `1999`. It now records only unexpected
  numbers, and the case reports `1998`. A witness that names the wrong thing is
  worse than a bare count, because it looks like an answer.
- **A limit in the registry's own proof, recorded rather than hidden:**
  `test_mbz_all_demonstrated` asks about an *id*, not a call site, so removing one
  of a multi-site id's hooks stays green (the first sabotage of it did exactly that
  and scored NOT RED). It is covered for the single-site ids (ELF, log sink) and
  for at least one site of the rest. A per-site counter would close it and every
  site would need a test.

**Sabotage.** `core-observability.txt` — a counter declared and never
incremented; a must-be-zero printed and not asserted; a witness that reports a
count instead of an object.

---

## C3 — the second dispatcher is fixed or deleted

*Serves: maintainable.*

**Objective.** `kernel/core/syscall.c` (1,477 lines) and `kernel/proc/process.c`
(1,115) stop being a liability with a plausible name. The arch layer names
`vibeos_syscall` **zero** times: nothing reaches this code.

**The three defects, restated so a fix cannot be vague.** Thread creation with
no check on the caller. A process slot that is never freed — the same shape as
the thread-slot leak, which surfaced as a fork refused on a machine with plenty
of memory and took a day to attribute. And a caller identity read from an
argument the caller supplies, which is not a weak security model but the absence
of one.

**The gate, and it is absolute.**

> Nothing here may become reachable from ring 3 before those three are closed.
> Not afterwards. Not in the same change.

If the answer is deletion this is satisfied trivially, and that is a point in
deletion's favour.

**How the decision gets made — it is C4's, not taste.** C4 needs somewhere for
the kernel's own syscall vocabulary. Either this code becomes that place with
the defects closed first, or it is deleted and C4 builds the vocabulary from the
live path. Decide it by asking which produces the smaller diff in C4, and write
the answer down here before starting C4.

**Sabotage.** `core-dispatcher.txt` — if fixed: create a thread as another
process, exhaust the slot table, lie about the caller id. If deleted: confirm
`check-reachable.py`'s baseline drops and that nothing references it.

**Status (2026-09-20): done - by deletion, the gate enforced, and the dispatcher's
orphaned vocabulary removed.** The three headers left behind by the deletion
(1,023 lines, included by two files that used nothing from them) went on
2026-09-20; before that C3 was done in the sense of its own criterion but left
a second, dead vocabulary with a misleading name.

*(Earlier status line, 2026-09-19:)* **done - by deletion, with the gate now enforced.**

*The decision, written down as the plan requires before C4 starts.* Deleted,
not fixed (commit `4462174`, 2026-09-10). The rule was "which produces the
smaller diff in C4", and measuring it went against integrating: both dispatchers
were `switch` statements, C4 wants a table (a syscall id mapped to the checks
that apply), so integrating meant carrying 1,502 lines of the wrong shape into
the change meant to fix the shape. The twenty-three lines that *were* the right
shape are kept as prose in `docs/core/architecture.md`. The deletion also
established that `vibeos_sec_*` appears zero times in the arch layer: **the
kernel that boots has no capability model at all**, so C4 is "there is none on
the metal", not "move the policy layer down". `check-reachable.py`'s baseline
went 27 to 12 with it.

*What this phase added today.* The gate is absolute ("nothing here may become
reachable from ring 3... not in the same change") and nothing enforced it: a
baseline that only goes down is *pleased* when a new caller appears, because
the unreached count falls. `check-reachable.py` now has a **fence**: no symbol
defined in `kernel/proc/process.c` - the second process model that still carries
the slot and caller-identity defects, and is C5's subject - may be named by
`kernel/arch`, `boot` or `user`. `kmain.c` and `waitset.c` name a few of them and
are allowed to: that is the portable kernel building and consulting its own table
at boot, not a path a syscall takes. `scripts/dev/cases/core-dispatcher.txt` makes
the arch layer name `vibeos_proc_init`; the run goes red with `fenced: ...
names vibeos_proc_init` and passes again when restored. The fence lifts in C5.

Not done, and stated: the three defects themselves are still in `process.c`
(closed by deletion for `syscall.c`, but `process.c` is kept). They are
unreachable from ring 3, which the fence now guarantees rather than assumes.

---

## C4 — one syscall vocabulary, ABIs as translators, and 41% of the file leaves

*Serves: modular, maintainable, and it is the monster-file phase.*

**Objective.** The kernel has its own syscall vocabulary with the permission
checks attached to it; Linux becomes the first translator onto it; and the four
ABI sections — **4,169 lines, 41% of `arch_hw.c`** — move to `kernel/abi/linux`
as part of that move rather than as a separate cut.

**Why these are one phase.** Splitting them was the previous plan's worst
decision. Cutting the file first produces four smaller files with the same
shape, which then all get rewritten when the ABI interface arrives. Building the
interface first and moving the code through it does the work once. It is also
the only version in which the cut is *verifiable*: code that moves behind an
interface either satisfies it or does not compile.

**Steps.**

1. `vibeos_syscall_id` — the kernel's vocabulary, not Linux's numbers.
2. Each entry declares which checks apply. The checks stay where they are and
   are *called by* the dispatcher; they do not travel with handlers, because
   that is exactly how one choke point becomes several.
3. `vibeos_abi_t` with `classify`, `marshal`, `result`. The existing Linux table
   becomes its first implementation — moved, not rewritten.
4. A process records its ABI at creation. **Bound once, not looked up per call**
   — C0's syscall-entry histogram is what proves that claim rather than asserting
   it.
5. The four sections move: Linux syscall layer (679), socket syscalls (1,654),
   what a program needs once running (651), what a C runtime asks for (1,185).

**Done when.** Adding a syscall without declaring its checks fails a host test
rather than a review. A second ABI would be a table, not a code path.
`check-chokepoints.py` still reports one site per check. `check-blast-radius.py`
reports **1** for "add a syscall". And the `[PERF]` syscall histogram has not
regressed past its margin.

**Sabotage.** `core-syscall.txt` — remove a check from a handler and confirm the
enumeration names it; declare a check that is not run; run one that is not
declared; add a second ABI and confirm no check was duplicated; look the ABI up
per call and confirm the perf ratchet fires.

**Status (2026-09-19): stages 1 and 3 done; stage 2 (the dispatcher runs the
checks, handlers register rows) is next.** Stage 1 is the vocabulary, the
declarations and their enforcement; stage 3 moved about 4,000 lines out of
`arch_hw.c` into `kernel/abi/linux/` (12,174 to about 7,800), cut by reference
graph rather than by line range, with the details in
`docs/implementation_progress/core_c4_abi.md`. Stage 3 was done before stage 2 on
purpose - see the reversal below - and it moved two metrics: "input device" went
4 to 5 (the handlers named the keyboard directly; fixed with a two-function
console seam) and "add a syscall" is a deliberate **6** until stage 2 lands.

*What exists now.* `include/vibeos/abi.h` declares each operation once, with its
checks, in an X-macro list (enum, names and check table cannot drift, and a line
without its checks does not compile). `kernel/abi/abi_linux.c` is the Linux ABI as
a translator: a constant `nr -> operation` table plus `clone()`'s flag decision,
which is why `classify` takes the first argument. The dispatcher switches on the
operation, not on Linux numbers; the ABI is bound to a task in `hw_task_alloc`,
the one place a slot is claimed, and read from the task per call - not looked up.
`scripts/dev/check-syscall-checks.py` (in `check.sh`) holds every declaration
against the dispatcher's call graph in both directions: declared-not-run,
run-not-declared, declared-never-dispatched. Its first run corrected three of my
own declarations - `exit`, `exit_group` and `prctl` do touch user memory (the
join word, a name string) and I had written NONE. Sabotage:
`core-syscall.txt`, `core-syscall-decl.txt` - five cases, all red with the
operation and check named.

*Measured.* `[PERF] syscall_min` is 229 cycles against a historical 208-440 and a
5,000 ceiling. The mean is not evidence (blocking inflates it - see C0).

*What the first run also found.* `vibeos_linux_translate_syscall` was being called
on every syscall, its result stored in a variable nothing read - "configured and
consulted by nobody" running on every call - and its `[COMPAT] translated=` counter
printed a number that meant nothing. Both gone. The `[MM] COW_STATS` block was
nested inside `if (vibeos_compat_stats(...) == 0)`, so the memory counters printed
only if a compat accounting call succeeded; that coupling is gone too.
`include/vibeos/syscall.h`, `syscall_abi.h` and `syscall_policy.h` still held the
*deleted* dispatcher's vocabulary - 1,023 lines that `user/lib/user_api.c` and a
host test included and used **no symbol of**. Deleted (C3's last increment); the
new vocabulary is named `vibeos_op_id` because the old word was taken.

*Not done, and the two remaining stages.*

- **Blast radius for "add a syscall" is 4, not the plan's 1** (it was 1 file before,
  with no declaration at all): abi.h, abi_linux.h, abi_linux.c and the handler plus
  its case in arch_hw.c. The regression is deliberate and recorded in
  `check-blast-radius.py`; getting to 1-2 means one row carrying number, operation
  and checks, which needs the handlers to leave arch_hw.c.
- **The checks are still inside the handlers.** The plan's step 2 has the
  *dispatcher* call them, so that "one site per check" is one site. Today
  `hw_user_range_ok` has 46 call sites; the enumeration proves each operation
  reaches one, not that it validates the right pointer. Moving them needs each
  operation to declare its pointer arguments (which argument, how long, in which
  direction), which is a per-syscall rewrite of 44 sites and the riskiest part of
  this phase. Stage 2.
- **The four ABI sections** were to follow stage 2 (so the 46 call sites would
  not be touched twice). **Reversed by the user's instruction and, on reflection,
  for the better:** the move is mechanical and compiler-verified, and stage 2 now
  edits smaller files it can hold in its head. The 46 call sites did travel, and
  stage 2 will touch them once, in their new home.

---

## C5 — one owner for "what is a task"

*Serves: maintainable, modular.*

**Objective.** The task table has a single definition; the arch layer keeps only
what a context switch needs. 1,363 lines, of which most leave.

**Why after C4 - and why it was taken before it.** It is the structure everything
reaches into, so moving it early means every later phase edits it again. C4 settles
what a process is from the syscall side; C5 then moves the structure once.

That ordering assumed the structure was *shaped* right and only in the wrong
place. It was not. Five external findings in two days - the mapping cursor, the
region list, signal dispositions, exit_group, and descriptors - were one defect:
`clone` copied `hw_proc_t` by value, so everything that belongs to a process was a
private copy per thread. Moving a structure with the wrong ownership model only
relocates the defect, and C4's handlers would have been written against it.

So C5 now has a **step 0**, done first: process state is referenced, not copied
(`hw_procstate_t`; see `implementation_progress/core_c5_process_state.md`). Its
tests were written before the kernel was touched and failed on it. The move below
is unchanged and still comes after C4.

Closed in step 0 (2026-09): WNOHANG, tkill, the blocking waits; exec in a
threaded process ends the siblings and takes the leader's id (H-006, `8a3903c`);
a leader that exited before its threads is released when exec takes its slot;
and `hw_procstate_t` has a must-be-zero - `procstate_double_put`, a process
reference given back that nobody held, on the `[TASKS] MUSTBEZERO` line and
asserted by the gate.

Partly closed: there is now one per-process address-space lock (`hw_mm_lock`,
`mm_busy`), bounded so a missed release is a named panic not a hang. brk takes
it, and fork takes it across its read of the parent's page tables and region
list; munmap and mmap take it too (mmap's mutation was extracted into
`hw_mmap_locked` so one lock/unlock covers its six returns). mprotect takes it
across its region-list update (`vibeos_vma_protect`) and releases before its
page-table narrowing pass. So fork is now atomic against all four mm calls on
the *region list*.

**The shootdown-under-lock deadlock is closed.** fork holds the lock across
`clone_cow`, which itself does a TLB shootdown - so the very hazard first noted
for mprotect was already live in fork (`d6fe3bc`): a sibling spinning in
`hw_mm_lock` is a shootdown target that cannot ack with interrupts masked, and
will not leave the spin until fork releases - the shootdown's timeout panic.
`hw_mm_lock`'s spin now opens an interrupt window each iteration (`sti; pause;
cli`) so it can service the shootdown IPI; the spin holds no lock, so a timer
there is a safe preemption, and `g_current_task` is per-CPU so identity
survives it. Interrupts are restored to the caller's state on acquire.

Still open:

- **mprotect's page-table race with fork.** mprotect still releases the lock
  before narrowing, so fork's `copy_user` can read a PTE mprotect is narrowing -
  no worse than before, but not closed. Now that the spin services the IPI,
  mprotect *could* hold the lock across the narrowing too; a small follow-up.
- descriptors are still per thread.

**Steps.** `vibeos_task_t` holds identity, state, parent, exit status,
credentials and descriptors. `hw_task_t` keeps `ctx`, `kstack_*`, `cr3` and a
pointer across. The state machine and its transition table move with it — they
are the single source of truth about what READY means, and a second opinion is
how two cores once ran one task.

Note the constraint the move must preserve: `VIBEOS_HW_MAX_TASKS <= 64` is a
static assertion because the runnable bitmask is 64 bits wide. It replaced a
runtime check that had never once been false — which reads as a guard, costs a
comparison per slot, and would have silently dropped slots rather than failing.

**Done when.** `arch_hw.c` names no task field that is not part of a context
switch, `[TASKS] MUSTBEZERO` still reads zero, and the context-switch histogram
has not regressed.

**Sabotage.** `core-task.txt` — publish a slot before its state is set; let two
cores claim one task; free a kernel stack from the core standing on it. All
three are defects this project has had; the move must not lose their guards.

---

## C6 — diagnostics become a module

*Serves: observable, and the monster file.*

**Objective.** The kernel log (724 lines) and the crash records (56) move to
`kernel/diag`. The UART stays behind a registered sink; everything above it is
the same answer on any architecture.

**Why it is worth its own phase.** These two are the instruments every other
phase is verified with, so moving them is the one move that can invalidate its
own evidence. It goes late, after C2 has given the rest of the kernel detectors
that do not depend on them, and its own verification is the interleaving check
that already exists.

**Sabotage.** `core-diag.txt` — an unmatched unlock; a multi-part message left
unbracketed; a sink that drops a line silently. The first two are defects this
project has had, and one of them invented crashes that never happened.

---

## C7 — the registries the next refactor needs

*Serves: modular. This is the phase the driver refactor is waiting on.*

**Objective.** Character, input, network and display devices register the way
block devices do. `check-blast-radius.py` reports **1** for every row of its
table.

**Not in scope: isolation, hot-plug, dynamic loading.** Those are the driver
refactor's subject. What C7 owes it is that adding a driver does not mean
editing the core — true for storage today, false for everything else.

**Done when.** The GUI is registered rather than wired in, and has all seven
parts: its own lock, its counters, a must-be-zero, and a case file. It is the
subsystem with none of those today and the prime suspect for corrupting another
process's memory, which is the whole argument in miniature.

---

## Already done, recorded so it is not re-planned

**The scheduler decides and the arch layer switches.** `hw_pick_next` asks
`vibeos_sched_policy_pick` first and `vibeos_runq_pick` as a fallback — a
fallback rather than an idle, deliberately, because a task the policy was never
told about is an admission bug and refusing to run it turns that bug into a hang
instead of a slower machine. `charged + idle == seen` watches it.

---

## What this plan still does not do

It does not make `kernel/core/` portable to a second architecture. There is no
second architecture, and building for one that does not exist is exactly how the
present portable kernel became untested and unreachable.

It does not set a target line count for `arch_hw.c`. That criterion has been
tried twice and failed twice — both times the file grew while the document
naming its length as the completion criterion sat unchanged. The criterion is
blast radius; the file shrinks as a consequence, by about 7,700 lines if every
phase lands.
