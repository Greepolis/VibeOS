# VibeOS

VibeOS is a from-scratch operating system research and engineering project focused on four primary goals:

- high performance on modern multi-core hardware
- security-first isolation and hardening
- modular architecture for long-term maintainability
- multi-OS application compatibility via dedicated runtime and compatibility subsystems

This repository currently contains Phase 1 project design artifacts:

- product vision and engineering goals
- kernel and platform architecture
- subsystem specifications
- build, toolchain, and testing strategy
- compatibility strategy for Linux, Windows, and macOS applications
- repository layout and module boundaries
- phased roadmap and milestone plan

Phase 1 intentionally prioritizes design clarity over implementation. The first executable code phase will begin only after the initial architecture is documented, reviewed, and stabilized.

## Repository layout

- `docs/` system specifications and subsystem design documents
- `design/` diagrams, module maps, and repository structure
- `roadmap/` development plan and milestones
- `research/` comparative analysis and rationale
- `boot/`, `kernel/`, `user/` reserved implementation roots for the next phase

## Architectural direction

VibeOS adopts a hybrid microkernel-inspired modular architecture:

- a small privileged kernel core handles scheduling, address spaces, IPC primitives, interrupts, and low-level memory management
- selected performance-critical services may run in kernel space behind strict module interfaces
- drivers, filesystems, compatibility services, and high-risk components are biased toward user space

This balances performance, fault isolation, portability, and long-term compatibility goals better than a classic monolithic design.

## Current phase

Phase 2: implementation bootstrap (build, kernel core primitives, automated feedback harness)

**Current Status: Milestone M2 In Progress**
- M1 (Toolchain & Build Skeleton): ✓ COMPLETED
- M2 (Boot to Kernel Banner): 🏗️ IN PROGRESS (serial I/O ready, QEMU test ready)
  - Serial I/O primitives implemented
  - Kernel logs boot phases to serial
  - "BOOT_OK" marker on successful boot
  - QEMU boot smoke test framework ready (requires QEMU installation)

### M2 Quick Start (when QEMU installed)

```powershell
# Build with M2 serial support
cmake -S . -B build -G Ninja -DVIBEOS_BUILD_TESTS=ON -DVIBEOS_BUILD_KERNEL_IMAGE=ON
cmake --build build

# Run M2 boot smoke test on QEMU
./scripts/run-tests.ps1 -BuildDir build -RunM2BootSmoke

# Or directly test kernel boot
./scripts/run-qemu.ps1 -BuildDir build -ImagePath build/artifacts/vibeos_boot.img -ExpectToken "BOOT_OK"
```

**Expected Serial Output (M2)**
```
[BOOT] VibeOS kernel starting...
[BOOT] Boot info validated
[BOOT] Memory manager initialized
[BOOT] Virtual memory initialized
[BOOT] Memory stage ready
[BOOT] Scheduler stage ready
[BOOT] BOOT_OK
[BOOT] VibeOS kernel ready for user-space
```

- `docs/software_architecture_specification.md`
- `docs/implementation_progress.md`
- `docs/adrs/README.md`
- `docs/test_automation_spec.md`
- `docs/test_feedback_profiles.md`
- `docs/vision.md`
- `docs/architecture.md`
- `docs/ipc.md`
- `docs/toolchain.md`
- `docs/testing_strategy.md`
- `roadmap/development_phases.md`
- `roadmap/phase2_technical_backlog.md`
- `roadmap/milestones.md`
- `roadmap/risk_register.md`
