# Process Scheduler Progress

Status: In Progress (real preemptive multitasking verified in QEMU)
Last review: 2026-07-22

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

## Pending
- Topology/NUMA-aware placement strategy.
- Better priority/wake fairness policy under heavy contention.
- Runqueue lock/atomic model definition for true parallel execution path.

## Next checkpoint
- Introduce topology-aware balancing policy and benchmark hooks for scheduler regressions.
