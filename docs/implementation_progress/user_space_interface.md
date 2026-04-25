# User Space Interface Progress

Status: In Progress
Last review: 2026-04-25

## Implemented
- User API syscall bridge in `user/lib/user_api.c`.
- Service-facing interface contracts in `include/vibeos/user_api.h`.
- Compatibility runtime scaffolding under `user/compat/*`.
- Baseline integration with process/security/syscall surfaces.

## Pending
- Stable libc/runtime contract for richer userland.
- Expanded compatibility ABI translation depth (Linux/Windows/macOS targets).
- Tooling and diagnostics for userland API evolution.

## Next checkpoint
- Publish versioned user API contract and compatibility runtime capability matrix.
