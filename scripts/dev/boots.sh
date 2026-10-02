#!/usr/bin/env bash
# Boot N times at once and report every verdict.
#
#   scripts/dev/boots.sh [build-dir] [count] [parallel]
#
# repeat-boot.sh runs them one after another, which at about ninety seconds
# each makes any question about a failure that reproduces one time in four a
# question you ask twice a day rather than twice an hour. These run together:
# each gets its own echo port and its own log files, so the only thing they
# share is the machine.
#
# Parallelism defaults to half the cores, because each boot is a four-vCPU
# guest and oversubscribing turns a timing bug into a scheduling artefact -
# which is worse than slow, since it looks exactly like the bug being hunted.
set -uo pipefail
cd "$(dirname "$0")/../.."

d="${1:-build-gcc-Release}"
n="${2:-4}"
# Measured on eight host threads (2026-10-02): six boots, three at a time, in
# four minutes - forty seconds a boot against about a hundred one after another.
par="${3:-$(( ($(nproc) + 2) / 3 ))}"
# A healthy boot reaches the shell in about ninety seconds, so this is a little
# over the worst honest case and not the five minutes the gate allows. It bounds
# the one failure the per-phase quiet budget cannot catch: a guest that keeps
# talking without ever getting anywhere.
# 180, not 90: the first boot of a sweep reads OVMF and the images off a cold
# host cache and is markedly slower than the rest. At 90 it failed reliably in
# the bootloader phase, and that looked like a bootloader defect for a while.
# It was this number.
# 300 since docs/abi/ L3: the boot runs the corpus and five ring-3 self-tests
# now, and three at once on eight host threads take about two minutes each.
budget="${4:-300}"
[ "$par" -lt 1 ] && par=1
[ "$par" -gt "$n" ] && par="$n"

echo "booting $n times, $par at a time, ${budget}s budget each"

run_one() {
    local id="$1"
    VIBEOS_SMOKE_ID="$id" python3 scripts/qemu-cli-smoke-linux.py "$d" "$budget" \
        > /dev/null 2>&1
    local sfx=""
    [ "$id" != "0" ] && sfx="-$id"
    local r
    r=$(grep -o '^reason=.*' "qemu-cli-summary${sfx}.txt" 2>/dev/null | head -1)
    case "$r" in
        *cli_and_network_verified*) echo "boot $id: pass" ;;
        *) echo "boot $id: FAIL $r"
           # Keep the log of the boot that failed. The next run with this id
           # overwrites it, and the failing one is the only one worth reading.
           cp "qemu-cli-serial${sfx}.log" "wedge-serial-${id}.log" 2>/dev/null
           cp "qemu-cli-summary${sfx}.txt" "wedge-summary-${id}.txt" 2>/dev/null
           ;;
    esac
}

results=$(mktemp)
running=0
for i in $(seq 1 "$n"); do
    run_one "$i" | tee -a "$results" &
    running=$((running + 1))
    if [ "$running" -ge "$par" ]; then
        wait -n 2>/dev/null || wait
        running=$((running - 1))
    fi
done
wait

echo "clean=$(grep -c ': pass$' "$results" 2>/dev/null || echo 0)/$n"
rm -f "$results"
echo "--- logs of any failures are in wedge-serial-<id>.log ---"
