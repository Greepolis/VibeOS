# Full verification. Exits non-zero if anything is red, because a summary line
# saying FAIL above "exited with code 0" is a line you learn not to read - the
# same trap as check.sh's rc=, wearing a wrapper.
cd /mnt/c/Users/Stefa/Documents/Progetti/VibeOS
bad=0
out=$(bash scripts/dev/check.sh all build-clang-Release 2>&1)
echo "$out" | grep -E '^rc=|^warnings=|^clang-|^host-tests|^bootloader-tests|assertions-covered|counters-produced|mm-layering|nightly-coverage|reachable=|chokepoints=|rmap-crosscheck=|ALL_TESTS|error:'
echo "$out" | grep -qE '^rc=0$'          || bad=1
echo "$out" | grep -qE '^clang-rc=0$'    || bad=1
echo "$out" | grep -qE '^warnings=0$'    || bad=1
echo "$out" | grep -qE '^clang-warnings=0$' || bad=1
echo "$out" | grep -qE '^host-tests=pass' || bad=1
echo "$out" | grep -qE '^bootloader-tests=pass' || bad=1
for i in 1 2 3; do
  python3 scripts/qemu-cli-smoke-linux.py build-clang-Release 300 >/dev/null 2>&1
  line=$(head -1 qemu-cli-summary.txt)
  echo "  boot$i $line"
  case "$line" in *status=pass*) ;; *) bad=1 ;; esac
done
echo "VERDICT=$([ $bad -eq 0 ] && echo green || echo RED)"
exit $bad
