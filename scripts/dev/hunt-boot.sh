#!/usr/bin/env bash
# Boot until a signature appears in the serial log, and keep every log worth
# reading.
#
# repeat-boot.sh answers "how often", which stops being the useful question
# once the answer is "sometimes". Every hard bug here was closed by one log
# with the signature in it - the four-worker poison panic, the rmap mismatch,
# the XMM corruption - and each hunt was written as its own throwaway loop with
# its own grep: this is that loop, once.
#
# A boot is kept when the signature appears (even on a boot that passed - a
# detector event on a green boot is the evidence) and when the boot failed for
# any reason at all: the first argv hunt kept only signature hits and threw two
# failures of another kind away unread, breaking this project's rule about
# evidence in the script written to gather it.
#
# Usage: hunt-boot.sh <build-dir> <boots> <extended-regex> [--stop]
#   --stop   end at the first boot whose log matches the signature
#
# Examples:
#   hunt-boot.sh build-clang-Release 24 'dead0000dead0000|POISON_BROKEN' --stop
#   hunt-boot.sh build-gcc-Release 12 'RING3_WRITE_NUL|at=argv:'
#
# Logs land in .boot-evidence/hunt/<timestamp>-bNN.{log,summary}.
set -u
cd "$(dirname "$0")/../.." || exit 1

if [ $# -lt 3 ]; then
    sed -n '2,/^set -u/p' "$0" | sed -e 's/^# \{0,1\}//' -e '/^set -u/d'
    exit 2
fi
dir="$1"
n="$2"
sig="$3"
stop=0
[ "${4:-}" = "--stop" ] && stop=1

run=$(date +%Y%m%d-%H%M%S)
out=".boot-evidence/hunt"
mkdir -p "$out"
hits=0
fails=0
for i in $(seq 1 "$n"); do
    python3 scripts/qemu-cli-smoke-linux.py "$dir" 300 > /dev/null 2>&1
    st=$(head -1 qemu-cli-summary.txt)
    reason=$(grep -o '^reason=.*' qemu-cli-summary.txt | head -1 | cut -c1-110)
    matches=$(grep -acE "$sig" qemu-cli-serial.log 2>/dev/null)
    matches=${matches:-0}
    failed=0
    case "$st" in *status=pass*) ;; *) failed=1 ;; esac
    tag=$(printf '%s-b%02d' "$run" "$i")
    if [ "$matches" -gt 0 ] || [ "$failed" -eq 1 ]; then
        cp qemu-cli-serial.log "$out/$tag.log"
        cp qemu-cli-summary.txt "$out/$tag.summary"
        echo "boot$i matches=$matches failed=$failed $reason -> $out/$tag.log"
        grep -aE "$sig" qemu-cli-serial.log | head -3 | cut -c1-200 | sed 's/^/        /'
    else
        echo "boot$i clean"
    fi
    [ "$matches" -gt 0 ] && hits=$((hits + 1))
    [ "$failed" -eq 1 ] && fails=$((fails + 1))
    if [ "$stop" -eq 1 ] && [ "$matches" -gt 0 ]; then
        echo "signature at boot$i; stopping"
        break
    fi
done
echo "---- $i boots: signature in $hits, failed $fails"
