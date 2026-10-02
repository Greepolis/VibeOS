#!/usr/bin/env bash
# Build, and run the host tests alone: a second, not nine.
#
#     SABOTAGE_VERIFY="bash scripts/dev/verify-host-quick.sh" python3 scripts/dev/sabotage.py ...
#
# verify-host.sh also runs the lock, memory and GUI tortures, which is right
# for a case in the code they cover and eight seconds of nothing for a case in
# a syscall handler - and a case file is a dozen of those. Use this one when
# the test a case aims at is in vibeos_kernel_tests; use verify-host.sh when
# it is a torture's, and before believing a whole file.
set -u
cd "$(dirname "$0")/../.." || exit 1

BUILD="${1:-build-gcc-Release}"

if ! cmake --build "$BUILD" --target vibeos_kernel_tests -j8 > /tmp/verify-host-build.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-host-build.log)"
    exit 1
fi
if ! out="$("./$BUILD/vibeos_kernel_tests" 2>&1)"; then
    echo "$out" | grep -E '^FAIL' | head -2
    exit 1
fi
echo "reason=host_tests_pass"
