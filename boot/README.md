# Boot

Bootloader and early handoff implementation for VibeOS.

## Key Components

- `uefi_main.c`: UEFI entrypoint and phase orchestration.
- `uefi_file_io.c`: EFI filesystem kernel payload loading.
- `uefi_pe_loader.c`: kernel image planning and segment load (ELF64 primary, PE fallback).
- `uefi_boot_info.c`: boot contract (`vibeos_boot_info_t`) allocation/finalization.
- `uefi_boot_handoff.c`: `ExitBootServices` hardening and kernel transfer.

## Artifact Contract

Build pipeline creates an EFI media root in:

- `artifacts/efi_root/EFI/BOOT/BOOTX64.EFI`
- `artifacts/efi_root/EFI/BOOT/VIBEOSKR.ELF`

This layout is consumed by OVMF smoke scripts and CI gating.
