#!/usr/bin/env bash
# Boot until one attempt wedges, then print what that boot printed last.
#
# repeat-boot.sh answers "how often", which stops being the useful question
# once the answer is "sometimes". Where the machine stopped is only in the
# serial log of the boot that failed, and a summary line drops it - so this
# keeps that log instead of overwriting it with the next attempt.
set -uo pipefail
cd "$(dirname "$0")/../.."

d="${1:-build-gcc-Release}"
n="${2:-8}"

for i in $(seq 1 "$n"); do
    python3 scripts/qemu-cli-smoke-linux.py "$d" 300 > /dev/null 2>&1
    r=$(grep -o '^reason=.*' qemu-cli-summary.txt | head -1)
    case "$r" in
        *cli_and_network_verified*)
            echo "boot $i: pass"
            ;;
        *)
            echo "boot $i: WEDGED $r"
            cp qemu-cli-serial.log wedge-serial.log
            echo "--- last 30 lines before the silence ---"
            tr -d '\r' < wedge-serial.log | tail -30
            exit 1
            ;;
    esac
done
echo "no wedge in $n attempts"
