# Bootloader Progress

Status: Completed (Phase 2 scope)
Last review: 2026-04-25

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
- Wider smoke matrix in CI (`q35`/`pc` x `vibeos_kernel.elf`/`vibeos_boot.img`) plus artifact checks.
- Boot artifact pipeline fix preserving both `vibeos_kernel.elf` and `vibeos_boot.img` for QEMU gates.

## Pending
- No open Phase 2 bootloader gaps in implementation progress.

## Next checkpoint
- Move to Phase 3 boot media pipeline: real EFI disk layout (`EFI/BOOT/BOOTX64.EFI`) and OVMF path validation with direct bootloader execution.
