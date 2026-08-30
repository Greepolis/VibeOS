#!/usr/bin/env bash
# The verify step for sabotage runs whose cases are caught on the host.
#
# verify-boot.sh takes ninety seconds a case because it boots a virtual
# machine. The memory-manager cases do not need one: the logic they break is
# portable C, so the host tests and a short torture run score them in about a
# second, which is the difference between a case file that gets re-run and one
# that gets written once.
#
# It prints what failed, not just that something did. A case that goes red with
# no reason has proved nothing, and this project has scored a whole sabotage
# run green for exactly that reason - the verifier was missing and every case
# "passed".
set -u
cd "$(dirname "$0")/../.." || exit 1

BUILD="${1:-build-gcc-Release}"

if ! cmake --build "$BUILD" -j8 > /tmp/verify-host-build.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-host-build.log)"
    exit 1
fi

rc=0
if ! out="$("./$BUILD/vibeos_kernel_tests" 2>&1)"; then
    echo "$out" | grep -E '^FAIL' | head -2
    rc=1
fi
if ! out="$("./$BUILD/vibeos_mm_torture" 3 4000 2>&1)"; then
    echo "$out" | grep -E '^FAIL' | head -1
    rc=1
fi
if [ "$rc" -eq 0 ]; then
    echo "reason=host_tests_and_torture_pass"
fi
exit "$rc"
