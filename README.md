# VibeOS

VibeOS is a from-scratch operating system project focused on:
- modular architecture
- security-first design
- high-performance kernel/runtime paths
- multi-platform compatibility strategy (Linux/Windows/macOS software layers)

The repository is in active implementation mode with host-side validation and boot-path bring-up.

## Current Focus

- kernel and user-space core primitives
- bootloader and handoff stabilization
- scheduler/IPC/syscall hardening
- CI and automated regression feedback

## Build and Test

### Windows (PowerShell)

```powershell
cmake -S . -B build -DVIBEOS_BUILD_TESTS=ON -DVIBEOS_BUILD_KERNEL_IMAGE=ON
cmake --build build
powershell -ExecutionPolicy Bypass -File scripts\run-tests.ps1 -BuildDir build -Generator Ninja -Profile agent
# Optional OVMF smoke (if QEMU+OVMF firmware are available on host):
powershell -ExecutionPolicy Bypass -File scripts\run-qemu-ovmf.ps1 -BuildDir build -ExpectToken BOOT_OK -Timeout 60 -Strict
```

### Linux/WSL

```bash
cmake -S . -B build -G Ninja -DVIBEOS_BUILD_TESTS=ON -DVIBEOS_BUILD_KERNEL_IMAGE=ON
cmake --build build
ctest --test-dir build --output-on-failure
# Required UEFI smoke gate (boots EFI/BOOT/BOOTX64.EFI from artifacts/efi_root):
./scripts/qemu-smoke-ovmf-linux.sh build "BOOT_OK" 60
# Interactive boot-to-CLI gate:
./scripts/qemu-smoke-cli-linux.sh build 90
# Informational direct-loader probe:
./scripts/qemu-smoke-linux.sh build
```

Notes:
- CI uses required boot gates: host tests + OVMF smoke + boot-to-CLI smoke, while the direct-loader probe is informational.
- `qemu-system-x86_64 -kernel` has known compatibility limits with ELF64/Multiboot2 images in newer QEMU builds. The direct-loader probe classifies these loader-side incompatibilities separately from real boot regressions.
- Bootloader artifacts use a validated EFI conversion flow (`bootloader.elf` -> `bootloader.efi`) with fail-fast PE32+ and relocation checks.
- OVMF smoke emits structured diagnostics (`qemu-ovmf-summary.txt`) with explicit status reasons, selected boot profile, and last observed boot phase.
- EFI media generation includes `startup.nsh` fallback in `artifacts/efi_root` to make firmware-shell boot path deterministic.
- Boot-to-CLI smoke waits for `BOOT_OK`, verifies `CLI_READY`, sends `help`, `status`, `echo vibeos`, and `halt`, and stores `qemu-cli-summary.txt`.

## Project Structure

- `boot/`: bootloader and firmware handoff code (UEFI path + helpers)
- `kernel/`: kernel subsystems (`core`, `sched`, `mm`, `ipc`, `proc`, `arch`)
- `user/`: user-space services/runtime scaffolding (`init`, `servicemgr`, `fs`, `net`, `drivers`, `compat`)
- `include/`: public/internal subsystem headers
- `tests/`: host-side integration and subsystem regression suite
- `docs/`: architecture specs and progress tracking

## Progress Tracking

Implementation progress is tracked by macro area:

- [docs/implementation_progress.md](docs/implementation_progress.md)
- [docs/implementation_progress/bootloader.md](docs/implementation_progress/bootloader.md)
- [docs/implementation_progress/kernel_core.md](docs/implementation_progress/kernel_core.md)
- [docs/implementation_progress/process_scheduler.md](docs/implementation_progress/process_scheduler.md)
- [docs/implementation_progress/memory_manager.md](docs/implementation_progress/memory_manager.md)
- [docs/implementation_progress/virtual_memory.md](docs/implementation_progress/virtual_memory.md)
- [docs/implementation_progress/interrupt_handling.md](docs/implementation_progress/interrupt_handling.md)
- [docs/implementation_progress/system_call_interface.md](docs/implementation_progress/system_call_interface.md)
- [docs/implementation_progress/ipc_subsystem.md](docs/implementation_progress/ipc_subsystem.md)
- [docs/implementation_progress/driver_framework.md](docs/implementation_progress/driver_framework.md)
- [docs/implementation_progress/filesystem_layer.md](docs/implementation_progress/filesystem_layer.md)
- [docs/implementation_progress/networking_stack.md](docs/implementation_progress/networking_stack.md)
- [docs/implementation_progress/user_space_interface.md](docs/implementation_progress/user_space_interface.md)
- [docs/implementation_progress/init_system.md](docs/implementation_progress/init_system.md)

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for contribution workflow and coding standards.
