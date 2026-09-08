#!/bin/bash
# Boot until the argv defect shows, and keep the log when it does.
#
# The failure is about one boot in six and every earlier investigation of it
# started by re-running and comparing counts, which this project has a rule
# against. What this script does instead is keep the evidence: a boot whose
# serial log contains an argv refusal is copied out before the next boot
# overwrites it, along with the GUI ownership line that says whether the
# desktop's frames were shared or lost.
#
# Usage: hunt-argv.sh [build-dir] [boots]
set -u
dir="${1:-build-clang-Release}"
n="${2:-8}"
out="argv-hunt"
mkdir -p "$out"
hits=0
for i in $(seq 1 "$n"); do
    python3 scripts/qemu-cli-smoke-linux.py "$dir" 900 >/dev/null 2>&1
    st=$(head -1 qemu-cli-summary.txt)
    gui=$(grep -a 'MUSTBEZERO guard_broken' qemu-cli-serial.log)
    argv=$(grep -ac 'at=argv:' qemu-cli-serial.log)
    echo "boot$i $st argv_refusals=$argv"
    echo "        $gui"
    # Every failure is kept, not only the argv ones.
    #
    # The first version of this script kept a log only when it found an argv
    # refusal, so a sweep that produced two failures of another kind discarded
    # both before anybody read them - which is this project's own rule about
    # not destroying evidence, broken by the script written to gather it.
    if [ "$argv" != "0" ] || [ "$st" != "status=pass" ]; then
        [ "$argv" != "0" ] && hits=$((hits + 1))
        cp qemu-cli-serial.log "$out/serial-$i.log"
        cp qemu-cli-summary.txt "$out/summary-$i.txt"
        sed -n 2p qemu-cli-summary.txt | cut -c1-200
        grep -a 'at=argv:' qemu-cli-serial.log | head -2
        echo "        kept in $out/serial-$i.log"
    fi
done
echo "argv_failures=$hits of $n"
