#!/usr/bin/env bash
# Run Linux Test Project tests on VibeOS and say what each one concluded.
#
#     scripts/dev/ltp-run.sh <build-dir> <test> [<test> ...]
#     scripts/dev/ltp-run.sh <build-dir> --list <file>      one test name a line
#
# The tests are the ones scripts/dev/corpus-build.sh built against musl
# (<build>/corpus/ltp/). They are staged on the boot volume and run by the
# corpus script after the workloads (docs/abi/ L1 step 8); each prints its own
# verdict as its exit code - LTP's convention: 0 passed, 1 failed, 2 broken
# (the test could not do its work), 4 warned, 32 not applicable here - and this
# reads those out of the serial log. The boot's own pass or fail is not this
# script's subject: a boot whose LTP tests all fail still boots.
#
# One line at the end, and the first thing each test that did not pass said.
#
# As of L1 step 8 every test is "broken" before it starts: LTP's harness keeps
# its results in a page shared between the test and the child that runs it,
# which it maps from a file - mmap(MAP_SHARED) of a descriptor - and file-backed
# mappings are L3's. This script is here so that the day they exist, the oracle
# is one command away.
set -u
cd "$(dirname "$0")/../.." || exit 1

B="${1:?usage: ltp-run.sh <build-dir> <test>... | --list <file>}"
shift
if [ "${1:-}" = "--list" ]; then
    tests=$(grep -v '^#' "${2:?--list needs a file}" | tr '\n' ' ')
else
    tests="$*"
fi
[ -n "$tests" ] || { echo "ltp=FAIL reason=no_tests_named"; exit 2; }
for t in $tests; do
    [ -f "$B/corpus/ltp/$t" ] || { echo "ltp=FAIL reason=not_built:$t (scripts/dev/corpus-build.sh $B)"; exit 2; }
done

n=$(echo "$tests" | wc -w)
# A boot is about ninety seconds; a test is given two on top of it.
VIBEOS_SMOKE_LTP="$tests" python3 scripts/qemu-cli-smoke-linux.py "$B" $((200 + 2 * n)) > /dev/null 2>&1

log=qemu-cli-serial.log
passed=0 failed=0 broken=0 other=0 missing=0
for t in $tests; do
    rc=$(grep -a -o "C:ltp: $t rc=[0-9]*" "$log" | head -1 | sed 's/.*rc=//')
    case "$rc" in
        "") missing=$((missing + 1)); echo "  $t: did not run" ;;
        0) passed=$((passed + 1)) ;;
        *)
            if [ $((rc & 2)) -ne 0 ]; then broken=$((broken + 1));
            elif [ $((rc & 1)) -ne 0 ]; then failed=$((failed + 1));
            else other=$((other + 1)); fi
            why=$(grep -a "C:ltp-$t: " "$log" | grep -a -m1 -E 'TBROK|TFAIL|TCONF|TWARN' | sed "s/.*C:ltp-$t: //" | tr -d '\r' | cut -c1-140)
            echo "  $t: rc=$rc $why"
            ;;
    esac
done
echo "ltp=ran tests=$n passed=$passed failed=$failed broken=$broken other=$other did_not_run=$missing"
[ "$failed" -eq 0 ] && [ "$broken" -eq 0 ] && [ "$missing" -eq 0 ]
