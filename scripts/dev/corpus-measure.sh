#!/usr/bin/env bash
# Measure which syscalls each corpus workload makes, on the host, under strace.
#
#   scripts/dev/corpus-measure.sh [build-dir] [out-dir]
#
# Writes <out-dir>/<workload>.txt (default tests/corpus/needs/): the sorted set
# of syscall names the program asked for, including the ones Linux itself
# answered with ENOSYS - a program that asks is a program that needs an answer.
# scripts/dev/corpus-report.py turns them into docs/abi/corpus.md.
#
# The set is a property of the program and its C library, not of the kernel.
# That is why it is measured on Linux and checked in: the nightly measures again
# and says when a program's needs have changed underneath the plan.
set -uo pipefail
cd "$(dirname "$0")/../.."
ROOT=$(pwd)
BUILD=${1:-build-gcc-Release}
OUT=${2:-tests/corpus/needs}
case "$BUILD" in /*) ;; *) BUILD="$ROOT/$BUILD" ;; esac
case "$OUT" in /*) ;; *) OUT="$ROOT/$OUT" ;; esac

command -v strace > /dev/null || { echo "corpus-measure: strace not installed"; exit 2; }
BUSYBOX=$(command -v busybox) || { echo "corpus-measure: busybox not installed"; exit 2; }
mkdir -p "$OUT"

fixture() {
    printf 'hello from a file\nsecond line with hello\n' > note.txt
    mkdir -p dir/sub
    printf 'hello\n' > dir/a.txt
    printf 'world\n' > dir/sub/b.txt
    ln -sf a.txt dir/link
}

measured=0
failed=0
while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    name=$(printf '%s' "${line%%|*}" | tr -d ' ')
    cmd=${line#*|}
    cmd=${cmd# }
    cmd=${cmd//\$BUSYBOX/$BUSYBOX}
    cmd=${cmd//\$BUILD/$BUILD}
    cmd=${cmd//\$CORPUS/$BUILD/corpus}

    work=$(mktemp -d)
    (
        cd "$work" || exit 1
        fixture
        # eval only splits the line into argv honouring its quotes; strace then
        # runs that argv directly. No host shell runs the workload itself.
        eval "set -- $cmd"
        if [ ! -x "$1" ]; then
            echo "corpus-measure: $name: $1 is not there - skipped" >&2
            exit 3
        fi
        strace -f -qq -o trace.txt "$@" > /dev/null 2>&1 < /dev/null
        # "1234 openat(" and "1234 <... openat resumed>" both name a call.
        sed -nE 's/^[0-9]+ +(<\.\.\. )?([a-z_][a-z0-9_]*)[( ].*/\2/p' trace.txt \
            | sort -u > "$OUT/$name.txt"
        # The system files it opens, which a syscall list cannot show: `ps`
        # needs no syscall VibeOS lacks and still fails without /proc. Process
        # ids are folded so the list does not change from run to run.
        grep -oE '"/(proc|sys|dev|etc)/[^"]*"' trace.txt \
            | tr -d '"' | sed -E 's#/proc/[0-9]+#/proc/<pid>#; s#/(task)/[0-9]+#/\1/<tid>#' \
            | sort -u > "$OUT/$name.files"
        [ -s "$OUT/$name.files" ] || rm -f "$OUT/$name.files"
    )
    rc=$?
    rm -rf "$work"
    if [ $rc -eq 0 ] && [ -s "$OUT/$name.txt" ]; then
        measured=$((measured + 1))
        printf '%-18s %3d syscalls\n' "$name" "$(wc -l < "$OUT/$name.txt")"
    else
        failed=$((failed + 1))
    fi
done < "$ROOT/tests/corpus/workloads.txt"

echo "corpus-measure: measured=$measured skipped=$failed out=$OUT"
[ $measured -gt 0 ]
