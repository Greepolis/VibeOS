#!/usr/bin/env bash
# The repository's own checks, the ones that read the tree rather than run it.
#
#   scripts/dev/static-checks.sh [build-dir]
#
# Prints each check's verdict line and exits non-zero if any says FAIL, or if a
# check printed no verdict at all. With a build directory, also runs the module
# check, which reads the objects; without one that check is skipped and says so.
#
# These lived inside check.sh and ran only on a developer's machine: no workflow
# called any of them, so a change that broke one could merge with every CI job
# green (external review, 2026-10-07). check.sh and the CI both call this file
# now, so the list cannot be kept in two places and drift.
#
# Why each check exists is written at the top of the check itself, and here
# where it is not obvious from the name.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1

d="${1:-}"
bad=0

run() {   # run <verdict-name> <command...>: print the verdict line, judge it
    local name="$1" line
    shift
    line=$("$@" 2>&1 | tail -1)
    echo "$line"
    case "$line" in
        "$name"=ok*|"$name"=skip*) ;;
        *) bad=1
           case "$line" in "$name"=*) ;; *) echo "  $name printed no verdict" ;; esac ;;
    esac
}

# The memory manager's layering, checked by the build rather than by review:
# one place decides what an address space owns.
run mm-layering            bash scripts/dev/check-mm-layering.sh
# Two checks about the checks: an assertion nobody proved can fire, and a
# counter nothing increments. VIBEOS_BLK_TIMEOUT was both at once.
run assertions-covered     python3 scripts/dev/check-assertions-covered.py
run counters-produced      python3 scripts/dev/check-counters-produced.py
# A MUSTBEZERO counter the gate never reads is green by construction.
run mustbezero-asserted    python3 scripts/dev/check-mustbezero-asserted.py
# Every module gets an intensive nightly run.
run nightly-coverage       python3 scripts/dev/check-nightly-coverage.py
# Which of the portable kernel's functions nothing reaches. Written two days
# before it was wired in, and unwired for both of them.
run reachable              python3 scripts/dev/check-reachable.py
# No decision rests on the reverse map alone.
run rmap-crosscheck        python3 scripts/dev/check-rmap-crosscheck.py
# Every security check has one call site, and a second one is noticed.
run chokepoints            python3 scripts/dev/check-chokepoints.py
# Every operation runs exactly the checks it declares (C4).
run syscall-checks         python3 scripts/dev/check-syscall-checks.py
# The published syscall table is the registry's, not a copy that drifted.
run syscall-table          python3 scripts/dev/make-syscall-table.py --check
# Every sabotage case still has an anchor to break.
run sabotage-anchors       python3 scripts/dev/check-sabotage-anchors.py
if [ -n "$d" ]; then
    # The seven parts of a module, for the four a script can judge.
    run subsystem          python3 scripts/dev/check-subsystem.py "$d"
else
    echo "subsystem=skip reason=no_build_dir"
fi
# How many existing files an extension costs.
run blast-radius           python3 scripts/dev/check-blast-radius.py
# One definition of what a task is (C5).
run task-identity          python3 scripts/dev/check-task-identity.py
# User memory only through the fault-safe copy (M-050..M-052).
run user-access            python3 scripts/dev/check-user-access.py
# Every socket call that waits re-verifies its socket (M-020).
run net-stable             python3 scripts/dev/check-net-stable.py
# docs/abi/ A2: a personality reaches the kernel only through vibeos/ksvc.h.
run abi-layering           python3 scripts/dev/check-abi-layering.py
# docs/abi/ A5: every Linux layout is compared with Linux's own headers.
run linux-layout           python3 scripts/dev/check-linux-layout.py
# A deliberate fault planted to test the panic path once got committed,
# because removing it was a separate step that a change of plan skipped.
if grep -rn 'TEMPORARY' kernel/ --include=*.c > /dev/null 2>&1; then
    echo "LEFTOVER-DEBUG-CODE:"
    grep -rn 'TEMPORARY' kernel/ --include=*.c | head -3
    bad=1
fi
exit $bad
