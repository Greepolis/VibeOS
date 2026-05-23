# Kernel Core Progress

Status: In Progress (boot-to-CLI baseline verified)
Last review: 2026-05-23

## Implemented
- Boot stage orchestration in `kernel/core/kmain.c`.
- Early bring-up sequence for core subsystems (PMM/VM/scheduler/IPC hooks).
- Boot event signaling and serial boot markers (`BOOT_OK`) for smoke validation.
- Boot-to-CLI baseline: after `BOOT_OK`, kernel emits `CLI_READY`, starts the serial prompt `vibeos>`, and handles `help`, `status`, `echo <text>`, `halt`, and `reboot` as bootstrap commands.
- Kernel object containers wired in `include/vibeos/kernel.h`.
- Security/policy subsystem initialization path integrated in core startup.
- Host-safe x86_64 serial backend behavior: privileged port I/O is now gated by CPL detection to avoid user-mode `SIGILL` in CI host tests while preserving ring0 boot logging.
- Stage health model in `kernel/core/kmain.c` with explicit boot-health bitset and fatal-failure classification (`vibeos_kernel_boot_health`).

## Pending
- Stronger failure-mode recovery between bring-up phases.
- More explicit stage rollback behavior for partial init failures.
- Deeper boot-time configuration profile support (debug/perf/hardened) across runtime tunables.

## Next checkpoint
- Move the bootstrap CLI behind a cleaner console abstraction and prepare the transition toward userland init/shell.
