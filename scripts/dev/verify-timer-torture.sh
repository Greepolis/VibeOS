#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
build_dir="${1:-build-asan}"
if ! cmake --build "$build_dir" --parallel --target vibeos_timer_torture; then
    echo BUILD_FAILED
    exit 2
fi
"$build_dir/vibeos_timer_torture" 1 10000
"$build_dir/vibeos_timer_torture" 7 10000
