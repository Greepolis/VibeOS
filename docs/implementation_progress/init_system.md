# Init System Progress

Status: In Progress (dependency graph and restart policy host-verified)
Last review: 2026-08-26

## Implemented
- Init service bootstrap stub in `user/init/init_system.c`.
- Service startup orchestration integrated with service manager.
- Start/stop lifecycle controls for core user-space services.
- Dependency-aware init graph execution (`vibeos_init_graph_start`) with explicit unresolved-node reporting.
- Restart-class policy controls for core vs optional services (`vibeos_init_restart_policy`, `vibeos_init_restart_note`, `vibeos_init_restart_allowed`).
- Dependency resolution now evaluates only enabled nodes; disabled optional services no longer make an otherwise valid startup graph fail. Regression coverage verifies the enabled/failed counts.
- Service-manager startup now commits the manager to `RUNNING` only after all dependencies start; each failure path rolls back already-started services and resets the supervised count.

## Pending
- The runtime boot path still starts fixed user ELF programs; it is not yet a supervised init process.
- Declarative/service-config source of truth for init graph nodes (currently call-site supplied).
- Restart/backoff policies with stronger failure domains.
- Declarative init configuration format and validation.
- Runtime supervision, health probes and failure-domain isolation remain pending.

## Next checkpoint
- Replace fixed runtime program selection with ring-3 init execution and service supervision.
