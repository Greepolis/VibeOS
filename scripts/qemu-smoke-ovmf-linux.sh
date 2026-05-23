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
SERIAL_TAIL_LINES=30
MAX_ATTEMPTS=2

write_summary() {
  local status="$1"
  local reason="$2"
  local rc="${3:-0}"
  local profile="${4:-none}"
  local last_phase="${5:-none}"
  local attempts="${6:-0}"
  local serial_tail="${7:-}"
  local error_tail="${8:-}"
  {
    echo "status=${status}"
    echo "reason=${reason}"
    echo "qemu_rc=${rc}"
    echo "token_regex=${TOKEN_REGEX}"
    echo "timeout_sec=${TIMEOUT_SEC}"
    echo "profile_used=${profile}"
    echo "last_boot_phase=${last_phase}"
    echo "attempts=${attempts}"
    echo "ovmf_code=${OVMF_CODE:-unknown}"
    echo "ovmf_vars_template=${OVMF_VARS_TEMPLATE:-unknown}"
    echo "efi_root=${EFI_ROOT}"
    echo "serial_log=${SERIAL_LOG}"
    echo "error_log=${ERR_LOG}"
    echo "serial_tail=${serial_tail}"
    echo "error_tail=${error_tail}"
  } > "${SUMMARY_LOG}"
}

tail_inline() {
  local path="$1"
  if [[ ! -f "$path" ]]; then
    echo ""
    return
  fi

  tail -n "${SERIAL_TAIL_LINES}" "$path" \
    | tr '\r' ' ' \
    | tr '\n' '|' \
    | sed 's/|$//'
}

detect_last_phase() {
  local serial_path="$1"
  local phase="none"

  if [[ -f "$serial_path" ]]; then
    if grep -Eq "BL_FS_OK" "$serial_path"; then
      phase="BL_FS_OK"
    fi
    if grep -Eq "BL_PLAN_OK" "$serial_path"; then
      phase="BL_PLAN_OK"
    fi
    if grep -Eq "BL_LOAD_OK" "$serial_path"; then
      phase="BL_LOAD_OK"
    fi
    if grep -Eq "BL_HANDOFF_OK|BL_HANDOFF_START" "$serial_path"; then
      phase="BL_HANDOFF_OK"
    fi
    if grep -Eq "BL_EBS_OK" "$serial_path"; then
      phase="BL_EBS_OK"
    fi
    if grep -Eq "${TOKEN_REGEX}" "$serial_path"; then
      phase="BOOT_OK"
    fi
    if grep -Eq "CLI_READY|vibeos> " "$serial_path"; then
      phase="CLI_READY"
    fi
  fi

  echo "$phase"
}

phase_score() {
  local phase="$1"
  case "$phase" in
    CLI_READY) echo 7 ;;
    BOOT_OK) echo 6 ;;
    BL_EBS_OK) echo 5 ;;
    BL_LOAD_OK) echo 4 ;;
    BL_HANDOFF_OK) echo 3 ;;
    BL_PLAN_OK) echo 2 ;;
    BL_FS_OK) echo 1 ;;
    *) echo 0 ;;
  esac
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

characteristics = struct.unpack_from("<H", data, e_lfanew + 0x16)[0]
if characteristics & 0x1:
    print("relocs_stripped")
    raise SystemExit(1)

number_of_rva_and_sizes = struct.unpack_from("<I", data, e_lfanew + 0x18 + 0x6C)[0]
if number_of_rva_and_sizes <= 5:
    print("reloc_directory_slot_missing")
    raise SystemExit(1)

reloc_rva, reloc_size = struct.unpack_from("<II", data, e_lfanew + 0x18 + 0x70 + (5 * 8))
if reloc_rva == 0 or reloc_size < 8:
    print("reloc_directory_missing")
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
  write_summary "infra_error" "qemu_missing" 1 "none" "none" 0 "" ""
  echo "[QEMU-OVMF] qemu-system-x86_64 not found" >&2
  exit 1
fi

if [[ ! -d "$EFI_ROOT/EFI/BOOT" ]]; then
  write_summary "infra_error" "efi_root_missing" 1 "none" "none" 0 "" ""
  echo "[QEMU-OVMF] EFI media root missing: ${EFI_ROOT}" >&2
  exit 1
fi

if [[ ! -f "$BOOTLOADER_EFI" || ! -f "$KERNEL_PAYLOAD" ]]; then
  write_summary "infra_error" "efi_payload_missing" 1 "none" "none" 0 "" ""
  echo "[QEMU-OVMF] Missing BOOTX64.EFI and/or VIBEOSKR.ELF in EFI root" >&2
  exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
  write_summary "infra_error" "python3_missing_for_efi_validation" 1 "none" "none" 0 "" ""
  echo "[QEMU-OVMF] python3 not found; cannot validate BOOTX64.EFI header" >&2
  exit 1
fi

set +e
efi_validation_output="$(validate_efi_bootloader "$BOOTLOADER_EFI" 2>&1)"
efi_validation_rc=$?
set -e
if [[ "$efi_validation_rc" -ne 0 ]]; then
  write_summary "infra_error" "efi_header_invalid:${efi_validation_output}" 1 "none" "none" 0 "" ""
  echo "[QEMU-OVMF] BOOTX64.EFI validation failed: ${efi_validation_output}" >&2
  exit 1
