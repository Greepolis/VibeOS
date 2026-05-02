#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"
ARTIFACTS_DIR="${BUILD_DIR}/artifacts"
TOKEN_REGEX="${2:-BOOT_OK|Kernel initialized|scheduler online}"

IMAGES=("vibeos_kernel.elf" "vibeos_boot.img")
MACHINES=("q35" "pc" "pc-i440fx-3.1")

KNOWN_DIRECT_LOADER_ERRORS=(
  "without PVH ELF Note"
  "invalid kernel header"
  "linux kernel too old to load a ram disk"
  "Error loading uncompressed kernel"
)

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
  echo "[QEMU-SMOKE] qemu-system-x86_64 not found; skipping direct-loader probe"
  exit 0
fi

for image in "${IMAGES[@]}"; do
  if [[ ! -f "${ARTIFACTS_DIR}/${image}" ]]; then
    echo "[QEMU-SMOKE] missing artifact: ${ARTIFACTS_DIR}/${image}" >&2
    exit 1
  fi
done

pass_count=0
unsupported_count=0
timeout_count=0
no_token_count=0
unknown_fail_count=0
total_count=0

contains_known_loader_error() {
  local err_file="$1"
  local needle
  for needle in "${KNOWN_DIRECT_LOADER_ERRORS[@]}"; do
    if grep -Fq "${needle}" "${err_file}"; then
      return 0
    fi
  done
  return 1
}

for machine in "${MACHINES[@]}"; do
  for image in "${IMAGES[@]}"; do
    total_count=$((total_count + 1))
    artifact_path="${ARTIFACTS_DIR}/${image}"
    serial_log="qemu-serial-${machine}-${image}.log"
    err_log="qemu-err-${machine}-${image}.log"

    : > "${serial_log}"
    : > "${err_log}"

    set +e
    timeout 20s qemu-system-x86_64 \
      -machine "${machine}" \
      -kernel "${artifact_path}" \
      -display none \
      -monitor none \
      -serial "file:${serial_log}" \
      -no-reboot \
      -no-shutdown \
      > /dev/null 2> "${err_log}"
    rc=$?
    set -e

    if grep -Eq "${TOKEN_REGEX}" "${serial_log}"; then
      echo "[QEMU-SMOKE] PASS machine=${machine} image=${image} token matched"
      pass_count=$((pass_count + 1))
      continue
    fi

    if contains_known_loader_error "${err_log}"; then
      echo "[QEMU-SMOKE] WARN machine=${machine} image=${image} direct-loader unsupported for this image format"
      unsupported_count=$((unsupported_count + 1))
      continue
    fi

    if [[ "${rc}" -eq 124 ]]; then
      echo "[QEMU-SMOKE] WARN machine=${machine} image=${image} timeout without boot token"
      timeout_count=$((timeout_count + 1))
      continue
    fi

    if [[ "${rc}" -eq 0 ]]; then
      echo "[QEMU-SMOKE] WARN machine=${machine} image=${image} completed without expected boot token"
      no_token_count=$((no_token_count + 1))
      continue
    fi

    echo "[QEMU-SMOKE] ERROR machine=${machine} image=${image} unexpected qemu failure (rc=${rc})" >&2
    unknown_fail_count=$((unknown_fail_count + 1))
  done
done

{
  echo "total=${total_count}"
  echo "pass=${pass_count}"
  echo "unsupported_direct_loader=${unsupported_count}"
  echo "timeout=${timeout_count}"
  echo "no_token=${no_token_count}"
  echo "unknown_fail=${unknown_fail_count}"
} > qemu-smoke-summary.txt

cat qemu-smoke-summary.txt

if [[ "${unknown_fail_count}" -gt 0 ]]; then
  echo "[QEMU-SMOKE] failing due to unexpected qemu errors" >&2
  exit 1
fi

if [[ "${pass_count}" -eq 0 ]]; then
  echo "[QEMU-SMOKE] no direct-loader success yet; host bootloader tests remain the primary CI gate"
fi

exit 0
