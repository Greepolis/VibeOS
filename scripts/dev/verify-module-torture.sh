#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
build_dir="${1:-build-module-asan}"
if [ "$#" -ge 2 ]; then
    modules=("$2")
else
    modules=(ipc object)
fi
for module in "${modules[@]}"; do
    case "$module" in
        ipc|object) target=vibeos_module_torture; binary=vibeos_module_torture; rounds=4000 ;;
        proc) target=vibeos_proc_torture; binary=vibeos_proc_torture; rounds=5000 ;;
        txn) target=vibeos_txn_torture; binary=vibeos_txn_torture; rounds=1 ;;
        *) echo "unknown module: $module"; exit 2 ;;
    esac
    if ! cmake --build "$build_dir" --parallel --target "$target"; then
        echo BUILD_FAILED
        exit 2
    fi
    for seed in 1 7; do
        if [ "$module" = "proc" ] || [ "$module" = "txn" ]; then
            "./$build_dir/$binary" "$seed" "$rounds"
        else
            "./$build_dir/$binary" "$module" "$seed" "$rounds"
        fi
    done
done
