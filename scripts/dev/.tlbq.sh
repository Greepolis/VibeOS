cd /mnt/c/Users/Stefa/Documents/Progetti/VibeOS
# Evidence goes beside the repo, NOT in /tmp. CLAUDE.md records WSL cleaning
# /tmp out from under a sabotage run; the same thing has now eaten the log of
# an intermittent boot failure, which is worse - a case file can be rewritten,
# a one-in-twelve failure cannot be summoned back.
OUT=/mnt/c/Users/Stefa/Documents/Progetti/VibeOS/.boot-evidence
mkdir -p "$OUT"
pass=0; fail=0
for i in $(seq 1 12); do
  python3 scripts/qemu-cli-smoke-linux.py build-clang-Release 300 >/dev/null 2>&1
  r=$(grep -o 'reason=[^ ]*' qemu-cli-summary.txt | head -1)
  line=$(head -1 qemu-cli-summary.txt)
  case "$line" in
    *status=pass*) pass=$((pass+1)) ;;
    *) fail=$((fail+1))
       cp qemu-cli-serial.log "$OUT/fail-$i.log"
       cp qemu-cli-summary.txt "$OUT/fail-$i.summary"
       echo "  FAIL $i $r  (kept: .boot-evidence/fail-$i.log)" ;;
  esac
  echo "  boot$i $r $(grep -o 'tlbq_overflow=0x[0-9a-f]*' qemu-cli-serial.log | tail -1)"
done
echo "BOOTS pass=$pass fail=$fail"
