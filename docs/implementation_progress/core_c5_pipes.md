# C5 - pipes as a module

Status, 2026-09-21: **done.** This is the detector `core_c5_task_identity.md` said the
descriptor work was waiting for.

## What it was

`hw_pipe_t`, `g_pipes[]` and `g_pipe_lock` were globals of the architecture layer,
touched from twelve places in the Linux ABI: read, write, pipe creation and its two
rollbacks, dup2 (twice), fork, clone, close and exit each adjusted the `readers` and
`writers` counts or the ring by hand. That is the classic hung-pipeline defect - a
count that is wrong on one path - with no place to check it.

## What it is now

`include/vibeos/pipe.h`, `kernel/ipc/pipe.c`: portable, no globals outside the module.

- **One attempt, then the caller decides.** `vibeos_pipe_read` / `_write` move what
  they can and return a status - `OK`, `EMPTY`, `EOF`, `FULL`, `NO_READER`, `FAULT` -
  and never sleep, raise a signal or touch user memory. The waiting, `SIGPIPE` and
  the wake-ups stay in `fs.c`, which knows about the scheduler. The copy to and from
  user memory is a function the caller passes (`vibeos_uaccess_copy` has the shape).
- **Serialised by its own lock**, registered by the architecture
  (`vibeos_pipe_set_lock`; a registration, not a weak symbol - that does not resolve
  on the Windows build). The fourth layer in this project that needed one, and this
  time it was written in from the start.
- **Ends are acquired and released through the module**: `vibeos_pipe_end_acquire` and
  `_end_release` (which detaches the descriptor, so it cannot release twice). dup2,
  fork/clone and close use them; the three copies of "increment the right count under
  the lock" are gone.
- **Two must-be-zero counters, gated.** `pipe_end_underflow`: an end released with
  none held - the old code clamped it at zero and carried on, which is exactly how a
  wrong count stays invisible until a pipeline hangs. `pipe_bad_slot`: a descriptor
  named a pipe that is free or does not exist. The boot gate already asserts the
  registry total is zero, so both are gated from the first boot; the host test drives
  each on purpose (`test_mbz_all_demonstrated` requires that).

## What the first boots said

All three boots read zero for both counters: no double release and no dangling slot
anywhere in the boot script, including the pipelines, fork and the threads test. That
is a measurement, not an assumption - the clamp used to make it unknowable.

## Gates

Host test `pipe_tests.c` (about 50 checks: the ring across its wrap, EOF versus EMPTY,
a drain after the last writer closes, a fault that consumes nothing, a duplicated end
needing a second release, the module lock taken and never nested), and
`cases/ipc-pipe.txt`: end of file without asking about writers, a pipe freed when
either end goes, an underflow clamped instead of counted, a write accepted with no
reader - each red on a named check. `ipc/pipe` is the first module of this phase with a
real must-be-zero, so `mustbezero_exempt` stays at 23 and is not raised.

## Not done

`hw_fds_inherit` now acquires each pipe end separately, where the old code held the
pipe lock across all of them; another core cannot observe a half-inherited table
because the child is not runnable yet, but the property is argued, not tested.
