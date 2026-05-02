# Bootloader Progress

Status: Completed (Phase 3 scope)
Last review: 2026-05-02

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

## Final Bootloader Checklist
- Build pipeline emits `bootloader.efi`, `vibeos_kernel.elf`, legacy `vibeos_boot.img`, and EFI media root.
- EFI media contains canonical fallback path `EFI/BOOT/BOOTX64.EFI` and kernel payload `VIBEOSKR.ELF`.
- Bootloader loads kernel from filesystem (no fixed hardcoded load-address dependency).
- Segment plan enforces structural safety checks before memory copy/load.
- OVMF smoke path and logs are integrated in CI artifacts.

## Pending
- No open Phase 3 bootloader gaps in implementation progress.

## Next checkpoint
- Phase 4 boot hardening: Secure Boot policy path, measured boot metadata handoff, and richer fault telemetry around handoff failures.
