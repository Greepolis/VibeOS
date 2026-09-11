# C5, first step: threads are one process

C5 in `docs/core/phases.md` was "one owner for what a task is": move the task
table, keep only what a context switch needs in the arch layer. It was planned
after C4. It came first, and it came in a different shape, because five external
findings in two days turned out to be one defect in the structure C5 exists to
fix - and moving a structure with the wrong ownership model only relocates the
defect.

## The defect

`hw_sys_clone_thread` created a thread with

```c
child->proc = parent->proc;
```

which copies `hw_proc_t` **by value**. Everything in it that belongs to a
process became a private copy per thread. The address space survived that,
because what was copied was a pointer to the page tables and exit counts who
still holds them. Nothing else did:

| | reported as | what the copy did |
|---|---|---|
| mapping cursor | found while verifying the others | a thread's `mmap` never moved main's cursor, so main was handed the same base and its fresh pages silently replaced the thread's |
| signal dispositions | M-003 | `sigaction` in one thread was never seen by its siblings |
| exit_group | H-005 | shared a `case` with `exit` and ended only the calling thread |
| region list | H-004 | two list heads into one pool; a node one thread freed was still reachable from another's head, with its `next` overwritten by the free list |
| descriptors | already admitted in a comment | copied, not shared - not changed in this step |

The `sigaction` row is the one worth remembering. The copying loop in
`clone_thread` sat directly under the comment *"Signal dispositions are the
process's, so a thread inherits them."* The sentence described the intent and
the loop beneath it contradicted it.

The mapping cursor was not in any review. It came from asking, for each field in
`hw_proc_t`, "who writes this, and do the writers agree on which copy?" - the
answer for `mmap_cur` was two syscalls writing whichever copy the calling thread
happened to own.

## Why the existing tests were green

`musl_threads.c` created every thread from main, and no worker touched process
state. A kernel that gives each thread a private copy of the process passes that
test exactly, because each child copies an up-to-date value at the moment of its
creation and never changes it.

## Tests first, run against the unchanged kernel

Three stages were added to `tests/linux/musl_threads.c` and asserted by the boot
gate before any kernel code changed. Each was run against the old kernel and
failed for the reason it was written for:

```
THREADS_C5_MMAP_FAIL: main was given the thread's base 0x80040d2000
THREADS_C5_SIGACTION_FAIL: killed by signal 10
THREADS_C5_EXIT_GROUP_FAIL: exited 7, expected 42
```

On the same boot the three older stages stayed `OK`, which is the point.

Two things were checked before the stages were trusted to be able to fail at
all. SIGUSR1's default action here is to kill - `hw_signal_default_kills` - and a
default of "ignore" would have made the sigaction stage pass on the defect. And
the exit_group stage requires the code **42**, not merely that the process ended:
`waitpid` builds the status from the leader's `exit_code` and `exit_signal`, so an
exit_group implemented by SIGKILLing the siblings would report "killed by signal
9" for the leader. Requiring 42 means that implementation cannot pass.

The boot that established the red was guarded against the traps this project
already knows: the gate compiled, the build returned 0, and both the test binary
and the serial log were newer than the source. A stale `THREADS.ELF` prints no C5
lines at all and would have read as three `did_not_report` failures that proved
nothing.

## The change

**`hw_procstate_t`, referenced rather than copied.** It holds the program break,
the mapping cursor, the region list, the four disposition arrays and the
exit_group state. `hw_task_t` points at one. `fork` and `exec` create one, a
thread takes a reference, exit gives it back. The pool is sized for every task
plus one exec in flight per core, because exec builds the new process before it
lets go of the old one.

The last reference empties the region list **before** the slot is published
free, so a concurrent allocation never inherits a list that is still being
cleared. That ordering is safe because the only thing that raises a count is
clone, which runs on a live thread of the same process - and the holder of the
last reference is the thread that is exiting.

**The mapping cursor is claimed with a compare-exchange.** Sharing it turned a
deterministic defect into a race: two threads reading and advancing one cursor
at once can still get the same base. A lock around the mapping would have held
thousands of pages with the timer off, so the range is claimed first and mapped
afterwards, and nobody holds a lock across the mapping. A range whose mapping
fails stays a hole in address space.

**brk runs one at a time per process.** A thread shrinking the break while
another grows it would unmap pages the other has just mapped. Claimed with a
compare-exchange on a busy flag rather than a spinlock, for the same reason as
above. A C library already serialises its own brk calls, so it is almost never
contended; it has to be correct, not fast.

**exit_group** records the group's code, then SIGKILLs every sibling - which also
wakes one that is blocked - and exits. Delivery of that SIGKILL consults the
recorded code, so the leader reports 42 rather than "killed by 9". The first
caller's code wins, as on Linux. It does not hold `g_sched_lock` while raising,
because `hw_signal_raise` can take it through `hw_task_set_state` - the same
reason `kill()` already iterates without it.

**exec gets a new process.** Its dispositions are derived from the outgoing one -
caught signals revert to default, ignored stays ignored, masks carry over - and
they are derived **before** the reference to the outgoing process is given back.
Once the last one goes, the slot can be handed to an exec or a fork on another
core, and reading it afterwards could copy a stranger's dispositions.

## What was deliberately not done

- **exec in a multi-threaded process does not end the siblings.** Linux does;
  here the exec'ing task moves to a new process and its siblings keep running the
  old image on the old one. That is exactly what happened before this change, so
  it is no worse - and exec is the path that once produced the `cr2 == rip`
  defect, so it was changed as little as possible. It is the next step, and it
  can use `hw_task_exit_group` for the siblings.
- **A leader that exits while its threads run** becomes a zombie the parent can
  reap early. Linux waits for the whole group. Not changed, not tested.
