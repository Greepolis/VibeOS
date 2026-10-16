#!/usr/bin/env bash
# Run Wine's conformance tests on VibeOS and hold each against Wine's own answer
# (docs/abi/windows.md W2): the NT personality's oracle, as ltp-run.sh is the
# Linux one's.
#
#     scripts/dev/winetest-run.sh <build-dir> <dll:test> [<dll:test> ...]
#     e.g. scripts/dev/winetest-run.sh build-gcc-Release ntdll:om ntdll:sync
#
# Needs winetest-build.sh to have built <build>/winetest/<dll>_test.exe. The gate
# stages the executables at /win beside our DLLs and the corpus script runs each
# as "<dll>_test.exe <test>", keeping its last lines and its exit code - Wine's
# harness's failure count - as "C:wt: <dll:test> rc=<n>". One boot for all of
# them: a test that cannot get further than a missing function says so and ends.
#
# Each verdict is set beside tests/corpus/winetest-wine9.txt, what Wine 9.0
# answered on the host: a test is "as Wine" when it finished with Wine's
# summary line and Wine's failure count. Until kernel32 exists most will end at
# "unimplemented: KERNEL32.dll.<function>", and that line is the result: the
# next function to write. Results go to <build>/winetest-results.txt.
set -u
cd "$(dirname "$0")/../.." || exit 1
B="${1:?usage: winetest-run.sh <build-dir> <dll:test>...}"
shift
[ $# -gt 0 ] || { echo "winetest=FAIL reason=no_tests_named"; exit 2; }
for t in "$@"; do
    dll=${t%%:*}
    [ -f "$B/winetest/${dll}_test.exe" ] || {
        echo "winetest=FAIL reason=not_built:$dll (scripts/dev/winetest-build.sh $B)"; exit 2; }
done
answers=tests/corpus/winetest-wine9.txt
# The last run's verdict and log moved aside first, as boots.sh does: a gate
# that dies before writing its own - an image that would not build, on this
# script's first run - left them to be read as this run's, and the first
# "results" of Wine's tests on VibeOS were a sabotage boot's log.
mv -f qemu-cli-summary.txt qemu-cli-summary.prev.txt 2>/dev/null
mv -f qemu-cli-serial.log qemu-cli-serial.prev.log 2>/dev/null
VIBEOS_SMOKE_WINETEST="$*" python3 scripts/qemu-cli-smoke-linux.py "$B" 400 > "$B/winetest-gate.out" 2>&1
if [ ! -f qemu-cli-summary.txt ] || [ ! -f qemu-cli-serial.log ]; then
    echo "winetest=FAIL reason=gate_wrote_no_verdict (see $B/winetest-gate.out)"
    exit 2
fi
log=qemu-cli-serial.log
cp "$log" "$B/winetest-boot.log" 2>/dev/null
results="$B/winetest-results.txt"
: > "$results"
as_wine=0 other=0 missing=0
for t in "$@"; do
    rc=$(grep -a -o "C:wt: $t rc=[0-9]*" "$log" | head -1 | sed 's/.*rc=//')
    stop=$(grep -a "C:wt-$t: " "$log" | grep -a -m1 "unimplemented: " | sed 's/.*unimplemented: //' | tr -d '\r')
    summary=$(grep -a "C:wt-$t: " "$log" | grep -a -E "tests executed" | tail -1 | sed "s/.*C:wt-$t: //;s/^[0-9a-f]*://" | tr -d '\r')
    wine=$(grep -E "^$t rc=" "$answers" 2>/dev/null | head -1 | sed 's/^[^ ]* //')
    if [ -z "$rc" ]; then
        missing=$((missing + 1)); verdict="did_not_run"
    elif [ -n "$stop" ]; then
        other=$((other + 1)); verdict="stopped at $stop"
    elif [ -n "$summary" ] && [ "rc=$rc $summary" = "$wine" ]; then
        as_wine=$((as_wine + 1)); verdict="as_wine"
    else
        other=$((other + 1)); verdict="rc=$rc ${summary:-no summary line} (wine: ${wine:-none})"
    fi
    echo "$t $verdict" | tee -a "$results"
done
echo "winetest=ran tests=$# as_wine=$as_wine other=$other did_not_run=$missing"
