# User Space Interface Progress

Status: In Progress (kernel CLI bootstrap verified)
Last review: 2026-05-23

## Implemented
- User API syscall bridge in `user/lib/user_api.c`.
- Service-facing interface contracts in `include/vibeos/user_api.h`.
- Compatibility runtime scaffolding under `user/compat/*`.
- Baseline integration with process/security/syscall surfaces.
- Versioned user API contract and capability matrix exposure (`vibeos_user_api_contract`, `vibeos_user_api_capabilities`).
- Interim boot interaction path verified through kernel-space serial CLI (`vibeos>`) while full userland shell/runtime remains pending.

## Pending
- Stable libc/runtime contract for richer userland.
- Expanded compatibility ABI translation depth (Linux/Windows/macOS targets).
- Tooling and diagnostics for userland API evolution.

## Next checkpoint
- Promote the verified kernel CLI path into a userland shell once init/service startup is ready.
