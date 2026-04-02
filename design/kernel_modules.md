# Kernel Modules and Boundaries

## Kernel core modules

- `kernel/core/` kernel entry, init sequencing, panic handling
- `kernel/arch/` architecture-specific traps, context switch, tables
- `kernel/mm/` physical and virtual memory primitives
- `kernel/sched/` scheduler, per-CPU state, timers
- `kernel/ipc/` channels, shared memory, signaling
- `kernel/object/` handles, rights, object lifecycle
- `kernel/proc/` processes, threads, jobs, exceptions
- `kernel/time/` clocks and timers
- `kernel/platform/` SMP, ACPI or firmware integration, device discovery primitives

## User-space system modules

- `user/init/`
- `user/servicemgr/`
- `user/devmgr/`
- `user/fs/`
- `user/net/`
- `user/sec/`
- `user/compat/linux/`
- `user/compat/windows/`
- `user/compat/macos/`
- `user/drivers/`
- `user/shell/`
- `user/lib/`

### Control-plane responsibilities

- `user/init/` performs the earliest trusted userspace bootstrap
- `user/servicemgr/` owns long-lived supervision, dependency ordering, and restart policy
- `user/devmgr/` owns device discovery, policy, and driver host orchestration

## Boundary rules

- kernel core exposes versioned internal interfaces, not ad hoc cross-module coupling
- user-space services communicate through IPC contracts
- compatibility layers cannot directly depend on unstable kernel internals
- privileged modules require explicit justification and review
