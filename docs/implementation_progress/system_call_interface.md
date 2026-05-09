# System Call Interface Progress

Status: In Progress
Last review: 2026-05-09

## Implemented
- Central syscall dispatcher in `kernel/core/syscall.c`.
- ABI helper mappings in `include/vibeos/syscall_abi.h`.
- Rights/policy lookup layer in `kernel/core/syscall_policy.c`.
- Process/thread lifecycle syscall family.
- Scheduler observability and runtime-control syscall family.
- Waitset ownership/stats/wake-policy syscall paths.
- Security/audit/policy control syscalls with kernel-vs-user authorization checks.
- ABI version/compatibility syscalls (`VIBEOS_SYSCALL_ABI_VERSION_GET`, `VIBEOS_SYSCALL_ABI_COMPAT_CHECK`) with compatibility helpers in `syscall_abi.h`.

## Pending
- Stronger multi-tenant authorization model beyond relation-based checks.
- Syscall ABI versioning strategy with compatibility negotiation.
- Per-syscall latency telemetry and regression thresholds.

## Next checkpoint
- Extend ABI negotiation from major-version compatibility to per-feature negotiation for optional syscall families.
