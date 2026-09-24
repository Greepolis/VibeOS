#!/usr/bin/env bash
# Ask a guest that is stuck right now what every core is doing.
#
# catch-hang.py and until-wedge.sh boot their own guests. This is for the one
# already running - a boot gate or a repeat-boot run that has gone quiet in
# another terminal - and it has to be asked before whatever is driving it gives
# up and kills QEMU, after which there is nothing left to ask. It uses the
# monitor socket the boot gate opens and wedge_report.py, which the gate itself
# runs on every wedge: per-core RIP and CR3 and a frame walk, symbolised.
#
# Usage: wedge-now.sh [outfile] [build-dir]
set -u
cd "$(dirname "$0")/../.." || exit 1

out="${1:-.boot-evidence/wedge-now-$(date +%Y%m%d-%H%M%S).txt}"
dir="${2:-build-gcc-Release}"
# The gate names the socket vibeos-monitor.sock, or vibeos-monitor-<N>.sock for
# a run with a SMOKE_ID; the newest is the guest most likely to be the stuck one.
# Say VIBEOS_MONITOR=<path> to pick another.
sock="${VIBEOS_MONITOR:-$(ls -t /tmp/vibeos-monitor*.sock 2>/dev/null | head -1)}"

if [ -z "$sock" ] || [ ! -S "$sock" ]; then
    echo "no monitor socket under /tmp/vibeos-monitor*.sock - is a guest running under the boot gate?"
    exit 1
fi
echo "monitor=$sock"
kernel=$(find "$dir" -maxdepth 3 -type f -name 'vibeos_kernel' 2>/dev/null | head -1)
if [ -z "$kernel" ]; then
    echo "no vibeos_kernel under $dir to symbolise against"
    exit 1
fi
mkdir -p "$(dirname "$out")"
echo "kernel=$kernel"
timeout 90 python3 scripts/dev/wedge_report.py "$sock" "$kernel" > "$out" 2>&1
echo "rc=$? -> $out ($(wc -l < "$out") lines)"
head -70 "$out" | cut -c1-200
