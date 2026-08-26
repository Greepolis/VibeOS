# IPC Subsystem Progress

Status: In Progress
Last review: 2026-08-26

## Implemented
- Event and bounded channel primitives in `kernel/ipc/event.c` and `kernel/ipc/channel.c`.
- Handle-transfer path with rights reduction in `kernel/ipc/handle_transfer.c`.
- Waitset core plus ownership enforcement in `kernel/ipc/waitset.c`.
- Multiple wake policies (`FIFO`, `REVERSE`, `ROUND_ROBIN`, `PRIORITY`).
- Per-event metadata controls (priority and enable/disable).
- Batch wait, wait-all, and signaled peek helpers.
- Extended telemetry for wait behavior (disabled skips, priority wakes, wait-all, batch, peek).
- Scheduler-aware thread wait wrappers with wake CPU feedback.
- Contention snapshot API (`vibeos_waitset_contention_snapshot`) with enabled/signaled visibility for load diagnostics.
- Handle transfer rejects invalid source handles before allocating anything; valid generic handles remain transferable with reduced rights. Host regression covers invalid-source rejection.
- Maximum fan-in waitset coverage exercises all 32 event slots, contention snapshots and batch wake selection under round-robin policy.

## Pending
- Large fan-in waitset sharding strategy beyond the current bounded 32-slot primitive.
- Fairness proofs under mixed policy workloads.
- Stronger IPC load/perf benchmarks with reproducible profiles.

## Next checkpoint
- Add reproducible throughput benchmarks and a sharding design for waitsets larger than 32 events.
