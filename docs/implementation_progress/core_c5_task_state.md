# C5 - the task's state has one owner

Status, 2026-09-21: **done.**

## What it was

The task layer (`kernel/sched/task.c`) already owned the transition table - the one
place that says which state changes are legal - and `hw_task_t` kept a *second copy*
of the state, written after each transition. Two opinions about what READY means is how
two cores once ran one task, and the copy was also read from 52 places, so every
reader trusted whichever the code near it happened to use.

## What it is now

- `hw_task_t` has no `state`. `hw_slot_state(slot)` / `hw_task_state(task)` ask the task
  layer, and the only writer is `hw_task_set_state`, which calls the transition.
  The 52 readers were rewritten from the compiler's own error positions (`g_tasks[i].state`
  became `hw_slot_state(i)`, `t->state` became `hw_task_state(t)`), not by search and
  replace. The state array is `volatile` inside the task layer: other cores read it while
  a transition writes it, which the old field's `volatile` said and the new one must.
- **`ready_at` moved with it.** The tick a task became runnable was stamped by
  `hw_task_set_state` when the target was READY, on the argument that this is the only
  writer so no path can forget. That argument belongs to the transition, so it is
  stamped there now (`vibeos_task_ready_at`), and a new tenancy starts at zero.
- **`ran_once` became `vibeos_task_first_run`**: true once per tenancy, atomically, and
  reset when a slot is claimed. It was a flag the arch layer set by hand.
- `on_cpu` stays where it is: it is a fact about a CPU executing the task and only the
  context switch reads it.

## Gates

Host test (the stamp on READY and not on RUNNING, restamped on each return to READY, a
new tenancy starting unrun with no ready time, an out-of-range slot answering "no");
`cases/sched-task-state.txt` (three sabotages, each red on a named check) and a fourth
`core-task-identity.txt` case (a second `state` in `hw_task_t`). The five existing
`sched-task.txt` cases still go red.

## Measured, because the state read became a function call

The syscall and scheduling paths now call the task layer to read a state. `syscall_min`
under TCG, three boots each: **`c6efd0a` (before this session's work) 887-980,
`ebe9fae` 793-934, `f1e2b76` 489-560, this tree 776-949.** Nothing regressed, and that is
all it shows: the spread between builds of the *same* logic on the same host is wider
than any effect here, so the number cannot resolve a state read. The 229 quoted earlier
in this work came from another machine state and is not comparable. The ceiling (5000)
holds with a wide margin.

## Not done, as of the previous entry

`arch_hw.c` still names identity on 92 lines, and the fork of a whole task (allocation,
the child's registers and stack) is still the arch's - it needs the context and the
kernel stack, which are what the arch layer is *for*. The phase's done-condition is
about fields, and what remains in `hw_task_t` is now: the register frame, the address
space, the kernel stack, `on_cpu`, the FPU area, `fs_base`, the process pointer, the ABI
pointer and the diagnostic markers. Those are the context switch's.

## Closing the phase: the lifecycle leaves arch_hw.c (2026-09-22)

The 92 lines were not fields left in `hw_task_t` - they were *code*, spread through
`arch_hw.c`, that names identity while doing something that is not switching a
context: creating a task, describing it for a diagnostic, ending it, delivering a
signal, killing a faulted one. That code moved, whole, to a new file,
`kernel/arch/x86_64/task_life.c` - the same tool C4 stage 3 used (parse into top-level
items, build the reference graph, move a function iff everything that references it
moves too), pointed at fourteen roots (`hw_task_exit`, `hw_task_spawn_user`,
`hw_task_create_idle`, `hw_signal_raise`, the console interrupt, the ring-3 fault
killer, ...) instead of at the syscall handlers. 37 items, 1,605 lines.

**What stayed, and the line that says so is now a check.** Five functions *are* the
context switch - `vibeos_x86_64_isr_handler`, `hw_task_runnable`,
`hw_task_load_cpu_state`, `hw_ctx_check`, `hw_schedule` - and only they may still name
`.id.`. `check-task-identity.py` reads `arch_hw.c` function by function: identity
named anywhere else is a failure by itself, not just a count; the 12 lines those five
carry are the new ratchet, down from 92. `cases/core-task-identity-arch.txt` puts one
back in the slot allocator and confirms it is caught, named.

Twelve of the remaining lines were file-scope reads/writes of `pid`, `pgid`, `is_user`
and `service_id` from code that stayed (the diagnostics, the fault path, the console
handoff at boot) - not moved, because moving them would have meant moving their
callers too, which are switch-adjacent. They go through four small accessors instead
(`hw_task_pid_of`, `hw_task_pgid_of`, `hw_task_is_user_of`, `hw_task_set_service`,
defined with the lifecycle in `task_life.c`), the same shape as `hw_slot_state` /
`hw_task_state` for the state field itself.

`hw_fd_alloc` moved too - it is part of a task's lifecycle (claiming its first
descriptor), not part of switching to it.

**Two checks were found broken while working nearby it, both fixed, both given a
case:**

- `check-chokepoints.py`'s count for `hw_task_alloc_guarded` needed one more site:
  crossing into `task_life.c` cost it its `static`, so it gained a header
  declaration - the fourth of what was three. Recorded as the decision it is.
- `check-exec-layering.sh` (X-P4, kernel/exec/) named `arch_hw.c` by path and would
  have failed silently the moment the interpreter substitution moved with the
  lifecycle. Repointing it at `task_life.c` surfaced a second, older bug: it flagged a
  *comment* two thousand lines away that quotes a boot-log line containing
  `LDMUSL.SO` as an example - confirmed already red at HEAD, before any of today's
  work, by running the unmodified script against a clean worktree. Nobody had run it
  in a context where its exit code mattered, because it also always `exit 0`, so
  `check.sh` (which reads its printed text) never noticed and nothing else ever
  checked its return code. Both are fixed - comment lines are skipped, and it exits 1
  on failure - and `cases/core-exec-layering.txt` is its first sabotage case, proving
  both a real second path is still caught and the log-example comment is not.

## Done

C5 is closed: one definition of a task's identity, one table of its state, one module
for its descriptors, one module for pipes, and `arch_hw.c` naming a task's fields only
where it switches between them. `arch_hw.c` is 6,220 lines (was 12,174 before C4);
`task_life.c` is 1,643.

Full check green (clang and gcc, 3/3 boots, host tests, all sabotage cases including
the two written today). `syscall_min` not re-measured after this move - it touches no
hot path (creation, exit, signal delivery, diagnostics), and C0's ratchet would have
caught a regression in what it does touch.
