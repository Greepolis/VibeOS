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

## Pending
- The runtime boot path still starts fixed user ELF programs; it is not yet a supervised init process.
- Declarative/service-config source of truth for init graph nodes (currently call-site supplied).
- Runtime process spawn/exit supervision and stronger failure domains (the current supervisor is a host-tested model, not yet connected to ring 3).
- Declarative init configuration format and validation.
- Runtime health probes and process-backed failure-domain isolation remain pending.

## Next checkpoint
- Replace fixed runtime program selection with ring-3 init execution and service supervision.
