# Bootloader Progress

Status: Completed (Phase 4)
Last review: 2026-05-09

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
- Host-side UEFI file loader regression matrix (success, missing file, empty file, alloc failure, short read) in `tests/bootloader/uefi_bootloader_tests.c`.
- EFI build pipeline hardening with explicit intermediate image and conversion+validation stage (`cmake/convert_efi_image.cmake`):
  - objcopy primary target: `efi-app-x86_64`
  - automatic fallback: `pei-x86-64`
  - fail-fast PE32+ checks: `MZ`, `PE`, optional-header magic `0x20B`, subsystem `10`.
- Freestanding bootloader runtime memory primitives (`boot/freestanding_runtime.c`) to satisfy `-nostdlib` compiler-emitted calls (`memcpy`, `memmove`, `memset`, `memcmp`) on Linux/Clang and GCC.
- Toolchain-safe UEFI intermediate link flow for MinGW/Linux without hardcoded libc dependency.
- OVMF smoke hardening:
  - deterministic disk mapping with explicit drive/device and `bootindex`
  - network disabled (`-net none`) to avoid PXE stalls
  - extended timeout window (`60s`)
  - structured summary with status taxonomy: `pass`, `timeout`, `fail`, `infra_error`.
- Bootloader observability tokens on serial path:
  - `BL_FS_OK`
  - `BL_PLAN_OK`
  - `BL_HANDOFF_OK`
  - final success token remains `BOOT_OK`.

## Final Bootloader Checklist
- Build pipeline emits `bootloader.efi`, `vibeos_kernel.elf`, legacy `vibeos_boot.img`, and EFI media root.
- `bootloader.efi` is emitted as validated EFI application (PE32+ subsystem 10), not a raw file rename.
- EFI media contains canonical fallback path `EFI/BOOT/BOOTX64.EFI` and kernel payload `VIBEOSKR.ELF`.
- Bootloader loads kernel from filesystem (no fixed hardcoded load-address dependency).
- Segment plan enforces structural safety checks before memory copy/load.
- OVMF smoke path and logs are integrated in CI artifacts.
- Host-side regression suite is green (`vibeos-kernel-host`, `vibeos-bootloader-host`).

## Pending
- No open bootloader implementation gaps in current scope.

## Next checkpoint
- Track long-horizon improvements (post-closure): signature verification policy enforcement, richer measured-boot event export, and expanded firmware matrix.
