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

Still open inside step 0, in the order they should be taken: fork snapshots the
break without holding it; exec in a threaded process does not end the siblings;
a leader that exits before its threads can be reaped early; descriptors are still
per thread.

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
