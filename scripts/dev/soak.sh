#!/usr/bin/env bash
# The soak property of the memory manager's P7.
#
# The boot gate already compares frames free on the way into userland against
# frames free on the way out, and allows a ceiling of 64. A boot leaves about
# twenty-six unaccounted for. What that single number cannot say is whether
# twenty-six is a *fixed* cost paid once at startup or a *per-round* leak that
# a 120-round run happens to make look small - and those are the same figure
# with completely different consequences on a machine that stays up.
#
# So this runs the stress service at two round counts and prints both losses.
# One measurement proves nothing here; the comparison is the whole test:
#
#   fixed cost      -> lost stays about the same when rounds go up 100x
#   per-round leak  -> lost grows with the rounds, and 12000 makes it obvious
#
# Usage: bash scripts/dev/soak.sh [build-dir] [low-rounds] [high-rounds]
#
# It reconfigures the build directory, so do not run it while a repeat-boot is
# in flight - CLAUDE.md has that rule and it has invalidated three runs.
set -u

build="${1:-build-gcc-Release}"
low="${2:-120}"
high="${3:-12000}"

run_at() {
    rounds="$1"
    cmake -S . -B "$build" -DVIBEOS_STRESS_ROUNDS="$rounds" >/dev/null 2>&1
    if ! bash scripts/dev/check.sh build "$build" 2>&1 | grep -q '^rc=0'; then
        echo "rounds=$rounds BUILD FAILED"
        return 1
    fi
    # A generous budget: 12000 rounds of fork and copy-on-write is not fast
    # under TCG, and a timeout here would read as a wedge.
    # The budget is the gate's second argument, not an environment variable.
    python3 scripts/qemu-cli-smoke-linux.py "$build" 1800 >/dev/null 2>&1
    log=qemu-cli-serial.log
    ok="$(grep -a -o 'STRESS_OK rounds=[0-9]*' "$log" | tail -1)"
    reason="$(grep -a -o 'reason=[^ ]*' qemu-cli-summary.txt | tail -1)"
    python3 - "$log" "$rounds" "$ok" "$reason" <<'PY'
import re, sys
text = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
s = re.search(r'FRAMES_AT_USERLAND_START=0x([0-9a-f]{16})', text)
d = re.search(r'FRAMES_AT_USERLAND_DONE=0x([0-9a-f]{16}) '
              r'cache_resident=0x([0-9a-f]{16})', text)
if not s or not d:
    print(f'rounds={sys.argv[2]} accounting missing  {sys.argv[4]}')
    raise SystemExit(0)
lost = int(s.group(1), 16) - (int(d.group(1), 16) + int(d.group(2), 16))
print(f'rounds={sys.argv[2]:>6} lost={lost:<6} {sys.argv[3] or "STRESS DID NOT FINISH"}'
      f'  {sys.argv[4]}')
PY
}

echo "soak: comparing the frame loss at $low rounds against $high"
run_at "$low"
run_at "$high"
# Left as the tree found it, so the next check.sh does not silently measure a
# soak build.
cmake -S . -B "$build" -DVIBEOS_STRESS_ROUNDS= >/dev/null 2>&1
echo "soak: build dir restored to the default round count"
