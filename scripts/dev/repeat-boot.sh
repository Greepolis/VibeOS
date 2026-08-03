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
    case "$r" in *cli_and_network_verified*) pass=$((pass + 1)) ;; esac
done
echo "clean=$pass/$n"
