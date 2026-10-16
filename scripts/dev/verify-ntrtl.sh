#!/usr/bin/env bash
# Build, and run the Rtl host tests alone: the verify step for sabotage runs of
# scripts/dev/cases/win-ntrtl.txt (docs/abi/windows.md W2).
#
#     SABOTAGE_VERIFY="bash scripts/dev/verify-ntrtl.sh build-gcc-Release" \
#         python3 scripts/dev/sabotage.py user/win/ntdll/rtl/ip.c scripts/dev/cases/win-ntrtl.txt build-gcc-Release
#
# vibeos_ntrtl_tests is a small executable of its own, so this is about a second
# where verify-host.sh builds everything. It prints what failed, not only that
# something did - and says so when the binary died without printing a FAIL line,
# because a crash that scores red with no reason has proved nothing.
set -u
cd "$(dirname "$0")/../.." || exit 1

BUILD="${1:-build-gcc-Release}"

if ! cmake --build "$BUILD" --target vibeos_ntrtl_tests -j8 > /tmp/verify-ntrtl-build.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-ntrtl-build.log)"
    exit 1
fi
out="$("./$BUILD/vibeos_ntrtl_tests" 2>&1)"
rc=$?
if [ "$rc" -ne 0 ]; then
    fails="$(echo "$out" | grep -E 'FAIL' | head -2)"
    if [ -n "$fails" ]; then
        echo "$fails"
    else
        echo "FAIL: vibeos_ntrtl_tests ended with status $rc and no FAIL line"
    fi
    exit 1
fi
echo "reason=ntrtl_tests_pass"
