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

## Not done

`arch_hw.c` still names identity on 92 lines, and the fork of a whole task (allocation,
the child's registers and stack) is still the arch's - it needs the context and the
kernel stack, which are what the arch layer is *for*. The phase's done-condition is
about fields, and what remains in `hw_task_t` is now: the register frame, the address
space, the kernel stack, `on_cpu`, the FPU area, `fs_base`, the process pointer, the ABI
pointer and the diagnostic markers. Those are the context switch's.
