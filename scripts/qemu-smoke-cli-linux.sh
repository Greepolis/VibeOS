#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-build}"
TIMEOUT_SEC="${2:-90}"

if ! command -v python3 >/dev/null 2>&1; then
  echo "[QEMU-CLI] python3 not found" >&2
  exit 1
fi

python3 "$(dirname "$0")/qemu-cli-smoke-linux.py" "$BUILD_DIR" "$TIMEOUT_SEC"
