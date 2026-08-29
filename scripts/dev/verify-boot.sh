#!/usr/bin/env bash
# The verify step for sabotage runs: build, boot, report the gate's own reason.
#
# It lives in the repo rather than in /tmp because it did not survive there.
# A sabotage run whose verify script is missing scores every case red - which
# looks exactly like every case working, and is the one failure this whole
# exercise exists to catch. The reason line is printed on purpose: a case that
# goes red with no reason has proved nothing, and that has to be visible.
set -u
cd "$(dirname "$0")/../.." || exit 1

BUILD="${1:-build-gcc-Release}"

if ! cmake --build "$BUILD" -j8 > /tmp/verify-boot-build.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-boot-build.log)"
    exit 1
fi

python3 scripts/qemu-cli-smoke-linux.py "$BUILD" 150 > /dev/null 2>&1
grep -o '^reason=.*' qemu-cli-summary.txt | cut -c1-110
grep -q cli_and_network_verified qemu-cli-summary.txt
