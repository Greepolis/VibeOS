#!/usr/bin/env bash
# Which Linux syscalls does a real program actually use, and which of them does
# VibeOS not serve yet?
#
#   scripts/dev/trace-linux-binary.sh <binary> [args...]
#   scripts/dev/trace-linux-binary.sh /usr/bin/busybox ls -1
#
# This is the method behind every syscall in the kernel's Linux layer. Guessing
# at the list produces stubs nobody calls sitting next to gaps that stop
# everything; running the binary on Linux under strace produces the truth.
#
# The set is a property of the libc and of what the program does, not of the
# kernel, so it is worth re-running whenever either changes.
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)

if [ $# -lt 1 ]; then
    echo "usage: $0 <binary> [args...]"
    exit 2
fi
command -v strace > /dev/null || { echo "strace not installed"; exit 2; }

bin="$1"
shift

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
echo "hello from a file" > note.txt

echo "=== $bin $*"
readelf -h "$bin" | grep -E 'Type|Entry'
stat -c 'size=%s bytes' "$bin"

strace -f -o trace.txt "$bin" "$@" > /dev/null 2>&1
# -f prefixes each line with a pid, so strip that before taking the name.
strip_names() {
    sed 's/^[0-9][0-9]* *//; s/(.*//' trace.txt \
        | grep -E '^[a-z_][a-z0-9_]*$' | grep -v '^unfinished$'
}
strip_names | sort -u > used.txt

echo "=== syscalls used, in order of first appearance"
strip_names | awk '!seen[$0]++' | tr '\n' ' '
echo

grep -oE 'case LSYS_[a-z0-9_]+' "$ROOT/kernel/arch/x86_64/arch_hw.c" \
    | sed 's/case LSYS_//' | sort -u > have.txt

echo "=== served by VibeOS"
comm -12 used.txt have.txt | tr '\n' ' '
echo
echo "=== MISSING"
missing=$(comm -23 used.txt have.txt | tr '\n' ' ')
echo "$missing"
[ -z "${missing// /}" ] && echo "(nothing - this binary's syscall set is covered)"