- **Descriptors stay per thread.** The source already said so, and it is the next
  row of the same table.
- **fork is not atomic against its own process's other threads.** This was first
  written down as "fork reads the break without holding it - three lines", and
  the estimate was wrong in scope, found while writing those three lines. `fork`
  copies the pages (`hw_aspace_copy_user`) well before it clones the regions and
  copies the break, and no address-space lock covers that stretch - so a sibling
  running `mmap`, `munmap`, `mprotect` or `brk` during a fork can leave the child
  with pages and regions that disagree, not just a stale break. A guard on brk
  alone would be a partial fix presented as a whole one. Linux holds one
  address-space lock across fork and all of those calls; that is the shape of the
  fix, and it is larger than a line.
- **exit_group cannot end a sibling that is blocked in the kernel.** Found while
  preparing the next step, after this change was already green. SIGKILL is
  delivered on the way back to ring 3, and **no blocking wait in this kernel reads
  `sig_pending`**: `hw_futex_wait` loops on `while (!woken) hlt`, and the pipe and
  waitpid waits have the same shape. `hw_signal_raise` moves a blocked task to
  READY, the scheduler runs it, and it goes straight back into the loop.
  So exit_group ends a sibling running in user space, and one blocked in
  `pthread_join` on the exiting thread - that one is woken by the exiting thread's
  own clear_child_tid - but not one waiting on an unrelated condition variable, a
  pipe or a child. The process then never ends. That is not a regression - the
  sibling stayed blocked before as well - and it is not about threads: `kill -9`
  cannot end a process blocked on a pipe today either. It is the prerequisite for
  finishing exit_group and for exec ending its siblings, so it goes first.

  There are **four** such waits, not three. Console `read()` sets `wait_input`,
  and `hw_signal_raise` clears it - but the only code that reads the flag is the
  keyboard's wakeup, and the read loop itself just tries the keyboard again and
  re-blocks. `scripts/dev/cases/ctrl-c.txt` already records why nobody saw this:
  the Ctrl-C delivery case was written, run, and came back **not red**, because by
  the time the kernel CLI runs there is no foreground process left to reach.
  Delivery of a signal to a process blocked in the kernel has never been
  observed by any test here.

  And the test that would observe it cannot be written with the obvious bound:
  `case LSYS_wait4` calls `hw_sys_waitpid(a1, a2)` and the options argument never
  arrives, so `WNOHANG` is ignored and a "bounded" poll blocks exactly like an
  unbounded wait. A shell reaping background jobs depends on that option too, so
  it is a defect in its own right, and it goes first of all.

  **Closed, with tkill, in the next commit.** WNOHANG is honoured; WUNTRACED,
  WCONTINUED and Linux's `__W` bits are accepted and have no effect - refusing
  them would break BusyBox's shell - and unknown bits are refused.
  `hw_task_by_tid` had been `return hw_task_by_pid(tid);` under a comment saying
  a thread id and a process id were the same number, which stopped being true
  when `clone(CLONE_THREAD)` gave threads their own ids: a non-leader's id matched
  nothing and tkill answered ESRCH, and `raise()` - which a C library implements
  as `tkill(gettid())` - failed in every thread but the first. Both had red
  stages first (`waited for the child to exit`, `pthread_kill on a live thread
  returned 3`); both green on the first build; twelve boots: 11 pass, 1 fail.

  **And the four waits, closed after that.** Every blocking wait asks
  `hw_signal_interrupts` now. Three decisions carry it, each forced by something
  read rather than assumed. `hw_task_set_state` takes no lock and
  `hw_signal_raise` wakes a task without `g_sched_lock`, so each wait sets BLOCKED
  **first** and asks second - the other order has an interleaving in which the
  signal lands between the question and the block, raise sees a running task and
  wakes nothing, and the task parks with the signal pending. `futex_wait` decides
  "woken or interrupted" under `g_futex_lock`, or a concurrent FUTEX_WAKE would
  be counted as delivered to a waiter that returned EINTR and the thread it was
  meant for would never be told. And a signal that would be discarded - ignored,
  or a default that ignores - interrupts nothing, because the native shell
  ignores wait4's result and a spurious EINTR would stop it waiting for its
  child; SIGCHLD is never raised today, which is exactly why that cannot be left
  to chance. Red first (`THREADS_C5_EXIT_GROUP_BLOCKED_FAIL: the process never ended`), green after; twelve boots: 11 pass, 1 fail.
- **No must-be-zero counter for the new structure.** One was designed - a
  reference dropped below zero - and not added, because a counter nothing reads is
  this project's most repeated defect, and reading it means touching the portable
  stats struct, its printer and the gate in the same change. A leaked process
  would show today through `vmas_live`, which the gate already asserts returns to
  zero. Recorded as debt, not as coverage.

## Found on the way

Rewriting `hw_sys_mmap` for the claimed cursor exposed that this morning's fix
for the review's M-002 had covered **one of two** allocation loops - the
reservation branch - and missed the ordinary anonymous path that `malloc`
reaches. The note in that commit saying the leftover pages were unreachable from
ring 3 was true only of the loop it fixed. Both loops unwind now, and
`mm_brk_mmap_leaks.md` carries the correction.

## Verification

| | |
|---|---|
| first build after the change | all three C5 stages `OK`, `reason=cli_and_network_verified` |
| `check.sh all build-clang-Release` | `VERDICT=green` - rc 0, warnings 0, clang 0/0, host tests pass, every check ok, 3/3 boots |
| twelve-boot series, evidence kept outside `/tmp` | **12 pass, 0 fail**; TLB quarantine overflow 0 on every boot |

The three older `THREADS` stages stayed `OK` throughout, as they did on the red
run - they were never able to see this.
