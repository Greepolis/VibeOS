#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
build_dir="${1:-build-module-asan}"
if ! cmake --build "$build_dir" --parallel --target vibeos_abi_torture; then
    echo BUILD_FAILED
    exit 2
fi
for seed in 1 7; do
    "./$build_dir/vibeos_abi_torture" "$seed" 4000
done
