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
# kernel, so it is worth re-running whenever either changes. For the programs
# the plan is measured against, tests/corpus/ does this for every workload and
# docs/abi/corpus.md is the result; this is the same measurement for one.
#
# Until 2026-09-29 this said "(nothing - this binary's syscall set is covered)"
# for every binary: it looked for the kernel's handlers as `case LSYS_` in
# arch_hw.c, which C4 replaced with table rows, and its name extraction had
# stopped matching strace's output - so both lists came back empty and it
# reported full coverage. A tool that cannot say "missing" is not measuring.
# It now reads the state of each syscall from docs/abi/syscalls.md.
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
case "$bin" in /*) ;; *) [ -e "$bin" ] && bin="$ROOT/$bin" ;; esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
echo "hello from a file" > note.txt

echo "=== $bin $*"
readelf -h "$bin" 2>/dev/null | grep -E 'Type|Entry'
stat -c 'size=%s bytes' "$bin"

strace -f -qq -o trace.txt "$bin" "$@" > /dev/null 2>&1 < /dev/null
sed -nE 's/^[0-9]+ +(<\.\.\. )?([a-z_][a-z0-9_]*)[( ].*/\2/p' trace.txt > names.txt
sort -u names.txt > used.txt
[ -s used.txt ] || { echo "no syscalls parsed from strace's output - the tool is broken, not the program"; exit 1; }

echo "=== syscalls used, in order of first appearance"
awk '!seen[$0]++' names.txt | tr '\n' ' '
echo

# name state phase, from the generated table
sed -nE 's/^\| [0-9]+ \| `([a-z_0-9]+)` \| ([a-z]+) \| ([^|]+) \|.*/\1 \2 \3/p' \
    "$ROOT/docs/abi/syscalls.md" | sed 's/ *$//' > table.txt

for state in done partial missing; do
    echo "=== $state"
    awk -v st="$state" 'NR==FNR { s[$1]=$2; p[$1]=$3; for (i = 4; i <= NF; i++) p[$1]=p[$1]" "$i; next }
         ($1 in s) && s[$1]==st { printf "%s%s ", $1, (st=="done" ? "" : "(" p[$1] ")") }' \
        table.txt used.txt
    echo
done
unknown=$(awk 'NR==FNR { s[$1]=1; next } !($1 in s) { printf "%s ", $1 }' table.txt used.txt)
[ -n "$unknown" ] && echo "=== not in docs/abi/syscalls.md: $unknown"
missing=$(awk 'NR==FNR { s[$1]=$2; next } s[$1]=="missing"' table.txt used.txt | wc -l)
echo "=== missing=$missing"
