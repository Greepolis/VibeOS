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
    tests=$(grep -v '^#' "${2:?--list needs a file}" | tr -d '\r' | tr '\n' ' ')
else
    tests="$*"
fi
[ -n "$tests" ] || { echo "ltp=FAIL reason=no_tests_named"; exit 2; }
for t in $tests; do
    [ -f "$B/corpus/ltp/$t" ] || { echo "ltp=FAIL reason=not_built:$t (scripts/dev/corpus-build.sh $B)"; exit 2; }
done

# A boot at a time, thirty-two tests to a boot: each test is about a third of a
# megabyte on the boot volume, and a test that wedges the machine takes only
# its own boot's remainder with it - those are reported as "did not run", and
# the one after the last verdict is the one to look at. Every verdict is also
# appended to <build>/ltp-results.txt as "<test> <rc> <first complaint>", and
# each boot's serial log kept beside it as ltp-boot-<n>.log.
BATCH=${VIBEOS_LTP_BATCH:-32}
results="$B/ltp-results.txt"
: > "$results"
n=$(echo "$tests" | wc -w)
passed=0 failed=0 broken=0 other=0 missing=0
boot=0
set -- $tests
while [ $# -gt 0 ]; do
    chunk=""
    k=0
    while [ $# -gt 0 ] && [ $k -lt "$BATCH" ]; do
        chunk="$chunk $1"
        shift
        k=$((k + 1))
    done
    boot=$((boot + 1))
    # A boot is about ninety seconds; a test is given ten on top of it. Most
    # take one. A few fork ten thousand times (fcntl14), and a test that hangs
    # is not stopped by LTP's own timeout - that is alarm(), which is L2's -
    # so it takes whatever is left of its boot.
    VIBEOS_SMOKE_LTP="$chunk" python3 scripts/qemu-cli-smoke-linux.py "$B" $((200 + 10 * k)) > /dev/null 2>&1
    log=qemu-cli-serial.log
    cp "$log" "$B/ltp-boot-$boot.log" 2>/dev/null
    for t in $chunk; do
        rc=$(grep -a -o "C:ltp: $t rc=[0-9]*" "$log" | head -1 | sed 's/.*rc=//')
        why=$(grep -a "C:ltp-$t: " "$log" | grep -a -m1 -E 'TBROK|TFAIL|TCONF|TWARN' | sed "s/.*C:ltp-$t: //" | tr -d '\r' | cut -c1-140)
        echo "$t ${rc:-norun} $why" >> "$results"
        case "$rc" in
            "") missing=$((missing + 1)); echo "  $t: did not run" ;;
            0) passed=$((passed + 1)) ;;
            *)
                if [ $((rc & 2)) -ne 0 ]; then broken=$((broken + 1));
                elif [ $((rc & 1)) -ne 0 ]; then failed=$((failed + 1));
                else other=$((other + 1)); fi
                echo "  $t: rc=$rc $why"
                ;;
        esac
    done
done
echo "ltp=ran tests=$n boots=$boot passed=$passed failed=$failed broken=$broken other=$other did_not_run=$missing"
[ "$failed" -eq 0 ] && [ "$broken" -eq 0 ] && [ "$missing" -eq 0 ]
