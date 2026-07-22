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

## Known Limitations
- **Clang-built EFI is rejected at load by UEFI**. Diagnosed 2026-07-22 (clang 18 in WSL):
  - The Clang `bootloader.efi` generates and passes the PE fixup, and its PE32+ optional header looks valid (Magic 0x20b, Subsystem 10, SectionAlignment 0x1000, valid `.reloc` directory) — nearly identical to the GCC image.
  - Yet OVMF refuses to start it: `BdsDxe: failed to load ... Unsupported`, and running it from the UEFI shell gives `Script Error Status: Unsupported` — i.e. `LoadImage` returns `EFI_UNSUPPORTED` **before execution** (hence `last_phase=none`).
  - Structural difference vs GCC: Clang emits an extra `.data.rel.ro` + `.rela.data.rel.ro` (relocatable read-only data) and a different section layout. The `objcopy --output-target=efi-app-x86_64` ELF→PE path does not translate this cleanly. Adding `.data.rel.ro`/`.got` to the objcopy keep-list did **not** fix the load rejection (verified), so the problem is the ELF→PE conversion itself, not just a dropped section.
  - **Recommended real fix** (future, focused task): stop converting an ELF with `objcopy` and instead link the bootloader directly to a native PE with LLD — `clang -target x86_64-unknown-windows -fuse-ld=lld -Wl,-subsystem:efi_application -Wl,-entry:efi_main -nostdlib` — which produces a UEFI-conformant PE without the fragile objcopy step. `lld` is now available.
  - Impact: **none on the shipping path** — GCC is the reference bootable toolchain and is fully boot-gated. CI runs the QEMU boot smokes and VM-image build on the GCC matrix; Clang jobs still build everything and run host tests as a portability gate.

## Future Hardening
- **Phase 4.1**: Secure Boot and Measured Boot policy path discovery via EFI GetVariable
  - Currently creates firmware tags with Phase 4 defaults (SecureBoot=0, MeasuredBoot=0)
  - TODO: Implement actual GetVariable calls to detect real firmware capabilities
  - Target: May 2026 sprint close
- **Phase 4.2**: Richer fault telemetry around boot failures (handoff errors, security policy rejections).

## Next checkpoint
- Diagnose and fix the Clang EFI boot path so boot gates can re-include Clang (see Known Limitations).
- Implement GetVariable integration for real Secure Boot detection in Phase 4.1.
- Start the next kernel/userland milestone on top of the verified boot-to-CLI baseline.
