# Init System Progress

Status: In Progress
Last review: 2026-05-09

## Implemented
- Init service bootstrap stub in `user/init/init_system.c`.
- Service startup orchestration integrated with service manager.
- Start/stop lifecycle controls for core user-space services.
- Dependency-aware init graph execution (`vibeos_init_graph_start`) with explicit unresolved-node reporting.
- Restart-class policy controls for core vs optional services (`vibeos_init_restart_policy`, `vibeos_init_restart_note`, `vibeos_init_restart_allowed`).

## Pending
- Declarative/service-config source of truth for init graph nodes (currently call-site supplied).
- Restart/backoff policies with stronger failure domains.
- Declarative init configuration format and validation.

## Next checkpoint
- Add dependency-aware init graph execution with restart class policies.
