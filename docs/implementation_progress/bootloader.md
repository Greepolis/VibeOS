# Bootloader Progress

Status: Completed (Phase 4 boot-to-CLI verified)
Last review: 2026-05-23

## Implemented
- UEFI entry path and protocol wrappers in `boot/uefi_main.c` and `boot/uefi_protocol.c`.
- Firmware discovery helpers (ACPI/SMBIOS and boot metadata) in `boot/uefi_firmware.c`.
- Memory map normalization and `ExitBootServices` retry hardening in `boot/uefi_memory.c` and `boot/uefi_boot_handoff.c`.
- Boot structure allocation fallback (`AllocateAddress` -> `AllocateAnyPages`) with cleanup-aware error handling.
- Kernel image planning with ELF64 primary parser and PE32+ fallback (`include/vibeos/bootloader.h`, `boot/bootloader_stub.c`, `boot/uefi_pe_loader.c`).
- Real kernel load path from EFI filesystem (`boot/uefi_file_io.c`, `boot/uefi_main.c`) with primary/fallback file paths.
- Segment layout validation before load (file range, size consistency, overlap checks) in `boot/uefi_pe_loader.c`.
- Strict freestanding UEFI build path:
  - bootloader no longer links `libc` or embedded kernel objects
  - PE32+ fixup validates `Subsystem=10`, x86_64 machine type, non-stripped relocations, stack/heap reserve, and `.reloc` directory
  - `BOOTX64.EFI` is accepted and started by OVMF.
- Real EFI media pipeline in build artifacts:
  - `artifacts/efi_root/EFI/BOOT/BOOTX64.EFI`
  - `artifacts/efi_root/EFI/BOOT/VIBEOSKR.ELF`
  - manifest + SHA256 integrity entries (`cmake/create_boot_image.cmake`).
- Boot diagnostics expose phase tokens: `BL_EFI_OK`, `BL_FS_OK`, `BL_PLAN_OK`, `BL_LOAD_OK`, `BL_EBS_OK`, `BOOT_OK`, `CLI_READY`.
- OVMF smoke automation:
  - Linux required gate (`scripts/qemu-smoke-ovmf-linux.sh`)
  - Linux/WSL boot-to-CLI gate (`scripts/qemu-smoke-cli-linux.sh`)
  - Windows smoke runner (`scripts/run-qemu-ovmf.ps1`).
- CI gate model:
  - required: host tests + OVMF boot smoke + boot-to-CLI smoke
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

## Verified Evidence
- `ctest --test-dir build-wsl --output-on-failure`: 2/2 host tests passed.
- `./scripts/qemu-smoke-ovmf-linux.sh build-wsl BOOT_OK 60`: passed via OVMF primary profile.
- `bash scripts/qemu-smoke-cli-linux.sh build-wsl 90`: passed, verified `help`, `status`, `echo vibeos`, and `halt` through the serial CLI.

## Toolchain notes
- **Clang UEFI images: resolved (2026-07-22)**. Previously the Clang `bootloader.efi`, produced by the ELF→PE `objcopy` path, was rejected by OVMF at `LoadImage` (`EFI_UNSUPPORTED`, `last_phase=none`) — Clang's `.data.rel.ro`/`.rela.data.rel.ro` layout did not survive the objcopy conversion. Fixed by giving Clang a **native PE link path**: compile the bootloader with `--target=x86_64-unknown-windows-msvc` (MS x64 ABI matches UEFI) and link with `-fuse-ld=lld -Wl,-subsystem:efi_application -Wl,-entry:efi_main -nostdlib`, emitting `bootloader.efi` directly (no objcopy). Also removed a spurious `<stdio.h>` include from `boot/uefi_serial.h` so the bootloader is fully freestanding. GCC keeps the ELF→objcopy path unchanged. Both toolchains are now verified booting to CLI under QEMU+OVMF and both are boot-gated in CI.

## Future Hardening
- **Phase 4.1**: Secure Boot and Measured Boot policy path discovery via EFI GetVariable
  - Currently creates firmware tags with Phase 4 defaults (SecureBoot=0, MeasuredBoot=0)
  - TODO: Implement actual GetVariable calls to detect real firmware capabilities
  - Target: May 2026 sprint close
- **Phase 4.2**: Richer fault telemetry around boot failures (handoff errors, security policy rejections).

## Next checkpoint
- Implement GetVariable integration for real Secure Boot detection in Phase 4.1.
- Start the next kernel/userland milestone on top of the verified boot-to-CLI baseline.