fi

if ! OVMF_PAIR="$(find_ovmf_pair)"; then
  write_summary "infra_error" "ovmf_missing" 1 "none" "none" 0 "" ""
  echo "[QEMU-OVMF] OVMF firmware files not found" >&2
  exit 1
fi

OVMF_CODE="${OVMF_PAIR%%|*}"
OVMF_VARS_TEMPLATE="${OVMF_PAIR##*|}"

run_profile() {
  local profile="$1"
  local machine="$2"
  local vars_runtime="qemu-ovmf-vars-${profile}.fd"
  local attempt_serial="qemu-ovmf-serial-${profile}.log"
  local attempt_err="qemu-ovmf-err-${profile}.log"

  cp "$OVMF_VARS_TEMPLATE" "$vars_runtime"
  : > "$attempt_serial"
  : > "$attempt_err"

  local -a cmd=(
    qemu-system-x86_64
    -machine "$machine"
    -m 512M
    -display none
    -monitor none
    -serial "file:${attempt_serial}"
    -drive "if=pflash,format=raw,readonly=on,file=${OVMF_CODE}"
    -drive "if=pflash,format=raw,file=${vars_runtime}"
    -net none
    -no-reboot
    -no-shutdown
  )

  if [[ "$profile" == "primary" ]]; then
    cmd+=(
      -drive "if=none,id=esp,format=raw,file=fat:rw:${EFI_ROOT}"
      -device "virtio-blk-pci,drive=esp,bootindex=1"
    )
  else
    cmd+=(
      -drive "if=ide,index=0,media=disk,format=raw,file=fat:rw:${EFI_ROOT}"
    )
  fi

  set +e
  timeout "${TIMEOUT_SEC}s" "${cmd[@]}" > /dev/null 2> "${attempt_err}"
  local rc=$?
  set -e

  local phase
  phase="$(detect_last_phase "${attempt_serial}")"
  local serial_tail
  serial_tail="$(tail_inline "${attempt_serial}")"
  local err_tail
  err_tail="$(tail_inline "${attempt_err}")"

  local token_found=0
  if grep -Eq "${TOKEN_REGEX}" "${attempt_serial}"; then
    token_found=1
  fi

  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "${profile}" "${rc}" "${phase}" "${token_found}" "${attempt_serial}" "${attempt_err}" "${serial_tail}" "${err_tail}"
}

profiles=("primary:q35" "compat:pc")
attempts=0
best_profile="none"
best_rc=1
best_phase="none"
best_serial=""
best_err=""
best_serial_tail=""
best_err_tail=""
best_score=-1
seen_timeout=0

for profile_machine in "${profiles[@]}"; do
  profile="${profile_machine%%:*}"
  machine="${profile_machine##*:}"
  attempts=$((attempts + 1))
  result="$(run_profile "$profile" "$machine")"
  IFS=$'\t' read -r profile_name rc phase token_found serial_path err_path serial_tail err_tail <<< "$result"

  if [[ "$rc" -eq 124 ]]; then
    seen_timeout=1
  fi

  score="$(phase_score "$phase")"
  if [[ "$score" -gt "$best_score" ]]; then
    best_score="$score"
    best_profile="$profile_name"
    best_rc="$rc"
    best_phase="$phase"
    best_serial="$serial_path"
    best_err="$err_path"
    best_serial_tail="$serial_tail"
    best_err_tail="$err_tail"
  fi

  if [[ "$token_found" -eq 1 ]]; then
    cp "$serial_path" "$SERIAL_LOG"
    cp "$err_path" "$ERR_LOG"
    write_summary "pass" "boot_token_found" "$rc" "$profile_name" "BOOT_OK" "$attempts" "$serial_tail" "$err_tail"
    echo "[QEMU-OVMF] PASS token matched (${TOKEN_REGEX}) via profile=${profile_name}"
    exit 0
  fi
done

if [[ -n "$best_serial" && -f "$best_serial" ]]; then
  cp "$best_serial" "$SERIAL_LOG"
else
  : > "$SERIAL_LOG"
fi
if [[ -n "$best_err" && -f "$best_err" ]]; then
  cp "$best_err" "$ERR_LOG"
else
  : > "$ERR_LOG"
fi

if [[ "$seen_timeout" -eq 1 ]]; then
  write_summary "timeout" "timeout_waiting_for_boot_token" "$best_rc" "$best_profile" "$best_phase" "$attempts" "$best_serial_tail" "$best_err_tail"
  echo "[QEMU-OVMF] FAIL timeout waiting for token (${TOKEN_REGEX}); last_phase=${best_phase} profile=${best_profile}" >&2
  exit 2
fi

write_summary "fail" "token_missing" "$best_rc" "$best_profile" "$best_phase" "$attempts" "$best_serial_tail" "$best_err_tail"
echo "[QEMU-OVMF] FAIL no boot token; last_phase=${best_phase} profile=${best_profile} qemu_rc=${best_rc}" >&2
exit 1
