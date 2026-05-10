# Bootloader Progress

Status: Phase 4 in progress (Security Hardening)
Last review: 2026-05-10

## Implemented
- UEFI entry path and protocol wrappers in `boot/uefi_main.c` and `boot/uefi_protocol.c`.
- Firmware discovery helpers (ACPI/SMBIOS and boot metadata) in `boot/uefi_firmware.c`.
- Memory map normalization and `ExitBootServices` retry hardening in `boot/uefi_memory.c` and `boot/uefi_boot_handoff.c`.
- Boot structure allocation fallback (`AllocateAddress` -> `AllocateAnyPages`) with cleanup-aware error handling.
- Kernel image planning with ELF64 primary parser and PE32+ fallback (`include/vibeos/bootloader.h`, `boot/bootloader_stub.c`, `boot/uefi_pe_loader.c`).
- Real kernel load path from EFI filesystem (`boot/uefi_file_io.c`, `boot/uefi_main.c`) with primary/fallback file paths.
- Segment layout validation before load (file range, size consistency, overlap checks) in `boot/uefi_pe_loader.c`.
- Real EFI media pipeline in build artifacts:
  - `artifacts/efi_root/EFI/BOOT/BOOTX64.EFI`
  - `artifacts/efi_root/EFI/BOOT/VIBEOSKR.ELF`
  - manifest + SHA256 integrity entries (`cmake/create_boot_image.cmake`).
- OVMF smoke automation:
  - Linux required gate (`scripts/qemu-smoke-ovmf-linux.sh`)
  - Windows smoke runner (`scripts/run-qemu-ovmf.ps1`).
- CI dual gate model:
  - required: host tests + OVMF boot smoke
  - informational: direct-loader probe (`-kernel`) for compatibility telemetry.
- Host-side parser regression coverage (PE + ELF) in `tests/kernel/kernel_tests.c`.
- **May 2 (commit 630d94c)**: Fixed serial I/O privilege detection for Clang inline asm (changed `uint16_t cs` → `unsigned long`, constraint `"=r"` → `"=&r"` with memory clobber) in `kernel/arch/x86_64/serial.c`.
- **May 3 (commit 0d4ba7f)**: Phase 4 security discovery infrastructure:
  - `uefi_firmware_discover_security_settings()` in `boot/uefi_firmware.c` discovers Secure Boot and Measured Boot capabilities
  - Firmware tags (types 3-4) created with Phase 4 default values
  - Integration in PHASE 2 (discovery) and PHASE 4 (application) boot stages
  - Security telemetry logged before kernel handoff
- **May 10 (commit 6391147, 29b81db)**: Fixed UEFI bootloader generation for Clang on Linux:
  - Added `-fPIE` and `-fno-strict-aliasing` compile flags for UEFI compatibility
  - Linked against `libc` and `clang_rt.builtins-x86_64` on Linux targets
  - Removed unsupported `--subsystem=10` flag from GNU ld (objcopy handles PE subsystem during conversion)
  - Added fallback support in objcopy conversion: `efi-app-x86_64` (primary) → `pei-x86-64` (fallback 1) → `pe-i386` (fallback 2)
  - BOOTX64.EFI now generates correctly with Clang Release builds on Linux CI

## Final Bootloader Checklist
- Build pipeline emits `bootloader.efi`, `vibeos_kernel.elf`, legacy `vibeos_boot.img`, and EFI media root.
- EFI media contains canonical fallback path `EFI/BOOT/BOOTX64.EFI` and kernel payload `VIBEOSKR.ELF`.
- Bootloader loads kernel from filesystem (no fixed hardcoded load-address dependency).
- Segment plan enforces structural safety checks before memory copy/load.
- OVMF smoke path and logs are integrated in CI artifacts.

## Pending
- **Phase 4.1 (in progress)**: Secure Boot and Measured Boot policy path discovery via EFI GetVariable
  - Currently creates firmware tags with Phase 4 defaults (SecureBoot=0, MeasuredBoot=0)
  - TODO: Implement actual GetVariable calls to detect real firmware capabilities
  - Target: May 2026 sprint close
- **Phase 4.2**: Richer fault telemetry around boot failures (handoff errors, security policy rejections)

## Next checkpoint
- Validate BOOTX64.EFI executes on QEMU OVMF with GitHub Actions Linux CI (pending rebuild)
- Implement GetVariable integration for real Secure Boot detection in Phase 4.1
- Phase 5: Kernel bring-up and boot handoff validation
