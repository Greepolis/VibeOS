# Init System Progress

Status: In Progress (native ring-3 init is PID 1 and supervises a child, gated; service supervision is still a host-tested model)
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
- **The native ring-3 init is PID 1.** `user/prog/init.c` is what the kernel
  loads from the boot volume as `EFI/BOOT/INIT.ELF`; it forks, execs the
  bring-up workload as a child (`EFI/BOOT/SELFTEST.ELF`), reports the pid it is
  now responsible for, and waits for it. Gated on `NATIVE_INIT_READY` and on
  `NATIVE_INIT_CHILD_PID=` - the second because an init that exec'd the
  workload in place would produce an otherwise identical boot, so without it
  the gate would prove only that init ran and nothing about what it is for.

  The workload is supervised rather than replaced on purpose: it *was* PID 1,
  which is why there had been no process left over to supervise anything.
  Making init its parent moved the boundary without trading away any of the
  coverage that already existed.
- Media generation now exposes an explicit `VIBEOS_USE_NATIVE_INIT=ON` switch to select that candidate as `INIT.ELF`; the default remains the legacy bring-up workload until the native-init QEMU acceptance gate is green.
- Supervisor startup now fails closed when dependency resolution stops before all manifest nodes are running, including cyclic dependency graphs; host regression coverage verifies the bounded failure.
- Supervisor lifecycle now distinguishes clean `STOPPED` exits from `FAILED` faults, applies restart policy accordingly and marks a service failed only after its bounded restart limit is exhausted.
- Supervisor runtime snapshots now support explicit service-to-PID binding, reverse lookup and unbind with collision rejection; this is host-verified groundwork for process-backed init supervision.
- Exit events can now be reported by PID (`vibeos_service_supervisor_report_exit_pid`), atomically unbinding the process and applying service restart policy. Supervisor clock ticks remain successful while exposing terminal `FAILED` state through health snapshots.
- The x86_64 bring-up now initializes a bounded native supervisor manifest and binds the initial runtime task to the init service PID; task exit reports are forwarded through the PID-based API. Automatic process restart and QEMU acceptance remain pending.
- Supervisor adapters may now provide bounded spawn/stop hooks. After a fault, the replacement PID is bound only after the spawn hook succeeds, then the service returns to `RUNNING`; host tests cover the complete PID replacement transition.

## Pending
- The runtime boot path starts one fixed program - init - which then starts everything else. What init supervises is still a single fixed child rather than a manifest: services, dependencies and restart policy remain the host-tested model described above, not something the guest runs.
- Declarative/service-config source of truth for init graph nodes (currently call-site supplied).
- Runtime process spawn/exit supervision and stronger failure domains (the current supervisor is a host-tested model, not yet connected to ring 3).
- Declarative init configuration format and validation.
- Runtime health probes and process-backed failure-domain isolation remain pending.

## Next checkpoint
- Replace fixed runtime program selection with ring-3 init execution and service supervision.
