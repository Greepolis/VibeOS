# User Space Interface Progress

Status: In Progress (ring-3 shell runtime verified)
Last review: 2026-08-01

## Implemented
- User API syscall bridge in `user/lib/user_api.c`.
- Service-facing interface contracts in `include/vibeos/user_api.h`.
- Compatibility runtime scaffolding under `user/compat/*`.
- Baseline integration with process/security/syscall surfaces.
- Versioned user API contract and capability matrix exposure (`vibeos_user_api_contract`, `vibeos_user_api_capabilities`).
- Real ELF programs now run in ring 3 through the native syscall entry path. The runtime loads programs from the FAT boot volume and supports fork, `execve`, `waitpid` and exit.
- `/BIN/SH` supplies the current interactive shell baseline with keyboard/serial input, line editing and file commands; framebuffer output is available in the runtime console path.

## Pending
- Stable libc/runtime contract for richer userland.
- Expanded compatibility ABI translation depth (Linux/Windows/macOS targets).
- Tooling and diagnostics for userland API evolution.
- A stable libc/runtime boundary, executable format policy and user-space memory/error isolation.

## Next checkpoint
- Replace the fixed boot program selection with a supervised ring-3 init process and a stable userland runtime contract.
