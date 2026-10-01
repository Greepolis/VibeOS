#!/usr/bin/env bash
# What Linux answers the corpus's file workloads with.
#
#     scripts/dev/corpus-expect.sh [build-dir]            write tests/corpus/l1-expected.txt
#     scripts/dev/corpus-expect.sh [build-dir] --check    compare with it instead
#
# tests/corpus/run-l1.sh is run here under the host's BusyBox - the binary the
# boot image stages - in a scratch directory, and what it prints is kept. The
# boot gate compares what the same script prints on VibeOS with that file, line
# for line (docs/abi/ L1 step 8). The expectation is therefore Linux's own
# answer and nobody's opinion of it; this script is how it is refreshed when a
# workload changes, and --check is how the nightly says the file checked in is
# still what Linux says.
#
# SQLite and Lua come from scripts/dev/corpus-build.sh. Without them their
# workloads answer "absent" and the file written would say so - which is
# refused, because the file is the oracle and an oracle that says "absent"
# expects nothing.
set -u
cd "$(dirname "$0")/../.." || exit 1

B="${1:-build-gcc-Release}"
BB=$(command -v busybox) || { echo "corpus-expect=FAIL reason=no_busybox"; exit 2; }
for p in sqlite3 lua; do
    [ -x "$B/corpus/$p" ] || { echo "corpus-expect=FAIL reason=no_$p (run scripts/dev/corpus-build.sh $B --no-ltp)"; exit 2; }
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
out="$work/expected.txt"
CORPUS_DIR="$(cd "$B/corpus" && pwd)" CORPUS_WORK="$work/w" \
    "$BB" sh tests/corpus/run-l1.sh > "$out" 2> "$work/err.txt"
if ! grep -q '^C:done: ' "$out"; then
    echo "corpus-expect=FAIL reason=the_script_did_not_finish"
    head -5 "$work/err.txt"
    exit 1
fi
if grep -q ': absent$' "$out"; then
    echo "corpus-expect=FAIL reason=a_workload_was_absent"
    exit 1
fi

if [ "${2:-}" = "--check" ]; then
    if diff -u tests/corpus/l1-expected.txt "$out" > "$work/diff.txt"; then
        echo "corpus-expect=ok lines=$(wc -l < "$out")"
        exit 0
    fi
    head -30 "$work/diff.txt"
    echo "corpus-expect=FAIL reason=linux_answers_differently_now"
    exit 1
fi
cp "$out" tests/corpus/l1-expected.txt
echo "corpus-expect=written lines=$(wc -l < "$out")"
