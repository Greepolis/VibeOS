#!/usr/bin/env bash
# Build one revision, boot it N times, and always come back to where you were.
#
#   scripts/dev/bisect-boot.sh <rev> [count] [build-dir]
#
# Run this from the *host* shell, not from inside WSL. The git half has to run
# where the repository's line-ending configuration lives: WSL's git sees CRLF in
# the working tree against LF in the index and reports every file in the
# repository as modified, so a clean tree looks dirty and this refuses to start.
# That cost one run before it was understood. The build and the boots are handed
# to WSL because that is where qemu is.
#
# The "always come back" is the point. Bisecting an intermittent failure means
# leaving the tree on a detached head for half an hour at a time, and a run
# interrupted there leaves the next session unsure what it is even looking at.
set -uo pipefail
cd "$(dirname "$0")/../.."

rev="${1:?usage: bisect-boot.sh <rev> [count] [build-dir]}"
n="${2:-16}"
d="${3:-build-gcc-Release}"

# How to reach the build tools. Empty means "run them here".
WSL="${VIBEOS_WSL:-wsl.exe -d Ubuntu}"
repo_wsl="$(pwd | sed 's|^/\([a-zA-Z]\)/|/mnt/\1/|')"

run() {
    if [ -n "$WSL" ]; then
        MSYS_NO_PATHCONV=1 $WSL bash -lc "cd '$repo_wsl' && $1"
    else
        bash -lc "$1"
    fi
}

here="$(git rev-parse --abbrev-ref HEAD)"
[ "$here" = "HEAD" ] && here="$(git rev-parse HEAD)"

git update-index -q --refresh 2>/dev/null
if [ -n "$(git status --porcelain)" ]; then
    echo "working tree is dirty; commit or stash first"
    git status --porcelain | head -5
    exit 2
fi

restore() { git checkout -q "$here"; echo "back on $here"; }
trap restore EXIT

git checkout -q "$rev" || exit 2
echo "=== $(git log --oneline -1)"
if ! run "cmake --build $d -j\$(nproc) > /tmp/bisect-build.log 2>&1"; then
    echo "BUILD_FAILED"
    run "grep -E 'error:' /tmp/bisect-build.log | head -3"
    exit 1
fi
run "bash scripts/dev/repeat-boot.sh $d $n" | tail -3
