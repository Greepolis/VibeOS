#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
build_dir="${1:-build-module-asan}"
if ! cmake --build "$build_dir" --parallel --target vibeos_module_torture; then
    echo BUILD_FAILED
    exit 2
fi
for seed in 1 7; do
    for module in abi ipc object proc txn; do
        rounds=4000
        if [ "$module" = txn ]; then rounds=100; fi
        "./$build_dir/vibeos_module_torture" "$module" "$seed" "$rounds"
    done
done
