# System Call Interface Progress

Status: In Progress (real ring-3 syscall entry verified in QEMU)
Last review: 2026-07-22

## Implemented
- **Real on-metal ring-3 -> ring-0 syscall entry** (`kernel/arch/x86_64/arch_hw.c` + `isr.S`): user GDT segments (DPL 3), a TSS with RSP0, an `int 0x80` gate at DPL 3, and a minimal handler (`write`/`exit`). Verified in QEMU: the kernel drops to ring 3, a user program issues `int 0x80` syscalls (the kernel prints its `write` payload), and `exit` returns control to the kernel via a setjmp/longjmp-style resume. This is the hardware entry path; it will be connected to the portable dispatcher below.
- Central syscall dispatcher in `kernel/core/syscall.c` (portable model).
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
