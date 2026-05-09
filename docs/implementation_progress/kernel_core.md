# Kernel Core Progress

Status: In Progress
Last review: 2026-05-09

## Implemented
- Boot stage orchestration in `kernel/core/kmain.c`.
- Early bring-up sequence for core subsystems (PMM/VM/scheduler/IPC hooks).
- Boot event signaling and serial boot markers (`BOOT_OK`) for smoke validation.
- Kernel object containers wired in `include/vibeos/kernel.h`.
- Security/policy subsystem initialization path integrated in core startup.
- Host-safe x86_64 serial backend behavior: privileged port I/O is now gated by CPL detection to avoid user-mode `SIGILL` in CI host tests while preserving ring0 boot logging.
- Stage health model in `kernel/core/kmain.c` with explicit boot-health bitset and fatal-failure classification (`vibeos_kernel_boot_health`).

## Pending
- Stronger failure-mode recovery between bring-up phases.
- More explicit stage rollback behavior for partial init failures.
- Deeper boot-time configuration profile support (debug/perf/hardened) across runtime tunables.

## Next checkpoint
- Add stage-by-stage health assertions and fatal/non-fatal classification in `kmain`.
