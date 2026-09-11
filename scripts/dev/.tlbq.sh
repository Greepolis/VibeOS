cd /mnt/c/Users/Stefa/Documents/Progetti/VibeOS
# Twelve boots, keeping the evidence of every failure.
#
# Evidence goes beside the repo, NOT in /tmp: WSL cleans /tmp, and that has
# eaten the log of an intermittent failure, which cannot be summoned back.
#
# And every file is named for its run, not only for its boot number. The first
# version wrote fail-<n>.log, so the third boot of one series overwrote the third
# boot of the series before it - which is how the log of a `cr2 == rip` failure
# read that morning was replaced by an unrelated one in the afternoon.
OUT=/mnt/c/Users/Stefa/Documents/Progetti/VibeOS/.boot-evidence
RUN=$(date +%Y%m%d-%H%M%S)
mkdir -p "$OUT"
pass=0; fail=0
for i in $(seq 1 12); do
  python3 scripts/qemu-cli-smoke-linux.py build-clang-Release 300 >/dev/null 2>&1
  r=$(grep -o 'reason=[^ ]*' qemu-cli-summary.txt | head -1)
  line=$(head -1 qemu-cli-summary.txt)
  case "$line" in
    *status=pass*) pass=$((pass+1)) ;;
    *) fail=$((fail+1))
       cp qemu-cli-serial.log "$OUT/fail-$RUN-boot$i.log"
       cp qemu-cli-summary.txt "$OUT/fail-$RUN-boot$i.summary"
       echo "  FAIL $i $r  (kept: .boot-evidence/fail-$RUN-boot$i.log)" ;;
  esac
  echo "  boot$i $r $(grep -o 'tlbq_overflow=0x[0-9a-f]*' qemu-cli-serial.log | tail -1)"
done
echo "BOOTS pass=$pass fail=$fail run=$RUN"
