#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"
TOKEN_REGEX="${2:-BOOT_OK}"
EFI_ROOT="${BUILD_DIR}/artifacts/efi_root"
SERIAL_LOG="qemu-ovmf-serial.log"
ERR_LOG="qemu-ovmf-err.log"
SUMMARY_LOG="qemu-ovmf-summary.txt"
OVMF_VARS_RUNTIME="qemu-ovmf-vars.fd"

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
  echo "status=infra_error" > "$SUMMARY_LOG"
  echo "reason=qemu_missing" >> "$SUMMARY_LOG"
  echo "[QEMU-OVMF] qemu-system-x86_64 not found" >&2
  exit 1
fi

if [[ ! -d "$EFI_ROOT/EFI/BOOT" ]]; then
  echo "status=infra_error" > "$SUMMARY_LOG"
  echo "reason=efi_root_missing" >> "$SUMMARY_LOG"
  echo "efi_root=${EFI_ROOT}" >> "$SUMMARY_LOG"
  echo "[QEMU-OVMF] EFI media root missing: ${EFI_ROOT}" >&2
  exit 1
fi

if [[ ! -f "$EFI_ROOT/EFI/BOOT/BOOTX64.EFI" || ! -f "$EFI_ROOT/EFI/BOOT/VIBEOSKR.ELF" ]]; then
  echo "status=infra_error" > "$SUMMARY_LOG"
  echo "reason=efi_payload_missing" >> "$SUMMARY_LOG"
  echo "[QEMU-OVMF] Missing BOOTX64.EFI and/or VIBEOSKR.ELF in EFI root" >&2
  exit 1
fi

if ! OVMF_PAIR="$(find_ovmf_pair)"; then
  echo "status=infra_error" > "$SUMMARY_LOG"
  echo "reason=ovmf_missing" >> "$SUMMARY_LOG"
  echo "[QEMU-OVMF] OVMF firmware files not found" >&2
  exit 1
fi

OVMF_CODE="${OVMF_PAIR%%|*}"
OVMF_VARS_TEMPLATE="${OVMF_PAIR##*|}"
cp "$OVMF_VARS_TEMPLATE" "$OVMF_VARS_RUNTIME"

: > "$SERIAL_LOG"
: > "$ERR_LOG"

set +e
timeout 25s qemu-system-x86_64 \
  -machine q35 \
  -m 512M \
  -display none \
  -monitor none \
  -serial "file:${SERIAL_LOG}" \
  -drive if=pflash,format=raw,readonly=on,file="${OVMF_CODE}" \
  -drive if=pflash,format=raw,file="${OVMF_VARS_RUNTIME}" \
  -drive format=raw,file=fat:rw:"${EFI_ROOT}" \
  -no-reboot \
  -no-shutdown \
  > /dev/null 2> "${ERR_LOG}"
rc=$?
set -e

if grep -Eq "${TOKEN_REGEX}" "${SERIAL_LOG}"; then
  {
    echo "status=pass"
    echo "token_regex=${TOKEN_REGEX}"
    echo "ovmf_code=${OVMF_CODE}"
    echo "ovmf_vars_template=${OVMF_VARS_TEMPLATE}"
    echo "efi_root=${EFI_ROOT}"
  } > "$SUMMARY_LOG"
  echo "[QEMU-OVMF] PASS token matched (${TOKEN_REGEX})"
  exit 0
fi

if [[ "${rc}" -eq 124 ]]; then
  {
    echo "status=timeout"
    echo "token_regex=${TOKEN_REGEX}"
    echo "ovmf_code=${OVMF_CODE}"
    echo "ovmf_vars_template=${OVMF_VARS_TEMPLATE}"
    echo "efi_root=${EFI_ROOT}"
  } > "$SUMMARY_LOG"
  echo "[QEMU-OVMF] FAIL timeout waiting for token (${TOKEN_REGEX})" >&2
  exit 2
fi

{
  echo "status=fail"
  echo "qemu_rc=${rc}"
  echo "token_regex=${TOKEN_REGEX}"
  echo "ovmf_code=${OVMF_CODE}"
  echo "ovmf_vars_template=${OVMF_VARS_TEMPLATE}"
  echo "efi_root=${EFI_ROOT}"
} > "$SUMMARY_LOG"
echo "[QEMU-OVMF] FAIL no boot token and qemu rc=${rc}" >&2
exit 1
