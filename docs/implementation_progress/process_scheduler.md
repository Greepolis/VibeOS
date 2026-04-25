# Process Scheduler Progress

Status: In Progress
Last review: 2026-04-25

## Implemented
- Per-CPU runqueues and core enqueue/dequeue primitives in `kernel/sched/scheduler.c`.
- Preemption and wait telemetry counters (timeouts/wakes/preemptions).
- Thread runtime tracking (`RUNNABLE/RUNNING/BLOCKED`) and wait transition accounting.
- Affinity masks and thread nice-level controls with validation APIs.
- Rebalance pass support with bounded migration budget.
- Starvation tick and starvation-boost utilities for runnable threads.
- QoS summary metrics (rebalance passes/moves, affinity misses, boosts).

## Pending
- Topology/NUMA-aware placement strategy.
- Better priority/wake fairness policy under heavy contention.
- Runqueue lock/atomic model definition for true parallel execution path.

## Next checkpoint
- Introduce topology-aware balancing policy and benchmark hooks for scheduler regressions.
