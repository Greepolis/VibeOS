# Init System Progress

Status: In Progress (dependency graph and restart policy host-verified)
Last review: 2026-08-28

## Implemented
- Init service bootstrap stub in `user/init/init_system.c`.
- Service startup orchestration integrated with service manager.
- Start/stop lifecycle controls for core user-space services.
- Dependency-aware init graph execution (`vibeos_init_graph_start`) with explicit unresolved-node reporting.
- Restart-class policy controls for core vs optional services (`vibeos_init_restart_policy`, `vibeos_init_restart_note`, `vibeos_init_restart_allowed`).
- Dependency resolution now evaluates only enabled nodes; disabled optional services no longer make an otherwise valid startup graph fail. Regression coverage verifies the enabled/failed counts.
- Service-manager startup now commits the manager to `RUNNING` only after all dependencies start; each failure path rolls back already-started services and resets the supervised count.
- Native service manifest and runtime snapshot contracts are defined in `include/vibeos/userland.h` and validated host-side.
- `user/servicemgr/supervisor.c` provides a bounded runtime supervisor model with dependency ordering, `STARTING`/`RUNNING`/`FAILED` states, restart limits and exponential backoff. The model is covered by the kernel host regression suite.
- A separate freestanding ring-3 native init candidate (`user/prog/init.c`) is built with the target user ABI and staged as `EFI/BOOT/NATIVE_INIT.ELF`; it starts the shell child and waits for its exit. It is not yet selected as runtime PID 1.
- Media generation now exposes an explicit `VIBEOS_USE_NATIVE_INIT=ON` switch to select that candidate as `INIT.ELF`; the default remains the legacy bring-up workload until the native-init QEMU acceptance gate is green.
- Supervisor startup now fails closed when dependency resolution stops before all manifest nodes are running, including cyclic dependency graphs; host regression coverage verifies the bounded failure.
- Supervisor lifecycle now distinguishes clean `STOPPED` exits from `FAILED` faults, applies restart policy accordingly and marks a service failed only after its bounded restart limit is exhausted.
- Supervisor runtime snapshots now support explicit service-to-PID binding, reverse lookup and unbind with collision rejection; this is host-verified groundwork for process-backed init supervision.
- Exit events can now be reported by PID (`vibeos_service_supervisor_report_exit_pid`), atomically unbinding the process and applying service restart policy. Supervisor clock ticks remain successful while exposing terminal `FAILED` state through health snapshots.
- The x86_64 bring-up now initializes a bounded native supervisor manifest and binds the initial runtime task to the init service PID; task exit reports are forwarded through the PID-based API. Automatic process restart and QEMU acceptance remain pending.
- Supervisor adapters may now provide bounded spawn/stop hooks. After a fault, the replacement PID is bound only after the spawn hook succeeds, then the service returns to `RUNNING`; host tests cover the complete PID replacement transition.

## Pending
- The runtime boot path still starts fixed user ELF programs; it is not yet a supervised init process.
- Declarative/service-config source of truth for init graph nodes (currently call-site supplied).
- Runtime process spawn/exit supervision and stronger failure domains (the current supervisor is a host-tested model, not yet connected to ring 3).
- Declarative init configuration format and validation.
- Runtime health probes and process-backed failure-domain isolation remain pending.

## Next checkpoint
- Replace fixed runtime program selection with ring-3 init execution and service supervision.
