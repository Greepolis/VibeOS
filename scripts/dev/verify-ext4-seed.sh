#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# One seed of the ext4 torture, for a sabotage case aimed at a defect it found.
set -u
cd "$(dirname "$0")/../.." || exit 1
BUILD="${1:-build-gcc-Release}"
SEED="${2:?a seed}"
if ! cmake --build "$BUILD" --target vibeos_ext4_exercise -j8 > /tmp/verify-e4s.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-e4s.log)"
    exit 1
fi
out="$(python3 scripts/dev/ext4-torture.py "$BUILD" "$SEED" 1 2>&1 | tail -2)"
case "$out" in
    *ext4_torture=ok*) echo "reason=seed_passes" ;;
    *) echo "FAIL:$out" | head -1; exit 1 ;;
esac
