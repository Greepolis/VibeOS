#!/usr/bin/env bash
# Build, and run msvcrt's host tests alone: the verify step for
# scripts/dev/cases/win-msvcrt.txt.
#
#     SABOTAGE_VERIFY="bash scripts/dev/verify-msvcrt.sh build-msvcrt" \
#         python3 scripts/dev/sabotage.py user/win/msvcrt/crt_core.c \
#         scripts/dev/cases/win-msvcrt.txt build-msvcrt
#
# verify-host-quick.sh builds and runs vibeos_kernel_tests, which does not
# contain crt_core.c: a case run through it would score every sabotage NOT RED
# (or, worse, red for an unrelated reason), so msvcrt has a verifier of its
# own. The build directory is an argument because sabotage.py runs this
# command without one.
#
# A failed build says BUILD_FAILED, which sabotage.py reads as "this case proved
# nothing" rather than as red. The first failing line is printed so that a case
# that goes red says by which check.
set -u
cd "$(dirname "$0")/../.." || exit 1

BUILD="${1:-build-msvcrt}"

if ! cmake --build "$BUILD" --target vibeos_msvcrt_tests -j8 > /tmp/verify-msvcrt-build.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-msvcrt-build.log)"
    exit 1
fi
if ! out="$(timeout 120 "./$BUILD/vibeos_msvcrt_tests" 2>&1)"; then
    echo "$out" | grep -E '^FAIL' | head -2
    echo "$out" | grep -qE '^FAIL' || echo "$out" | tail -2
    exit 1
fi
echo "reason=msvcrt_host_tests_pass"
