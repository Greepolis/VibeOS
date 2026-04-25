# Bootloader Progress

Status: In Progress
Last review: 2026-04-25

## Implemented
- UEFI entry path and protocol wrappers in `boot/uefi_main.c` and `boot/uefi_protocol.c`.
- Firmware discovery helpers (ACPI/SMBIOS and boot metadata) in `boot/uefi_firmware.c`.
- Memory map and boot memory normalization in `boot/uefi_memory.c`.
- PE image load-plan and parsing helpers in `boot/uefi_pe_loader.c`.
- Boot handoff structure builders in `boot/uefi_boot_info.c` and `boot/uefi_boot_handoff.c`.
- Serial debug output support for boot-time diagnostics in `boot/uefi_serial.c`.

## Pending
- End-to-end hardware/firmware matrix validation (different UEFI implementations).
- Hardened error-path reporting and retry strategy on partial boot failures.
- Wider automated boot smoke coverage in CI for boot handoff correctness.

## Next checkpoint
- Stabilize one reproducible QEMU/OVMF boot handoff test as CI gate for bootloader changes.
