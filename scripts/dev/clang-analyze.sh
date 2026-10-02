#!/usr/bin/env bash
# clang's static analyzer over the portable kernel, one file at a time.
#
#     scripts/dev/clang-analyze.sh [--sarif <dir>] [file.c ...]
#
# Path-sensitive: it follows a value down a branch, which is what finds a
# pointer checked in one arm and used in the other, a read of a variable one
# path never set, a division one path leaves at zero. CodeQL reports patterns
# across the tree; this reports paths through one function. They overlap less
# than their names suggest.
#
# Without arguments it analyses every C file under kernel/ except the x86-64
# architecture, whose inline assembly and fixed addresses are most of what it
# would report. Prints each finding as file:line: warning, then
# `clang-analyze=ok` or `clang-analyze=FAIL findings=<n>`; with --sarif, also
# writes one SARIF file per source for code scanning to take.
#
# It was admitted the way CodeQL was (.github/workflows/codeql.yml): by
# planting a fault and seeing it reported. See the note in
# docs/implementation_progress/ about what it caught and what it did not.
set -u
cd "$(dirname "$0")/../.." || exit 1

sarif=""
if [ "${1:-}" = "--sarif" ]; then
    sarif="${2:?--sarif needs a directory}"
    mkdir -p "$sarif"
    shift 2
fi
if [ $# -gt 0 ]; then
    files="$*"
else
    files=$(find kernel -name '*.c' -not -path 'kernel/arch/*' | sort)
fi

CC=${CLANG:-clang}
command -v "$CC" > /dev/null || { echo "clang-analyze=FAIL reason=no_clang"; exit 2; }

# Findings that have been read and are not defects, one a line as
# "<file>: <checker>: <why>". Matched by file and checker, not by line, so an
# edit above one does not bring it back; a second finding of the same checker
# in the same file is new and is reported.
known=scripts/dev/clang-analyze-known.txt

n=0
out=$(mktemp)
for f in $files; do
    # unix.* is off: there is no malloc, no FILE and no fork in a kernel, and
    # its API checkers have nothing to say about ours. core, deadcode and
    # security stay.
    "$CC" --analyze -std=c11 -Iinclude -Ikernel -Ikernel/abi/linux \
        -Xclang -analyzer-disable-checker -Xclang unix \
        -Xclang -analyzer-output=text -o /dev/null "$f" > "$out" 2>&1
    if [ -n "$sarif" ]; then
        "$CC" --analyze -std=c11 -Iinclude -Ikernel -Ikernel/abi/linux \
            -Xclang -analyzer-disable-checker -Xclang unix \
            -Xclang -analyzer-output=sarif \
            -o "$sarif/$(echo "$f" | tr '/' '_').sarif" "$f" > /dev/null 2>&1
    fi
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        checker=$(echo "$line" | sed -n 's/.*\[\([a-zA-Z0-9_.]*\)\]$/\1/p')
        if [ -f "$known" ] && [ "$(grep -c "^$f: $checker: " "$known")" -ge 1 ] &&
           [ "$(grep ': warning: ' "$out" | grep -c "\[$checker\]\$")" -le "$(grep -c "^$f: $checker: " "$known")" ]; then
            continue
        fi
        echo "$line"
        n=$((n + 1))
    done < <(grep ': warning: ' "$out")
    if grep -q ': error: ' "$out"; then
        grep -m2 ': error: ' "$out"
        echo "clang-analyze=FAIL reason=does_not_compile:$f"
        rm -f "$out"
        exit 2
    fi
done
rm -f "$out"
if [ "$n" -eq 0 ]; then
    echo "clang-analyze=ok files=$(echo "$files" | wc -w)"
    exit 0
fi
echo "clang-analyze=FAIL findings=$n"
exit 1
