# VibeOS

**VibeOS** is an AI-driven operating system research and engineering project that leverages artificial intelligence to accelerate the design, implementation, and validation of a modern from-scratch OS. By combining human expertise with AI assistance, we're building a high-performance, secure, and modular operating system from the ground up.

## Vision

We aim to demonstrate how AI can be effectively used to:
- Accelerate OS design and architecture decisions
- Generate and validate implementation code
- Identify edge cases and security concerns early
- Optimize performance-critical components
- Maintain code quality and consistency at scale

## Primary Goals

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

## Contributing

We welcome contributions from the community! Whether you're interested in:
- Operating system design and architecture
- Kernel implementation
- AI-assisted development techniques
- Testing and validation
- Documentation and research

...please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on how to get involved.

## Current phase

Phase 2: implementation bootstrap (build, kernel core primitives, automated feedback harness)

**Current Status: Milestone M2 Complete ✓**
- M1 (Toolchain & Build Skeleton): ✓ COMPLETED (commit c860c1b)
- M2 (Boot to Kernel Banner): ✓ COMPLETED (commits 50e65f6 + 77d01c9)
  - Serial I/O primitives implemented (x86_64 COM1)
  - Kernel logs boot phases to serial
  - "BOOT_OK" marker on successful sequence
  - Assembly entry point (_start) created
  - Bootable kernel ELF executable generated
  - QEMU boot test infrastructure ready (awaits bootloader protocol refinement)

### M2 Build and Test (WSL/Linux native)

```bash
# Linux/WSL build with native toolchain
cmake -S . -B build -DVIBEOS_BUILD_TESTS=ON -DVIBEOS_BUILD_KERNEL_IMAGE=ON
cmake --build build

# Boot test (pending bootloader protocol - multiboot2/UEFI/PVH)
# qemu-system-x86_64 -machine q35 -m 512 -kernel build/artifacts/vibeos_boot.img -serial file:serial.log

# Host-side tests (Windows/WSL)
./scripts/run-tests.ps1 -BuildDir build
```

**M2 Artifacts**
- `build/vibeos_kernel`: Standalone ELF executable with entry point + kernel core + user core
- `build/artifacts/vibeos_boot.img`: Bootable kernel image (copy of ELF)
- `build/vibeos_kernel_tests`: Host-side validation binary (ALL_TESTS_PASS)

**M2 Exit Criteria**
- ✓ Kernel boots to serial output infrastructure  
- ✓ Boot stages logged and verified (in kmain)
- ✓ BOOT_OK marker codified in boot sequence
- ✓ Assembly entry point with stack setup
- ---
- ⏳ Live QEMU boot test (requires bootloader: multiboot2 stub or UEFI PVH support)

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
