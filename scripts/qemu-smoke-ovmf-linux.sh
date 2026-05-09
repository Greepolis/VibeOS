#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"
TOKEN_REGEX="${2:-BOOT_OK}"
TIMEOUT_SEC="${3:-60}"
EFI_ROOT="${BUILD_DIR}/artifacts/efi_root"
BOOTLOADER_EFI="${EFI_ROOT}/EFI/BOOT/BOOTX64.EFI"
KERNEL_PAYLOAD="${EFI_ROOT}/EFI/BOOT/VIBEOSKR.ELF"
SERIAL_LOG="qemu-ovmf-serial.log"
ERR_LOG="qemu-ovmf-err.log"
SUMMARY_LOG="qemu-ovmf-summary.txt"
OVMF_VARS_RUNTIME="qemu-ovmf-vars.fd"

write_summary() {
  local status="$1"
  local reason="$2"
  local rc="${3:-0}"
  {
    echo "status=${status}"
    echo "reason=${reason}"
    echo "qemu_rc=${rc}"
    echo "token_regex=${TOKEN_REGEX}"
    echo "timeout_sec=${TIMEOUT_SEC}"
    echo "ovmf_code=${OVMF_CODE:-unknown}"
    echo "ovmf_vars_template=${OVMF_VARS_TEMPLATE:-unknown}"
    echo "efi_root=${EFI_ROOT}"
    echo "serial_log=${SERIAL_LOG}"
    echo "error_log=${ERR_LOG}"
  } > "${SUMMARY_LOG}"
}

validate_efi_bootloader() {
  local image_path="$1"
  python3 - "$image_path" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "rb") as fp:
    data = fp.read()

if len(data) < 128:
    print("too_small")
    raise SystemExit(1)

if data[0:2] != b"MZ":
    print("missing_mz")
    raise SystemExit(1)

e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
if e_lfanew + 0x18 + 0x46 > len(data):
    print("pe_offset_out_of_range")
    raise SystemExit(1)

if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
    print("missing_pe_signature")
    raise SystemExit(1)

optional_magic = struct.unpack_from("<H", data, e_lfanew + 0x18)[0]
if optional_magic != 0x20B:
    print(f"optional_magic_{optional_magic:#x}")
    raise SystemExit(1)

subsystem = struct.unpack_from("<H", data, e_lfanew + 0x18 + 0x44)[0]
if subsystem != 10:
    print(f"subsystem_{subsystem}")
    raise SystemExit(1)

print("ok")
PY
}

find_ovmf_pair() {
  local code_candidates=(
    "/usr/share/OVMF/OVMF_CODE_4M.fd"
    "/usr/share/OVMF/OVMF_CODE.fd"
    "/usr/share/qemu/OVMF_CODE.fd"
    "/usr/share/ovmf/OVMF_CODE.fd"
  )
  local vars_candidates=(
    "/usr/share/OVMF/OVMF_VARS_4M.fd"
    "/usr/share/OVMF/OVMF_VARS.fd"
    "/usr/share/qemu/OVMF_VARS.fd"
    "/usr/share/ovmf/OVMF_VARS.fd"
  )
  local code=""
  local vars=""
  local c
  local v

  for c in "${code_candidates[@]}"; do
    if [[ -f "$c" ]]; then
      code="$c"
      break
    fi
  done
  for v in "${vars_candidates[@]}"; do
    if [[ -f "$v" ]]; then
      vars="$v"
      break
    fi
  done

  if [[ -n "$code" && -n "$vars" ]]; then
    echo "$code|$vars"
    return 0
  fi
  return 1
}

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  write_summary "infra_error" "qemu_missing" 1
  echo "[QEMU-OVMF] qemu-system-x86_64 not found" >&2
  exit 1
fi

if [[ ! -d "$EFI_ROOT/EFI/BOOT" ]]; then
  write_summary "infra_error" "efi_root_missing" 1
  echo "[QEMU-OVMF] EFI media root missing: ${EFI_ROOT}" >&2
  exit 1
fi

if [[ ! -f "$BOOTLOADER_EFI" || ! -f "$KERNEL_PAYLOAD" ]]; then
  write_summary "infra_error" "efi_payload_missing" 1
  echo "[QEMU-OVMF] Missing BOOTX64.EFI and/or VIBEOSKR.ELF in EFI root" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  write_summary "infra_error" "python3_missing_for_efi_validation" 1
  echo "[QEMU-OVMF] python3 not found; cannot validate BOOTX64.EFI header" >&2
  exit 1
fi

set +e
efi_validation_output="$(validate_efi_bootloader "$BOOTLOADER_EFI" 2>&1)"
efi_validation_rc=$?
set -e
if [[ "$efi_validation_rc" -ne 0 ]]; then
  write_summary "infra_error" "efi_header_invalid:${efi_validation_output}" 1
  echo "[QEMU-OVMF] BOOTX64.EFI validation failed: ${efi_validation_output}" >&2
  exit 1
fi

if ! OVMF_PAIR="$(find_ovmf_pair)"; then
  write_summary "infra_error" "ovmf_missing" 1
  echo "[QEMU-OVMF] OVMF firmware files not found" >&2
  exit 1
fi

OVMF_CODE="${OVMF_PAIR%%|*}"
OVMF_VARS_TEMPLATE="${OVMF_PAIR##*|}"
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS_RUNTIME"

: > "$SERIAL_LOG"
: > "$ERR_LOG"

set +e
timeout "${TIMEOUT_SEC}s" qemu-system-x86_64 \
  -machine q35 \
  -m 512M \
  -display none \
  -monitor none \
  -serial "file:${SERIAL_LOG}" \
  -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
  -drive if=pflash,format=raw,file="${OVMF_VARS_RUNTIME}" \
  -drive if=none,id=esp,format=raw,file=fat:rw:"${EFI_ROOT}" \
  -device virtio-blk-pci,drive=esp,bootindex=1 \
  -net none \
  -no-reboot \
  -no-shutdown \
  > /dev/null 2> "${ERR_LOG}"
rc=$?
set -e

if grep -Eq "${TOKEN_REGEX}" "${SERIAL_LOG}"; then
  write_summary "pass" "boot_token_found" "${rc}"
  echo "[QEMU-OVMF] PASS token matched (${TOKEN_REGEX})"
  exit 0
fi

if [[ "${rc}" -eq 124 ]]; then
  write_summary "timeout" "timeout_waiting_for_boot_token" "${rc}"
  echo "[QEMU-OVMF] FAIL timeout waiting for token (${TOKEN_REGEX})" >&2
  exit 2
fi

write_summary "fail" "token_missing" "${rc}"
echo "[QEMU-OVMF] FAIL no boot token and qemu rc=${rc}" >&2
exit 1
