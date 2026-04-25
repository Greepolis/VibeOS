# System Call Interface Progress

Status: In Progress
Last review: 2026-04-25

## Implemented
- Central syscall dispatcher in `kernel/core/syscall.c`.
- ABI helper mappings in `include/vibeos/syscall_abi.h`.
- Rights/policy lookup layer in `kernel/core/syscall_policy.c`.
- Process/thread lifecycle syscall family.
- Scheduler observability and runtime-control syscall family.
- Waitset ownership/stats/wake-policy syscall paths.
- Security/audit/policy control syscalls with kernel-vs-user authorization checks.

## Pending
- Stronger multi-tenant authorization model beyond relation-based checks.
- Syscall ABI versioning strategy with compatibility negotiation.
- Per-syscall latency telemetry and regression thresholds.

## Next checkpoint
- Introduce ABI version tag + compatibility guards and add syscall conformance tests.
