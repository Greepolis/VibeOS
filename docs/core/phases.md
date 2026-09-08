# Phases

Each phase says what it builds, what proves it, and what turns it red. Every
phase ends the same way: host tests, a sabotage case file under
`scripts/dev/cases/`, and — where the phase reaches the running machine — a
boot-gate assertion. A phase with no sabotage case is not finished, because
this project has three times shipped a check that could not fail.

The order is not negotiable in three places and all three say so.

Read `architecture.md` first. These phases build the shape it describes; on
their own they are a tidying plan, which is what the first version of this file
was and why it was wrong.

---

## C0 — the machine is measurable before anything moves

**Objective.** The boot is repeatable, and there is a performance baseline.

**Two things, and the second is new.** The first is the open memory defect: a
large refactor on a machine with an intermittent memory bug makes every later
failure attributable to two things, and this project has already lost sessions
to exactly that. The second is that **this kernel has no performance
measurement of any kind**, and the architecture in the next phases adds
indirection to paths that run millions of times a boot. Without a baseline,
every claim about what that cost would be invented.

**What is open on the defect.** An argv vector that reads `0x00ffffff00ffffff`
— two white pixels, `COL_TITLETXT` from the GUI — and separately as the
free-page poison. The same white-pixel value turned up independently as an
out-of-range sector in the block layer. The desktop is writing over memory that
belongs to something else, which the code comments already describe from a
previous occurrence, including its signature: every memory counter zero,
because nothing is freed early and frames are handed out twice.

**The baseline to establish.** Cheap, already-recorded, and hard to argue with:

- time from `kernel_early_init` to `BOOT_OK`, from the gate's own
  `phase_history` — it is being recorded now and nobody reads it;
- syscalls per boot and cycles per syscall, sampled;
- the three hot paths named in `architecture.md` timed individually: syscall
  entry, page fault, context switch.

**Done when.** Sixteen consecutive boots with no `POISON_BROKEN`, no
`free_while_mapped`, and no `bad-args` refusal — the number the kernel-stack
fix was held to, chosen because this project has a rule about deciding
intermittent questions with small counts. And a `[PERF]` line the gate records
and ratchets: a regression beyond a stated margin turns the boot red.

**Sabotage.** `mm-argv-poison.txt` exists and carries the measurements so far.
Add the case that puts the defect back once it is found. For the baseline:
`core-perf.txt` — make a hot path slower on purpose and confirm the ratchet
fires.

---

## C1 — the subsystem contract, and the checks that watch it

**Objective.** The seven parts of a subsystem in `architecture.md` stop being
folklore.

**Why before any code moves.** Every phase after this one creates or reshapes a
subsystem. Doing them first and writing the contract afterwards means writing
down whatever was built, which is how the current situation arose: five
registration seams, each invented separately, none named.

**Steps.**
1. `check-subsystem.py` — for each subsystem directory: one header, state
   confined to one `.c`, a lock registered rather than assumed, a stats struct
   with at least one must-be-zero field, an init that returns a reason, and a
   case file. Ratcheted; today's violations are the baseline and may only go
   down.
2. `check-reachable.py` — already written. Nothing in the portable kernel may
   be named by no one. It reports 12 against a baseline of 27, and the two that
   matter are the second dispatcher.
3. `check-chokepoints.py` — each security check declares how many call sites it
   has. A new one is not forbidden, it is *noticed*: the failure this guards
   against is one choke point silently becoming two that drift.

**Done when.** All three run in `check.sh all` and each has been shown to go
red by breaking the thing it watches.

**Sabotage.** `core-contract.txt` — a subsystem with no lock; a counter no
must-be-zero covers; a second call site for `hw_signal_permitted`; a function
in `kernel/core/` nobody names.

---

## C2 — the second dispatcher is fixed or deleted

**Objective.** `kernel/core/syscall.c` and `kernel/proc/process.c` stop being a
liability with a plausible name.

**The three defects, restated so a fix cannot be vague.** Thread creation with
no check on the caller. A process slot that is never freed — the same shape as
the thread-slot leak, which surfaced as a fork refused on a machine with plenty
of memory and took a day to attribute. And a caller identity read from an
argument the caller supplies, which is not a weak security model but the
absence of one.

