# Full verification. Exits non-zero if anything is red, because a summary line
# saying FAIL above "exited with code 0" is a line you learn not to read - the
# same trap as check.sh's rc=, wearing a wrapper.
#
# Usage: bash scripts/dev/.f.sh [build-dir]   (default build-clang-Release)
cd "$(dirname "$0")/../.." || exit 1
BUILD="${1:-build-clang-Release}"
bad=0
out=$(bash scripts/dev/check.sh all "$BUILD" 2>&1)
echo "$out" | grep -E '^rc=|^warnings=|^clang-|^fuzz-rc|^[a-z][a-z-]*=(ok|pass|FAIL|skip)|ALL_TESTS|error:'
echo "$out" | grep -qE '^rc=0$'          || bad=1
echo "$out" | grep -qE '^clang-rc=0$'    || bad=1
echo "$out" | grep -qE '^warnings=0$'    || bad=1
echo "$out" | grep -qE '^clang-warnings=0$' || bad=1
echo "$out" | grep -qE '^fuzz-rc=0$'     || bad=1
echo "$out" | grep -qE '^host-tests=pass' || bad=1
echo "$out" | grep -qE '^bootloader-tests=pass' || bad=1
# Any check that says FAIL is red, whether or not it is in the list below. The
# list used to be the whole of the assertion: it named exec-layering after that
# check was deleted, so this script could never say green, and it missed
# abi-layering, linux-layout and fat-mtools, so they could fail unseen
# (external review, 2026-10-07).
echo "$out" | grep -qE '^[a-z][a-z-]*=FAIL' && bad=1
# And the checks that must have run at all. A check that silently stopped
# printing would otherwise pass by absence. subsystem= can print "skip" when
# the build has no objects, and a skip that reads like a pass is the whole
# reason these are asserted ok rather than not-FAIL.
for k in subsystem blast-radius mustbezero-asserted mm-layering assertions-covered counters-produced \
         nightly-coverage reachable rmap-crosscheck chokepoints syscall-checks syscall-table \
         sabotage-anchors task-identity user-access net-stable abi-layering linux-layout fat-mtools; do
  echo "$out" | grep -qE "^$k=ok" || { echo "  missing or not ok: $k"; bad=1; }
done
# A failed boot's log is kept, named for the run. This loop used to print only
# the status line, and the next boot overwrote qemu-cli-serial.log - so a
# failure in boot 1 of the H-011 check left nothing to read at all.
RUN=$(date +%Y%m%d-%H%M%S)
mkdir -p .boot-evidence
for i in 1 2 3; do
  python3 scripts/qemu-cli-smoke-linux.py "$BUILD" 300 >/dev/null 2>&1
  line=$(head -1 qemu-cli-summary.txt)
  reason=$(grep -o 'reason=[^ ]*' qemu-cli-summary.txt | head -1)
  echo "  boot$i $line $reason"
  case "$line" in
    *status=pass*) ;;
    *) bad=1
       cp qemu-cli-serial.log ".boot-evidence/check-fail-$RUN-boot$i.log"
       cp qemu-cli-summary.txt ".boot-evidence/check-fail-$RUN-boot$i.summary"
       echo "  (kept: .boot-evidence/check-fail-$RUN-boot$i.log)" ;;
  esac
done
echo "VERDICT=$([ $bad -eq 0 ] && echo green || echo RED)"
exit $bad
