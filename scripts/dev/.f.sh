# Full verification. Exits non-zero if anything is red, because a summary line
# saying FAIL above "exited with code 0" is a line you learn not to read - the
# same trap as check.sh's rc=, wearing a wrapper.
cd /mnt/c/Users/Stefa/Documents/Progetti/VibeOS
bad=0
out=$(bash scripts/dev/check.sh all build-clang-Release 2>&1)
echo "$out" | grep -E '^rc=|^warnings=|^clang-|^fuzz-rc|^host-tests|^bootloader-tests|assertions-covered|counters-produced|mm-layering|nightly-coverage|reachable=|chokepoints=|syscall-checks=|sabotage-anchors=|subsystem=|blast-radius=|mustbezero-asserted=|rmap-crosscheck=|task-identity=|exec-layering=|user-access=|net-stable=|ALL_TESTS|error:'
echo "$out" | grep -qE '^rc=0$'          || bad=1
echo "$out" | grep -qE '^clang-rc=0$'    || bad=1
echo "$out" | grep -qE '^warnings=0$'    || bad=1
echo "$out" | grep -qE '^clang-warnings=0$' || bad=1
echo "$out" | grep -qE '^host-tests=pass' || bad=1
echo "$out" | grep -qE '^bootloader-tests=pass' || bad=1
# subsystem= can print "skip" when the build has no objects, and a skip that
# reads like a pass is the whole reason these lines are asserted rather than
# printed. Require ok, not not-FAIL.
echo "$out" | grep -qE 'subsystem=ok'    || bad=1
echo "$out" | grep -qE 'blast-radius=ok' || bad=1
echo "$out" | grep -qE 'mustbezero-asserted=ok' || bad=1
# Printed and never asserted, until the uaccess change moved a chokepoint count
# and this script still said VERDICT=green. Every check it prints, it asserts.
# task-identity and exec-layering were printed by check.sh and missing from this
# list - the same gap as the comment above describes, found by an external review
# (2026-09-24); user-access and net-stable arrived after the list was written.
# fuzz-rc likewise: the fuzz build is the one CI job whose failure nothing here saw.
echo "$out" | grep -qE '^fuzz-rc=0$' || bad=1
for k in mm-layering assertions-covered counters-produced nightly-coverage reachable rmap-crosscheck chokepoints syscall-checks sabotage-anchors task-identity exec-layering user-access net-stable; do
  echo "$out" | grep -qE "^$k=ok" || bad=1
done
# A failed boot's log is kept, named for the run. This loop used to print only
# the status line, and the next boot overwrote qemu-cli-serial.log - so a
# failure in boot 1 of the H-011 check left nothing to read at all.
RUN=$(date +%Y%m%d-%H%M%S)
mkdir -p .boot-evidence
for i in 1 2 3; do
  python3 scripts/qemu-cli-smoke-linux.py build-clang-Release 300 >/dev/null 2>&1
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
