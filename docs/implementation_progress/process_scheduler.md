# Process Scheduler Progress

Status: In Progress (preemptive SMP runtime verified)
Last review: 2026-08-28

## Implemented
- **Real on-metal preemptive round-robin scheduler** (`kernel/arch/x86_64/arch_hw.c` + `isr.S`): the timer IRQ (IRQ0) saves the interrupted task and restores the next by rewriting the live interrupt frame; `sched_start` launches the first task and `ring3_resume` returns to the kernel when the demo's bounded switch budget is reached. Verified in QEMU (GCC and Clang): two ring-3 user tasks (loaded from the embedded `task` ELF, distinct ids/stacks) interleave A/B under timer preemption (`SCHED_OK`). This is the hardware mechanism; it will be driven by the portable scheduler model below and gain per-process address spaces.
- Per-CPU runqueues and core enqueue/dequeue primitives in `kernel/sched/scheduler.c` (portable model).
- Preemption and wait telemetry counters (timeouts/wakes/preemptions).
- Thread runtime tracking (`RUNNABLE/RUNNING/BLOCKED`) and wait transition accounting.
- Affinity masks and thread nice-level controls with validation APIs.
- Rebalance pass support with bounded migration budget.
- Starvation tick and starvation-boost utilities for runnable threads.
- QoS summary metrics (rebalance passes/moves, affinity misses, boosts).
- CPU load snapshot API (`vibeos_sched_load_snapshot`) for deterministic benchmark/telemetry hooks.
- Local APIC and AP startup support bring additional CPUs online; each core enters the runtime scheduling path in QEMU.
- Runtime task lifecycle includes per-task kernel stacks, fork, `execve`, blocking `waitpid`, exit handling and address-space reclamation.
- **`fork` is copy-on-write.** The child gets the parent's mappings rather than copies of its pages, which matters here specifically because a shell forks for every external command and the `execve` immediately after discards the copy - two megabytes of it for a program the size of BusyBox.
- **Per-task signal state.** Pending and blocked masks, per-signal handler, flags and restorer, all inherited across `fork` except the pending set, which is not. Raising a signal against a task blocked in `read()` moves it back to READY so it can notice; delivery itself happens on the way back to ring 3.
- **Exit status carries a signal.** A task killed by a signal records it, and `waitpid` encodes the exit code in the high byte and the signal in the low seven bits. `128 + sig` is what a shell prints, not what the kernel stores.
- Descriptors are part of task lifecycle: inherited on `fork` with pipe endpoint counts adjusted, and released on exit, so a pipeline's reader sees end-of-file when the last writer dies.
- The portable process table now tracks process-group ID, session ID and session leader. Host APIs validate same-session group assignment and leader-only session creation (`vibeos_proc_set_process_group`, `vibeos_proc_create_session`, `vibeos_proc_get_session`).
- The x86_64 console now broadcasts `SIGINT` to every live member of the foreground process group, with a bounded newest-task fallback during early boot. Runtime task-group reassignment and stopped-job semantics are still pending.
- The x86_64 Linux personality now exposes `setpgid`, `getpgrp`, `setsid` and `getsid` with same-session and group-leader checks. The APIs are syntax-checked in the freestanding build; QEMU syscall/job-control coverage is still pending.
- `SIGSTOP` now marks a task stopped and unschedulable, while `SIGCONT` clears that state and wakes it. The transition is implemented in the x86_64 signal path; user-visible stop/continue regression coverage still requires the QEMU gate.
- The shell now places external foreground jobs in their own process group and uses `TIOCSPGRP` to transfer terminal ownership while waiting. Kernel-side session validation is implemented; interactive QEMU coverage and background jobs remain pending.
- `kill(0, sig)` and `kill(-pgid, sig)` now address the caller's group or an explicit process group, restricted to the caller's session. This is syntax-checked but still lacks an interactive QEMU regression.
- The portable lifecycle reparents children of a terminating process to PID 1, or clears the parent if PID 1 itself exits; handle and thread cleanup remains part of the same termination transaction.

## Pending
- Process groups and sessions. `Ctrl-C` currently raises `SIGINT` against the newest live user task because there is no job to target.
- Hardware-task propagation of process groups/sessions and foreground terminal signal targeting.
- Job control. There is no stopped task state: `SIGSTOP` cannot be caught or blocked, but its default action ends the task rather than suspending it, and `SIGCONT` is ignored.
- Threads. `clone` with `CLONE_VM` or `CLONE_THREAD` returns `-ENOSYS`, and `gettid` reports the pid because there is exactly one thread per process.
- Topology/NUMA-aware placement strategy.
- Better priority/wake fairness policy under heavy contention.
- Runqueue lock/atomic model definition for true parallel execution path.
- Remove the current bounded demonstration scheduler assumptions and bind all runtime scheduling decisions to the portable runqueue policy.

## Next checkpoint
- Define the runqueue locking/atomic contract for concurrent AP scheduling, then add contention and fairness regression coverage.
