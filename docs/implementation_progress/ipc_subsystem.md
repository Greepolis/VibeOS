# IPC Subsystem Progress

Status: In Progress
Last review: 2026-04-25

## Implemented
- Event and bounded channel primitives in `kernel/ipc/event.c` and `kernel/ipc/channel.c`.
- Handle-transfer path with rights reduction in `kernel/ipc/handle_transfer.c`.
- Waitset core plus ownership enforcement in `kernel/ipc/waitset.c`.
- Multiple wake policies (`FIFO`, `REVERSE`, `ROUND_ROBIN`, `PRIORITY`).
- Per-event metadata controls (priority and enable/disable).
- Batch wait, wait-all, and signaled peek helpers.
- Extended telemetry for wait behavior (disabled skips, priority wakes, wait-all, batch, peek).
- Scheduler-aware thread wait wrappers with wake CPU feedback.

## Pending
- Large fan-in waitset sharding strategy.
- Fairness proofs under mixed policy workloads.
- Stronger IPC load/perf benchmarks with reproducible profiles.

## Next checkpoint
- Add large-scale contention test suite for waitset policy fairness and throughput.
