# Kernel Core Progress

Status: In Progress (boot-to-CLI + kernel log baseline verified)
Last review: 2026-06-01

## Implemented
- Boot stage orchestration in `kernel/core/kmain.c`.
- Early bring-up sequence for core subsystems (PMM/VM/scheduler/IPC hooks).
- Boot event signaling and serial boot markers (`BOOT_OK`) for smoke validation.
- Boot-to-CLI baseline: after `BOOT_OK`, kernel emits `CLI_READY`, starts the serial prompt `vibeos>`, and handles `help`, `status`, `echo <text>`, `halt`, and `reboot` as bootstrap commands.
- Kernel object containers wired in `include/vibeos/kernel.h`.
- Security/policy subsystem initialization path integrated in core startup.
- Host-safe x86_64 serial backend behavior: privileged port I/O is now gated by CPL detection to avoid user-mode `SIGILL` in CI host tests while preserving ring0 boot logging.
- Stage health model in `kernel/core/kmain.c` with explicit boot-health bitset and fatal-failure classification (`vibeos_kernel_boot_health`).
- Kernel observability baseline: fixed-size in-memory ring log (`vibeos_log_t`) records boot milestones and fatal boot failures without dynamic allocation.
- Bootstrap CLI exposes `log` and extends `status` with log counters for QEMU/serial diagnostics.

## Pending
- Stronger failure-mode recovery between bring-up phases.
- Panic/page-fault integration with the kernel log and trap state.
- More explicit stage rollback behavior for partial init failures.
- Deeper boot-time configuration profile support (debug/perf/hardened) across runtime tunables.

## Next checkpoint
- Wire trap/page-fault reporting into the kernel log, then move the bootstrap CLI behind a cleaner console abstraction and prepare the transition toward userland init/shell.
