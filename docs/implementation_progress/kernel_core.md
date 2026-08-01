# Kernel Core Progress

Status: In Progress (ring-3 runtime and shell baseline verified)
Last review: 2026-08-01

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
- Foundation fault-handling model: `vibeos_kernel_dispatch_trap` turns user-mode faults into non-fatal current-process termination and kernel-mode faults into fatal panic state, both recorded in the kernel ring log.
- x86_64 runtime bring-up now owns paging, GDT/TSS, IDT, timer and syscall entry before starting ring-3 work in `kernel/arch/x86_64/arch_hw.c`.
- A real user ELF can be loaded from the boot volume, forked/replaced, waited for and returned to the kernel without relying on the original kernel-space CLI path.
- The runtime provides framebuffer and serial console paths; `/BIN/SH` supplies the interactive shell baseline.

## Pending
- Stronger failure-mode recovery between bring-up phases.
- Runtime hardware page-fault entry wiring from the x86_64 IDT/assembly path into the kernel trap decision API.
- More explicit stage rollback behavior for partial init failures.
- Deeper boot-time configuration profile support (debug/perf/hardened) across runtime tunables.
- A supervised, persistent user-space init/service-manager handoff; the current boot flow launches fixed demonstration and shell programs.
- Crash-dump persistence, recovery policy and production panic containment.

## Next checkpoint
- Route the live runtime task/process state through the portable kernel core, rather than maintaining bring-up-specific runtime state in `arch_hw.c`.
