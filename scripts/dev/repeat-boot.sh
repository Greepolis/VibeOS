#!/usr/bin/env bash
# Boot N times and report every distinct verdict.
#
#   scripts/dev/repeat-boot.sh [build-dir] [count]
#
# One clean boot proves very little. Several of the worst bugs in this kernel -
# a lost TLS base, two CPUs on one task, a console deadlock - reproduced in
# roughly one boot in three, so a single green run and a broken kernel look
# exactly alike.
set -uo pipefail
cd "$(dirname "$0")/../.."

d="${1:-build-gcc-Release}"
n="${2:-6}"

pass=0
for i in $(seq 1 "$n"); do
    python3 scripts/qemu-cli-smoke-linux.py "$d" 300 > /dev/null 2>&1
    r=$(grep -o '^reason=.*' qemu-cli-summary.txt | head -1)
    v=$(grep -aoE 'tls survived[^\r]*|tls lost[^\r]*|abi: [^\r]*|auxv wrong|MUSL_OK|action=PANIC' \
        qemu-cli-serial.log | sort -u | tr '\n' ';')
    echo "boot $i: $r | $v"
    case "$r" in
        *cli_and_network_verified*)
            pass=$((pass + 1))
            ;;
        *)
            # Keep the log of every boot that failed.
            #
            # This did not, and a forty-eight boot run that found a defect in
            # twenty-one of them left nothing behind but the log of the last
            # boot - which had passed. The whole run had to be repeated to see
            # what had already been seen twenty-one times. An intermittent
            # failure is expensive to reproduce and free to save.
            cp qemu-cli-serial.log "repeat-fail-$i-serial.log" 2>/dev/null
            cp qemu-cli-summary.txt "repeat-fail-$i-summary.txt" 2>/dev/null
            ;;
    esac
done
echo "clean=$pass/$n"
if [ "$pass" -lt "$n" ]; then
    echo "failed boots kept as repeat-fail-*-serial.log"
fi
