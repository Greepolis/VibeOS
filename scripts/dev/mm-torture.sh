#!/usr/bin/env bash
# Many seeds of the memory-manager torture test.
#
#   scripts/dev/mm-torture.sh [build-dir] [seeds] [rounds]
#
# Exists because the interesting failures need hundreds of sequences, and
# because a failure has to come back with the seed that produced it - a
# randomised test whose seed is not printed is a story, not a reproduction.
set -uo pipefail
cd "$(dirname "$0")/../.."

d="${1:-build-gcc-Release}"
seeds="${2:-120}"
rounds="${3:-3000}"
bin="./$d/vibeos_mm_torture"

if [ ! -x "$bin" ]; then
    echo "no $bin - build first"
    exit 2
fi

failures=0
for s in $(seq 1 "$seeds"); do
    if ! out="$("$bin" "$s" "$rounds" 2>&1)"; then
        failures=$((failures + 1))
        echo "seed $s FAILED:"
        echo "$out" | grep -E '^FAIL' | head -2
    fi
done
echo "mm-torture: failures=$failures of $seeds (rounds=$rounds)"
test "$failures" -eq 0
