#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The blocks an ext4 volume keeps for the superuser (review v10, M-159).
#
#   scripts/dev/verify-ext4-reserved.sh [build-dir]
#
# mkfs.ext4 -m 20 keeps a fifth of an 8 MiB volume for root. A writer who is
# not root fills the volume until ENOSPC, and statfs must then say nothing is
# available to it; root must still be able to write a block, out of the
# reserve; e2fsck must call the volume clean. Before M-159 the reserve went to
# anybody, so the unprivileged fill took it all and root's write was refused.
#
# Prints ext4_reserved=ok, =skip reason=..., or ext4_reserved=FAILED reason=...

set -u
B="${1:-build-gcc-Release}"
cd "$(dirname "$0")/../.." || exit 2
EX="$(cd "$B" && pwd)/vibeos_ext4_exercise"

fail() {
    echo "  $1"
    echo "ext4_reserved=FAILED reason=$2"
    exit 1
}
for tool in mkfs.ext4 e2fsck; do
    command -v "$tool" > /dev/null 2>&1 || { echo "ext4_reserved=skip reason=no_$tool"; exit 0; }
done
[ -x "$EX" ] || fail "no exerciser at $EX" exercise_not_built

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
truncate -s 8M "$T/fs.img"
mkfs.ext4 -q -F -m 20 -O metadata_csum "$T/fs.img" > "$T/mkfs.log" 2>&1 || fail "$(head -1 "$T/mkfs.log")" mkfs_failed

# One script line per command, its fields separated by tabs, as ext4-tree.py
# writes them.
line() {
    local IFS=$'\t'
    echo "$*" >> "$T/script.txt"
}
: > "$T/script.txt"
line create /user 644
line create /root 644
line unprivileged
line fillup /user 7
line avail zero
line privileged
line write /root 0 4096 9

(cd "$T" && "$EX" apply) > "$T/apply.log" 2>&1 || fail "$(grep -m1 FAIL "$T/apply.log")" apply_failed
e2fsck -fn "$T/fs.img" > "$T/fsck.log" 2>&1 || fail "$(head -3 "$T/fsck.log" | tr '\n' ' ')" fsck_unclean
echo "ext4_reserved=ok $(grep -o 'fillup size=[0-9]*' "$T/apply.log")"
