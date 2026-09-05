#!/usr/bin/env bash
# The soak property of the memory manager's P7, plus swap, in one run.
#
# The boot gate already compares frames free on the way into userland against
# frames free on the way out. What a single figure cannot say is whether the
# twenty-six it reports are a *fixed* cost paid once at startup or a
# *per-round* leak that a short run makes look small - the same number with
# opposite consequences on a machine that stays up.
#
# So this boots at two round counts and compares. The verdict lives in
# scripts/dev/soak-report.py, which the nightly uses too: two copies of "what
# the soak asserts" is how two things come to disagree about the same fact.
#
# Usage: bash scripts/dev/soak.sh [build-dir] [low-rounds] [high-rounds]
#
# It reconfigures the build directory, so do not run it while a repeat-boot is
# in flight - CLAUDE.md has that rule and it has invalidated three runs.
set -uo pipefail

build="${1:-build-gcc-Release}"
low="${2:-120}"
high="${3:-12000}"

run_at() {
    rounds="$1"
    out="$2"
    cmake -S . -B "$build" -DVIBEOS_STRESS_ROUNDS="$rounds" >/dev/null 2>&1
    # Captured, then matched. Piping into `grep -q` looks equivalent and is
    # not: grep exits at the first match and closes the pipe, check.sh takes
    # SIGPIPE, and with `pipefail` the whole pipeline reports failure - so a
    # perfectly good build read as a broken one and this script silently
    # produced no logs at all. It cost one run to find.
    build_out="$(bash scripts/dev/check.sh build "$build" 2>&1)"
    case "$build_out" in
        *"rc=0"*) ;;
        *) echo "rounds=$rounds BUILD FAILED"
           echo "$build_out" | tail -5
           return 1 ;;
    esac
    # A generous budget: twelve thousand rounds of fork and copy-on-write is
    # not fast under TCG, and a timeout would read as a wedge.
    python3 scripts/qemu-cli-smoke-linux.py "$build" 1800 >/dev/null 2>&1
    cp qemu-cli-serial.log "$out"
}

echo "soak: comparing $low rounds against $high"
run_at "$low"  soak-low.log
run_at "$high" soak-high.log
python3 scripts/dev/soak-report.py soak-low.log soak-high.log
rc=$?

# Left as the tree found it, so the next check.sh does not silently measure a
# soak build.
cmake -S . -B "$build" -DVIBEOS_STRESS_ROUNDS= >/dev/null 2>&1
echo "soak: build dir restored to the default round count"
exit $rc
