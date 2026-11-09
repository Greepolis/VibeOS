#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# NtCreateFile is exercised by the independent NT file model in
# tests/kernel/nt_abi_tests.c, not by the Linux-only ABI torture executable.
set -euo pipefail
build_dir="${1:-build-module-asan}"

if ! cmake --build "$build_dir" --parallel --target vibeos_kernel_tests; then
    echo BUILD_FAILED
    exit 2
fi
output="$("$build_dir/vibeos_kernel_tests")"
printf '%s\n' "$output"
if ! grep -q '^ALL_TESTS_PASS$' <<<"$output"; then
    echo NT_ABI_TESTS_FAILED
    exit 1
fi
