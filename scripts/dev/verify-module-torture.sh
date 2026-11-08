#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
build_dir="${1:-build-module-asan}"
if ! cmake --build "$build_dir" --parallel --target vibeos_module_torture; then
    echo BUILD_FAILED
    exit 2
fi
if [ "$#" -ge 2 ]; then
    modules=("$2")
else
    modules=(ipc object)
fi
for seed in 1 7; do
    for module in "${modules[@]}"; do
        case "$module" in
            ipc|object) rounds=4000 ;;
            *) echo "unknown module: $module"; exit 2 ;;
        esac
        "./$build_dir/vibeos_module_torture" "$module" "$seed" "$rounds"
    done
done
