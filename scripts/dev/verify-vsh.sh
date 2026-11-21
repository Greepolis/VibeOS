#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build and run vsh's language tests alone: for sabotage cases aimed at them.
set -u
cd "$(dirname "$0")/../.." || exit 1

BUILD="${1:-build-gcc-Release}"

if ! cmake --build "$BUILD" --target vibeos_vsh_tests vibeos_vsh -j8 > /tmp/verify-vsh-build.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-vsh-build.log)"
    exit 1
fi
out="$("./$BUILD/vibeos_vsh_tests" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "$out" | grep -E '^FAIL' | head -2
    [ -n "$(echo "$out" | grep -E '^FAIL')" ] || echo "FAIL: vibeos_vsh_tests ended with status $rc and no FAIL line"
    exit 1
fi
got="$("./$BUILD/vibeos_vsh" tests/vsh/exec.vsh "$PWD/$BUILD/vibeos_vsh" 2>/dev/null | diff --strip-trailing-cr tests/vsh/exec-expected.txt - | head -3)"
if [ -n "$got" ]; then
    echo "FAIL:exec.vsh $got"
    exit 1
fi
echo "reason=host_tests_pass"
