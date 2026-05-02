# Bootloader Progress

Status: Completed (Phase 2 scope)
Last review: 2026-05-02

## Implemented
- UEFI entry path and protocol wrappers in `boot/uefi_main.c` and `boot/uefi_protocol.c`.
- Firmware discovery helpers (ACPI/SMBIOS and boot metadata) in `boot/uefi_firmware.c`.
- Memory map and boot memory normalization in `boot/uefi_memory.c`.
- PE image load-plan and parsing helpers in `boot/uefi_pe_loader.c`.
- Boot handoff structure builders in `boot/uefi_boot_info.c` and `boot/uefi_boot_handoff.c`.
- Serial debug output support for boot-time diagnostics in `boot/uefi_serial.c`.
- `ExitBootServices` hardening with real map-key refresh and retry-on-stale-key policy.
- Boot structure allocation fallback (`AllocateAddress` -> `AllocateAnyPages`) with cleanup-aware error handling.
- Firmware behavior matrix coverage via host-side UEFI mock tests (`tests/bootloader/uefi_bootloader_tests.c`).
- Linux CI direct-loader probe script (`scripts/qemu-smoke-linux.sh`) with explicit classification of known QEMU ELF loader incompatibilities.
- Windows/agent QEMU runner hardened (`scripts/run-qemu.ps1`) with machine profile selection and direct-loader incompatibility classification.
- Boot artifact pipeline fix preserving both `vibeos_kernel.elf` and `vibeos_boot.img` for QEMU gates.

## Pending
- No open Phase 2 bootloader gaps in implementation progress.

## Next checkpoint
- Move to Phase 3 boot media pipeline: real EFI disk layout (`EFI/BOOT/BOOTX64.EFI`) and OVMF path validation with direct bootloader execution.