**The gate, and it is absolute.** Nothing here may become reachable from ring 3
before those three are closed. Not afterwards, not in the same change. If the
answer is deletion this is satisfied trivially, and that is a point in
deletion's favour.

**How the decision gets made.** C3's ABI translator needs a place for the
kernel's own syscall vocabulary. Either this code becomes that place, with the
defects closed first, or it is deleted and C3 builds the vocabulary from the
live path. The choice is C3's requirement, not taste.

**Sabotage.** `core-dispatcher.txt` — if fixed: create a thread as another
process, exhaust the slot table, lie about the caller id. If deleted: confirm
`check-reachable.py`'s baseline drops and nothing references it.

---

## C3 — one syscall vocabulary, and ABIs as registered translators

**Objective.** The kernel has its own syscall vocabulary with the permission
checks attached to it, and Linux becomes the first translator onto it.

**Brought forward, deliberately.** The first version of this plan had the
syscall work last, as the most dangerous move. It is still the most dangerous,
but leaving it last was wrong for a different reason: the next refactor on the
roadmap is a multi-ABI syscall layer, and every phase that lands before this
one would have to be redone if the vocabulary turns out to be shaped wrong. The
danger is handled by the checks C1 builds, not by postponement.

**Steps.**
1. `vibeos_syscall_id` — the kernel's vocabulary, not Linux's numbers.
2. Each entry declares which checks apply. The checks stay where they are and
   are called by the dispatcher; they do not travel with handlers, because that
   is precisely how one choke point becomes several.
3. `vibeos_abi_t` with `classify`, `marshal`, `result`. The existing Linux
   table becomes its first implementation — moved, not rewritten.
4. A process records its ABI at creation. Bound once, not looked up per call.

**Done when.** Adding a syscall without declaring its checks fails a host test
rather than a review, and a second ABI would be a table rather than a code
path. `check-chokepoints.py` still reports one site per check.

**Sabotage.** `core-syscall.txt` — remove a check from a handler and confirm
the enumeration names it; declare a check that is not run; run one that is not
declared; add a second ABI and confirm no check was duplicated.

---

## C4 — one owner for "what is a task"

**Objective.** The task table has a single definition; the arch layer keeps
only what a context switch needs.

**Why after the syscalls rather than before.** It is the structure everything
reaches into, so moving it early means every later phase edits it again. C3
settles what a process is from the syscall side first; C4 then moves the
structure once.

**Steps.** `vibeos_task_t` holds identity, state, parent, exit status,
credentials and descriptors. `hw_task_t` keeps `ctx`, `kstack_*`, `cr3` and a
pointer across. The state machine and its transition table move with it — they
are the single source of truth about what READY means, and a second opinion is
how two cores ran one task.

**Done when.** `arch_hw.c` names no field that is not part of a context switch,
and `[TASKS] MUSTBEZERO` still reads zero.

**Sabotage.** `core-task.txt` — publish a slot before its state is set; let two
cores claim one task; free a kernel stack from the core standing on it. All
three are defects this project has had, and the move must not lose the guards.

---

## C5 — the scheduler decides, the arch layer switches

**Objective.** `hw_schedule` stops picking and starts obeying.

`kernel/sched/` already holds a policy, a run queue and a torture test that
found a real defect, and the boot path does not consult any of it. What changes
is which copy the machine reads; the risk is concentrated in one function, and
`charged + idle == seen` is a strong check on it.

**Sabotage.** `core-sched.txt` — return a task another core is running; ignore
the quantum; return idle while a ready task exists.

---

## C6 — the registries the next refactor needs

**Objective.** Character, input, network and display devices register the way
block devices do.

**Not in scope: isolation, hot-plug, dynamic loading.** Those are the driver
refactor's subject. What C6 owes it is that adding a driver does not mean
editing the core — which is true for storage today and false for everything
else.

**Done when.** The GUI is registered rather than wired in, and has the seven
parts: its own lock, its counters, a must-be-zero, and a case file. It is the
subsystem with none of those today and the current prime suspect for corrupting
another process's memory, which is the argument in miniature.

---

## What this plan still does not do

It does not make `kernel/core/` portable to a second architecture. There is no
second architecture, and building for one that does not exist is exactly how
the present portable kernel became untested and unreachable.
