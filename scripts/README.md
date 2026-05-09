# Scripts

Automation scripts used by development workflows and CI.

## Available scripts

- `run-tests.ps1`: host build/test pipeline with fallback and artifact summary.
- `run-qemu.ps1`: Windows-oriented QEMU launcher for smoke/debug runs with direct-loader incompatibility classification.
- `qemu-smoke-linux.sh`: Linux CI direct-loader compatibility probe for boot artifacts.
- `qemu-smoke-ovmf-linux.sh`: Linux OVMF smoke gate that boots from EFI media layout (`artifacts/efi_root`), validates `BOOTX64.EFI` as PE32+ EFI app, retries with compatibility profile on timeout/token-miss, and asserts `BOOT_OK`.
- `run-qemu-ovmf.ps1`: Windows OVMF smoke runner with parity behavior (profile fallback + phase diagnostics), emitting structured summary output (`status`, `reason`, `profile_used`, `last_boot_phase`).
