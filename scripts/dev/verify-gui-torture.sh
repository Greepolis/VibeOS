#!/usr/bin/env bash
# The verify step for sabotage runs against the GUI torture: build, then run it
# on a few seeds with threads, and print its first failure.
#
# In the repo rather than in /tmp for the reason verify-boot.sh gives: a verify
# script that is missing scores every case red, which looks exactly like every
# case working. The failure line is printed so a red case says why.
set -u
cd "$(dirname "$0")/../.." || exit 1

BUILD="${1:-build-gcc-Release}"

if ! cmake --build "$BUILD" -j8 --target vibeos_gui_torture > /tmp/verify-gui-build.log 2>&1; then
    echo "BUILD_FAILED (see /tmp/verify-gui-build.log)"
    exit 1
fi

rc=0
for s in 1 2 3; do
    if ! "./$BUILD/vibeos_gui_torture" "$s" 300 4 > /tmp/verify-gui.log 2>&1; then
        grep -m1 'FAIL' /tmp/verify-gui.log | cut -c1-160
        rc=1
        break
    fi
done
exit $rc
