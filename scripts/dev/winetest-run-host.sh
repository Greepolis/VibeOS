#!/usr/bin/env bash
# Run the oracle built by winetest-build.sh under Wine on the host, and keep the
# answers VibeOS is measured against (docs/abi/windows.md).
#
#   scripts/dev/winetest-run-host.sh [build-dir] [dll...]   default: ntdll kernel32
#
# Leaves, under <build>/winetest/host/:
#   <dll>-<test>.log      the test's whole output
#   summary.txt           <dll>:<test> rc=<exit> and Wine's own summary line,
#                         "N tests executed (T marked as todo, F failures), S skipped."
#
# A test's exit code is its failure count, as Wine's test harness reports it, so
# rc=0 with no summary line is not a pass: it is a test that did not finish.
# The prefix is a throwaway one of the script's own, and no display is offered:
# what the plan measures has no windows.
set -u
cd "$(dirname "$0")/../.." || exit 1
BUILD="${1:-build-gcc-Release}"
shift || true
DLLS="${*:-ntdll kernel32}"
IN="$BUILD/winetest"
OUT="$IN/host"
WINE=$(command -v wine64 || command -v wine) || { echo "winetest-run-host: wine not installed"; exit 2; }
export WINEPREFIX=${WINETEST_PREFIX:-$HOME/.cache/vibeos-winetest/prefix} WINEDEBUG=-all
unset DISPLAY WAYLAND_DISPLAY
# Wine makes the prefix but not the directory it sits in, and says so only on
# stderr - after which every executable runs as nothing at all.
mkdir -p "$OUT" "$(dirname "$WINEPREFIX")"
"$WINE" wineboot -i > /dev/null 2>&1

: > "$OUT/summary.txt"
echo "# $("$WINE" --version)" >> "$OUT/summary.txt"
for dll in $DLLS; do
    exe="$IN/${dll}_test.exe"
    [ -f "$exe" ] || { echo "winetest-run-host: $exe missing - run winetest-build.sh"; exit 2; }
    tests=$("$WINE" "$exe" --list 2>/dev/null | tr -d '\r' | sed -n 's/^    //p')
    [ -n "$tests" ] || { echo "winetest-run-host: $exe lists no tests under $WINE"; exit 1; }
    for t in $tests; do
        # A test that hangs under Wine is a test VibeOS cannot be compared on;
        # the timeout says so in the summary rather than stopping the run.
        log="$OUT/$dll-$t.log"
        if [ -z "${WINETEST_SUMMARISE_ONLY:-}" ]; then
            timeout 180 "$WINE" "$exe" "$t" > "$log" 2>&1
            rc=$?
            echo "$rc" > "$log.rc"
        else
            rc=$(cat "$log.rc" 2>/dev/null || echo "?")
        fi
        # A test that starts copies of itself has one summary line per process,
        # each prefixed with its pid, in whatever order they ended. The parent's
        # is the one that executed the most; and if the failures printed in the
        # log outnumber those all the summaries account for, the process that
        # printed them never got as far as its summary - kernel32:virtual under
        # Wine 9.0 on WSL is that case, and taking a child's line said "0
        # failures" for it.
        sums=$(tr -d '\r' < "$log" | grep -E "^[0-9a-f]+:$t: [0-9]+ tests executed" | sed 's/^[0-9a-f]*://')
        main=$(echo "$sums" | awk '{print $2, $0}' | sort -n | tail -1 | cut -d' ' -f2-)
        counted=$(echo "$sums" | sed -n 's/.* \([0-9]*\) failures*).*/\1/p' | awk '{s += $1} END {print s + 0}')
        printed=$(tr -d '\r' < "$log" | grep -c ': Test failed: ')
        verdict=${main:-no summary line}
        if [ "$printed" -gt "$counted" ]; then
            verdict="incomplete: $printed failures printed, $counted summarised; $verdict"
        fi
        echo "$dll:$t rc=$rc $verdict" | tee -a "$OUT/summary.txt"
    done
done
