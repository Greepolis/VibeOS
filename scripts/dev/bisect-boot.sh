#!/usr/bin/env bash
# Build one revision, boot it N times, and always come back to where you were.
#
#   scripts/dev/bisect-boot.sh <rev> [count] [build-dir]
#
# The "always come back" is the point. Bisecting an intermittent failure means
# leaving the tree on a detached head for half an hour at a time, and a run that
# is interrupted there leaves the next session confused about what it is even
# looking at. The trap restores the original branch whatever happens.
set -uo pipefail
cd "$(dirname "$0")/../.."

rev="${1:?usage: bisect-boot.sh <rev> [count] [build-dir]}"
n="${2:-16}"
d="${3:-build-gcc-Release}"

here="$(git rev-parse --abbrev-ref HEAD)"
[ "$here" = "HEAD" ] && here="$(git rev-parse HEAD)"

if [ -n "$(git status --porcelain)" ]; then
    echo "working tree is dirty; commit or stash first"
    exit 2
fi

restore() { git checkout -q "$here"; }
trap restore EXIT

git checkout -q "$rev" || exit 2
echo "=== $(git log --oneline -1)"
if ! cmake --build "$d" -j"$(nproc)" > /tmp/bisect-build.log 2>&1; then
    echo "BUILD_FAILED"
    grep -E 'error:' /tmp/bisect-build.log | head -3
    exit 1
fi
bash scripts/dev/repeat-boot.sh "$d" "$n" | tail -3
