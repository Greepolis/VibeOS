#!/usr/bin/env bash
# Phase X-P4 of docs/exec/: the interpreter substitution is one function.
#
# A dynamic program asks for /lib/ld-musl-x86_64.so.1 and the boot volume is
# FAT, which has neither that directory nor a name that long. The kernel
# translates the request. That is a stand-in for a filesystem layout, and the
# danger with a stand-in is not that it exists - it is that it breeds: a second
# hard-coded path appears somewhere else, the two disagree, and the substitution
# stops being something anyone can find, reason about, or delete.
#
# So the rule is checked by the build rather than by review. Exactly one
# function may name an interpreter path, it says in its own comment that it is
# temporary, and anything outside it fails here. If the filesystem layout ever
# becomes real, that function is deleted rather than generalised, and this
# script goes with it.
set -u

src=kernel/arch/x86_64/arch_hw.c
fail=0

# The function's line range: from its signature to the first line that closes a
# definition at column zero. Deliberately crude - it only has to bound one small
# function, and something cleverer would be one more thing to be wrong.
first=$(grep -n 'hw_interp_path_substitute(const char \*path)' "$src" | head -1 | cut -d: -f1)
if [ -z "$first" ]; then
    echo "exec-layering=FAIL hw_interp_path_substitute is gone; the substitution moved somewhere"
    exit 0
fi
# The window starts at the top of the comment block attached to the signature,
# found by walking up to the nearest blank line rather than by guessing a number
# of lines. A magic lookback was wrong within the hour it was written: an
# unrelated function was added above, the comment moved, and the check failed
# for a reason that had nothing to do with what it guards.
lo=$(awk -v s="$first" 'NR<s && $0 ~ /^[[:space:]]*$/ { last=NR } END { print (last ? last+1 : 1) }' "$src")
hi=$(awk -v s="$first" 'NR>s && /^}/ { print NR; exit }' "$src")
: "${hi:=$((first + 40))}"

bad=0
while IFS= read -r hit; do
    [ -z "$hit" ] && continue
    file=${hit%%:*}
    rest=${hit#*:}
    line=${rest%%:*}
    if [ "$file" = "$src" ] && [ "$line" -ge "$lo" ] && [ "$line" -le "$hi" ]; then
        continue    # inside the one function that is allowed to know
    fi
    if [ "$bad" -eq 0 ]; then
        echo "exec-layering=FAIL an interpreter path is named outside hw_interp_path_substitute"
    fi
    bad=$((bad + 1))
    [ "$bad" -le 5 ] && echo "  $hit"
done <<EOF
$(grep -rn 'ld-musl\|LDMUSL\|ld-linux\|/lib64/' kernel/ --include=*.c || true)
EOF

[ "$bad" -gt 0 ] && fail=1

# And it must still admit what it is. A stand-in that stops saying so is how one
# becomes permanent.
if ! sed -n "${lo},${hi}p" "$src" | grep -q 'stand-in'; then
    echo "exec-layering=FAIL the substitution no longer says it is a stand-in"
    fail=1
fi

if [ "$fail" -eq 0 ]; then
    echo "exec-layering=ok"
fi
exit 0
