#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The ext4 reader against volumes mkfs.ext4 made (docs/filesystem-design/ext4.md
# step 1).
#
#     scripts/dev/verify-ext4.sh [build-dir]
#
# The kernel's own host tests fabricate ext2 images in an array, which proves the
# driver agrees with the test's idea of the format - ISO9660 is the record of how
# little that can mean (CLAUDE.md). Here mkfs.ext4 makes the volume from a tree
# on the host, the driver reads all of it through vibeos_ext4_exercise, and the
# listing is held against the tree itself. e2fsck checks the volume first: a
# fixture nobody has checked is not believed.
#
# Then the refusals, on copies damaged with debugfs: a file whose inode
# checksum no longer matches is not read, and a volume claiming a feature the
# driver does not read is not mounted.
#
# Prints one verdict line last: ext4=ok, ext4=skip (no e2fsprogs) or
# ext4=FAILED reason=<why>.

B="${1:-build-gcc-Release}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

fail() {
    echo "  $1"
    echo "ext4=FAILED reason=$2"
    exit 1
}

for tool in mkfs.ext4 e2fsck debugfs; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "ext4=skip reason=no_$tool"
        exit 0
    fi
done
EX="$(cd "$B" && pwd)/vibeos_ext4_exercise"
[ -x "$EX" ] || fail "$EX missing" exercise_not_built

python3 scripts/dev/ext4-tree.py make "$T/tree" || fail "tree" tree_not_made
# 320 MiB is three groups, and 1024 inodes is about 340 a group: the tree's 619
# entries reach past the first group's inode table, so a descriptor other than
# group 0's is read.
truncate -s 320M "$T/fs.img"
mkfs.ext4 -q -F -N 1024 -d "$T/tree" "$T/fs.img" > "$T/mkfs.log" 2>&1 || fail "$(head -1 "$T/mkfs.log")" mkfs_failed
# mkfs.ext4 -d writes every directory linear; e2fsck -D rebuilds the large ones
# as htree, which is what a directory Linux grew would be. Its exit code is 1
# when it changed something, which here it must.
e2fsck -fyD "$T/fs.img" > "$T/fsckD.log" 2>&1
[ $? -le 1 ] || fail "$(head -3 "$T/fsckD.log" | tr '\n' ' ')" e2fsck_D_failed
e2fsck -fn "$T/fs.img" > "$T/fsck.log" 2>&1 || fail "$(head -3 "$T/fsck.log" | tr '\n' ' ')" fixture_not_clean

# The tree must have made the driver do what it is there for.
depth="$(debugfs -R "ex /sparse-forty-extents.bin" "$T/fs.img" 2>/dev/null | awk 'NR==2 {print $2}')"
case "$depth" in 0|"") fail "sparse file's tree has no index level (depth $depth)" fixture_has_no_index_level ;; esac
debugfs -R "htree /many" "$T/fs.img" 2>/dev/null | grep -q "Root node dump" \
    || fail "/many is not htree-indexed" fixture_has_no_htree

python3 scripts/dev/ext4-tree.py list "$T/tree" > "$T/want.txt"
"$EX" "$T/fs.img" walk > "$T/seen.raw" 2> "$T/walk.err" \
    || fail "$(grep -m1 FAIL "$T/walk.err" || tail -1 "$T/walk.err")" driver_walk_failed
sort "$T/seen.raw" > "$T/seen.txt"
if ! diff "$T/want.txt" "$T/seen.txt" > "$T/diff.txt"; then
    fail "$(head -4 "$T/diff.txt" | tr '\n' ' ')" listing_differs
fi
entries="$(grep -c . "$T/seen.txt")"

# An inode whose checksum no longer matches: its low half set to zero.
cp "$T/fs.img" "$T/csum.img"
debugfs -w -R "sif /small.txt checksum 0" "$T/csum.img" > /dev/null 2>&1
"$EX" "$T/csum.img" refused-file /small.txt > "$T/csum.out" 2> "$T/csum.err" \
    || fail "$(cat "$T/csum.err" "$T/csum.out" | head -1)" bad_inode_checksum_read

# A feature this driver does not read: inline_data puts a file in its inode.
cp "$T/fs.img" "$T/feat.img"
debugfs -w -R "feature inline_data" "$T/feat.img" > /dev/null 2>&1
"$EX" "$T/feat.img" refused-mount > "$T/feat.out" 2> "$T/feat.err" \
    || fail "$(cat "$T/feat.err" "$T/feat.out" | head -1)" unknown_feature_mounted
grep -q "features=0x8000" "$T/feat.out" || fail "$(cat "$T/feat.out")" refusal_names_wrong_feature

echo "ext4=ok entries=$entries"
