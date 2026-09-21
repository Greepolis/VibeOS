# C5 - what a task is, apart from what a CPU needs to run it

Status, 2026-09-21: **step 1 done (identity extracted); the phase is not finished.**
Step 0 (process state referenced, not copied) is `core_c5_process_state.md`.

## What moved

`hw_task_t` was one structure holding everything about a task: the saved registers,
the kernel stack, the address space and descriptors *and* who it is. Seventeen fields
of identity are now `vibeos_task_t` (`include/vibeos/task_ident.h`, code in
`kernel/sched/task_ident.c`), embedded in `hw_task_t` as `id`:

`pid tgid ppid pgid sid service_id is_thread is_user is_idle wait_input
signal_stopped exit_code exit_signal clear_child_tid comm sig_pending sig_blocked`

The portable type has host tests and functions, and the arch layer uses them:

- `vibeos_task_identity_reset` - **the one place identity is cleared**, called from
  `hw_task_alloc`. Before it, `alloc` reset three of these fields (`is_thread`,
  `service_id`, `clear_child_tid`) and a recycled slot kept the previous tenant's
  pending signals, exit status, pgid and name until each creator overwrote them - the
  shape of `interp_base` again. It clears the whole object byte by byte, so a field
  added later is cleared without anyone remembering to say so. The host test fills the
  structure with `0xA5` and needs every byte back to zero; its first version caught
  the reset *missing padding*.
- `vibeos_task_accountable_to` - the fork guard's "does this parent answer for this
  task" (child, or a thread of its group), replacing an inline condition.
- `vibeos_task_set_comm` - bounded and always terminated, replacing a loop in prctl
  that left the tail of the previous name.
- `vibeos_task_is_group_leader`.

## How the ~180 call sites were moved

Not by search-and-replace: `pid` and `ppid` also name fields of other structures. The
fields were removed from `hw_task_t` and the compiler then named every access - file,
line and column - which a small tool rewrote in place (`.pid` to `.id.pid`), round
after round until the build had no such error. Only accesses to `hw_task_t` can fail
that way, so nothing else was touched.

## What this does not finish

The phase's done-condition is that `arch_hw.c` names no task field that is not part of
a context switch. **It is not met**: arch_hw.c reaches into identity on 92 lines and
the Linux layer on ~126. `check-task-identity.py` states the two properties that can be
stated - `hw_task_t` may not declare an identity field again, and the 92 may only go
down - and is in `check.sh`. The descriptors (`fds`, `std_redirect`), the process
pointer, `state`, `on_cpu` and the scheduling timestamps are still `hw_task_t`'s; the
descriptors are the next move (they need `hw_fd_t` to become a portable type first,
which drags the pipe and socket references with it).

Gates: host test (`task_ident_tests.c`), `check-task-identity.py`, sabotage
`cases/core-task-identity.txt` (a second `pid`) and `core-task-identity-reset.txt`
(a reset that clears half) - both red.

## Two things found on the way

- `PTR_` in `linux_internal.h` did not initialise the two fields C4's futex descriptor
  added, so gcc reported `-Wmissing-field-initializers` on every row. The futex commit
  was reported green on a run whose head I had cut off with `tail` - the trap this
  project's notes describe. Fixed here; the clean run reads `warnings=0`.
- `check-subsystem.py`'s `mustbezero_exempt` went 21 to 22 with `sched/task_ident`
  (pure functions over a caller's struct; no state to be wrong). That baseline "may
  only go down", so it is recorded as a decision beside the number rather than edited.
