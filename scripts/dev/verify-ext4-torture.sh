#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# ext4's intensive test, short (docs/filesystem-design/ext4.md step 5): the
# exerciser rebuilt, then scripts/dev/ext4-torture.py on a fixed range of seeds,
# a few of them with power cuts. check.sh runs this and the sabotage cases use it
# as their verifier; the nightly runs the long version.
#
#     scripts/dev/verify-ext4-torture.sh [build-dir] [seeds] [cut-seeds]
#
# Prints the torture's own last line: ext4_torture=ok ..., =skip ..., or =FAILED.
B="${1:-build-gcc-Release}"
SEEDS="${2:-60}"
CUT_SEEDS="${3:-12}"
cd "$(dirname "$0")/../.." || exit 2
if ! cmake --build "$B" --target vibeos_ext4_exercise > /dev/null 2>&1; then
    echo "ext4_torture=FAILED seed=- reason=exercise_not_built"
    exit 2
fi
rc=0
for run in "1 $SEEDS" "100001 $CUT_SEEDS --cuts 6"; do
    # shellcheck disable=SC2086  # the run's words are meant to split
    line="$(python3 scripts/dev/ext4-torture.py "$B" $run | tail -1)"
    echo "$line"
    case "$line" in ext4_torture=ok*|ext4_torture=skip*) ;; *) rc=1 ;; esac
done
exit "$rc"
