#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The ext4 driver against volumes mkfs.ext4 made (docs/filesystem-design/ext4.md
# steps 1 and 2).
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
# Reading. The tree has an unwritten extent (debugfs fallocate), a two-level
# extent tree, and an htree directory; two groups are made BLOCK_UNINIT with
# their on-disk bitmaps filled with ones, as a volume that has never allocated
# in them looks and as mke2fs of this version no longer leaves them.
#
# Writing. The driver applies a scripted set of changes (create, writes at
# offsets and past the end, appends, truncates, directories made, moved and
# removed, links, symlinks of three kinds, a big directory, owners, modes and
# times, 200 MiB of data that leaves the first group) to a copy of that volume.
# Then e2fsck -fn must find it clean - every count the format keeps twice, every
# checksum, every link count - and debugfs reads the whole volume back out
# (rdump) to be compared with the host's own copy of the tree, to which the same
# changes were made with the host's own calls: names, types, sizes, contents,
# modes, link targets, and the owners and times that were set. A second, small
# volume is filled until the driver says ENOSPC and must still be clean.
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

# The superblock's free counts are the sum of the groups'. e2fsck does not hold
# the superblock to it (a mounted kernel updates them lazily, so a difference
# there is not an error to it), and statfs reads them: so this does.
counts_ok() {
    dumpe2fs "$1" 2> /dev/null | awk '
        /^Free blocks:/ { sb_b = $3 }
        /^Free inodes:/ { sb_i = $3 }
        /^ +[0-9]+ free blocks, [0-9]+ free inodes/ { gb += $1; gi += $4 }
        END { exit !(sb_b == gb && sb_i == gi && gb > 0) }'
}

for tool in mkfs.ext4 e2fsck debugfs dumpe2fs; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "ext4=skip reason=no_$tool"
        exit 0
    fi
done
EX="$(cd "$B" && pwd)/vibeos_ext4_exercise"
[ -x "$EX" ] || fail "$EX missing" exercise_not_built

python3 scripts/dev/ext4-tree.py make "$T/tree" || fail "tree" tree_not_made
# 768 MiB is six groups, and 1024 inodes is about 170 a group: the tree's 619
# entries reach past the first group's inode table, so a descriptor other than
# group 0's is read. mkfs puts the journal in the middle and leaves the groups
# nothing was allocated in BLOCK_UNINIT.
truncate -s 768M "$T/fs.img"
mkfs.ext4 -q -F -N 1024 -d "$T/tree" "$T/fs.img" > "$T/mkfs.log" 2>&1 || fail "$(head -1 "$T/mkfs.log")" mkfs_failed
# mkfs.ext4 -d writes every directory linear; e2fsck -D rebuilds the large ones
# as htree, which is what a directory Linux grew would be. Its exit code is 1
# when it changed something, which here it must.
e2fsck -fyD "$T/fs.img" > "$T/fsckD.log" 2>&1
[ $? -le 1 ] || fail "$(head -3 "$T/fsckD.log" | tr '\n' ' ')" e2fsck_D_failed

# An extent that is allocated and not written, which mkfs.ext4 -d never makes.
debugfs -w -R "fallocate /prealloc.bin 0 99" "$T/fs.img" > /dev/null 2>&1
debugfs -R "ex /prealloc.bin" "$T/fs.img" 2>/dev/null | grep -q Uninit \
    || fail "/prealloc.bin has no unwritten extent" fixture_has_no_unwritten_extent
# What an unwritten extent holds is whatever was on the disk before, and it must
# read as zeros all the same: the blocks are filled with junk, so a reader that
# treats the extent as written, or a writer that makes a block of it written
# without zeroing the rest, shows.
debugfs -R "ex /prealloc.bin" "$T/fs.img" 2>/dev/null | awk '/Uninit/ {print $8, $11}' | while read -r start len; do
    head -c $((len * 4096)) /dev/zero | tr '\0' '\245' | dd of="$T/fs.img" bs=4096 seek="$start" conv=notrunc 2> /dev/null
done

# The groups nothing was allocated in are BLOCK_UNINIT, and the bitmap on the disk
# was never written: it is overwritten here with ones, so a driver that read it
# would find no room, and one that builds it finds the superblock copy, the
# descriptors and the reserved blocks the groups with a backup carry (1 and 3)
# and the ones without (4). Three such groups are the fixture; fewer and the
# test would not reach what it is there for.
dumpe2fs "$T/fs.img" 2> /dev/null | awk '/^Group /{g=$2; sub(":", "", g); u=($0 ~ /BLOCK_UNINIT/)} $1=="Block" && $2=="bitmap" && u {print g, $4}' > "$T/bb.txt"
[ "$(awk '{printf "%s,", $1}' "$T/bb.txt")" = "1,3,4," ] \
    || fail "BLOCK_UNINIT groups are $(awk '{printf "%s ", $1}' "$T/bb.txt"), wanted 1 3 4" fixture_has_no_uninit_groups
while read -r g blk; do
    head -c 4096 /dev/zero | tr '\0' '\377' | dd of="$T/fs.img" bs=4096 seek="$blk" conv=notrunc 2> /dev/null
done < "$T/bb.txt"
e2fsck -fn "$T/fs.img" > "$T/fsck.log" 2>&1 || fail "$(head -3 "$T/fsck.log" | tr '\n' ' ')" fixture_not_clean
# And the free space the writer will take blocks from, in the first two groups,
# is junk too: a volume mkfs just made is zeros wherever nothing was written,
# which forgives a block that is not zeroed before part of it is used.
python3 scripts/dev/ext4-tree.py junk "$T/fs.img" 2 > /dev/null || fail "junk" junk_not_written

# The tree must have made the driver do what it is there for.
depth="$(debugfs -R "ex /sparse-forty-extents.bin" "$T/fs.img" 2>/dev/null | awk 'NR==2 {print $2}')"
case "$depth" in 0|"") fail "sparse file's tree has no index level (depth $depth)" fixture_has_no_index_level ;; esac
# Two leaves under the index, so choosing between them is something a lookup does.
leaves="$(debugfs -R "ex /sparse-400-extents.bin" "$T/fs.img" 2>/dev/null | awk 'NR==2 {print $4}')"
[ "${leaves:-0}" -ge 2 ] || fail "the 400-extent file has $leaves leaves" fixture_has_one_leaf
debugfs -R "htree /many" "$T/fs.img" 2>/dev/null | grep -q "Root node dump" \
    || fail "/many is not htree-indexed" fixture_has_no_htree

python3 scripts/dev/ext4-tree.py list "$T/tree" > "$T/want.txt"
(cd "$T" && "$EX" walk) > "$T/seen.raw" 2> "$T/walk.err" \
    || fail "$(grep -m1 FAIL "$T/walk.err" || tail -1 "$T/walk.err")" driver_walk_failed
sort "$T/seen.raw" > "$T/seen.txt"
if ! diff "$T/want.txt" "$T/seen.txt" > "$T/diff.txt"; then
    fail "$(head -4 "$T/diff.txt" | tr '\n' ' ')" listing_differs
fi
entries="$(grep -c . "$T/seen.txt")"

# ---- writing ----------------------------------------------------------------
mkdir -p "$T/w"
cp "$T/fs.img" "$T/w/fs.img"
python3 scripts/dev/ext4-tree.py script "$T/script.txt" || fail "script" script_not_made
(cd "$T/w" && cp "$T/script.txt" script.txt && "$EX" apply) > "$T/apply.out" 2> "$T/apply.err" \
    || fail "$(grep -m1 FAIL "$T/apply.err" || tail -1 "$T/apply.err")" driver_apply_failed
e2fsck -fn "$T/w/fs.img" > "$T/fsckw.log" 2>&1 \
    || fail "$(grep -v -E '^(e2fsck|Pass)' "$T/fsckw.log" | head -4 | tr '\n' ' ')" written_volume_not_clean
counts_ok "$T/w/fs.img" || fail "the superblock's free counts are not the groups' sum" superblock_counts_wrong
# A file written in two hundred pieces and a file written in six hundred are a
# few extents each, because the driver joins what runs on: merging is not a
# requirement of the format, so e2fsck cannot ask for it, and this does.
runs="$(debugfs -R "ex /big.bin" "$T/w/fs.img" 2>/dev/null | grep -c -E '^ *[0-9]+/ *[0-9]+ +[0-9]+/ *[0-9]+ ' )"
[ "$runs" -le 6 ] || fail "/big.bin is $runs extents" extents_not_merged
python3 scripts/dev/ext4-tree.py apply "$T/tree" || fail "host apply" host_apply_failed
mkdir "$T/dump"
debugfs -R "rdump / $T/dump" "$T/w/fs.img" > /dev/null 2> "$T/rdump.err"
python3 scripts/dev/ext4-tree.py compare "$T/tree" "$T/dump" "$T/w/fs.img" > "$T/compare.out" \
    || fail "$(head -3 "$T/compare.out" | tr '\n' ' ')" written_tree_differs
# The driver's own reading of what it wrote, through the reading operations.
python3 scripts/dev/ext4-tree.py list "$T/tree" > "$T/want2.txt"
(cd "$T/w" && "$EX" walk) 2> "$T/walk2.err" | sort > "$T/seen2.txt"
diff "$T/want2.txt" "$T/seen2.txt" > "$T/diff2.txt" \
    || fail "$(head -4 "$T/diff2.txt" | tr '\n' ' ')" driver_does_not_read_what_it_wrote
written="$(grep -c . "$T/seen2.txt")"

# ---- a full volume ----------------------------------------------------------
mkdir -p "$T/f"
truncate -s 24M "$T/f/fs.img"
mkfs.ext4 -q -F -N 64 "$T/f/fs.img" > "$T/mkfsf.log" 2>&1 || fail "$(head -1 "$T/mkfsf.log")" mkfs_small_failed
printf 'create\t/fill.bin\t644\nfillup\t/fill.bin\t51\n' > "$T/full1.txt"
(cd "$T/f" && cp "$T/full1.txt" script.txt && "$EX" apply) > "$T/full1.out" 2> "$T/full1.err" \
    || fail "$(grep -m1 FAIL "$T/full1.err" || tail -1 "$T/full1.err")" driver_fill_failed
e2fsck -fn "$T/f/fs.img" > "$T/fsckf.log" 2>&1 \
    || fail "$(grep -v -E '^(e2fsck|Pass)' "$T/fsckf.log" | head -4 | tr '\n' ' ')" full_volume_not_clean
printf 'truncate\t/fill.bin\t1000000\ncreate\t/after\t644\nwrite\t/after\t0\t200000\t52\n' > "$T/full2.txt"
(cd "$T/f" && cp "$T/full2.txt" script.txt && "$EX" apply) > "$T/full2.out" 2> "$T/full2.err" \
    || fail "$(grep -m1 FAIL "$T/full2.err" || tail -1 "$T/full2.err")" driver_cannot_reuse_freed_space
e2fsck -fn "$T/f/fs.img" > "$T/fsckf2.log" 2>&1 \
    || fail "$(grep -v -E '^(e2fsck|Pass)' "$T/fsckf2.log" | head -4 | tr '\n' ' ')" freed_volume_not_clean
counts_ok "$T/f/fs.img" || fail "the superblock's free counts are not the groups' sum (full)" superblock_counts_wrong
mkdir "$T/fdump"
debugfs -R "rdump / $T/fdump" "$T/f/fs.img" > /dev/null 2>&1
python3 scripts/dev/ext4-tree.py patcheck "$T/fdump/fill.bin" 51 > "$T/pc1.out" || fail "$(cat "$T/pc1.out")" filled_file_differs
python3 scripts/dev/ext4-tree.py patcheck "$T/fdump/after" 52 > "$T/pc2.out" || fail "$(cat "$T/pc2.out")" reused_file_differs
[ "$(stat -c %s "$T/fdump/fill.bin")" = 1000000 ] || fail "fill.bin is not 1000000 bytes" truncated_size_wrong

# ---- a fresh volume: inodes in a group nobody has used ----------------------
mkdir -p "$T/n" "$T/ntree"
truncate -s 320M "$T/n/fs.img"
mkfs.ext4 -q -F -N 1024 "$T/n/fs.img" > "$T/mkfsn.log" 2>&1 || fail "$(head -1 "$T/mkfsn.log")" mkfs_fresh_failed
dumpe2fs "$T/n/fs.img" 2> /dev/null | grep -q INODE_UNINIT || fail "the fresh volume has no INODE_UNINIT group" fixture_has_no_inode_uninit
python3 scripts/dev/ext4-tree.py junk "$T/n/fs.img" 2 > /dev/null || fail "junk" junk_not_written
python3 scripts/dev/ext4-tree.py script "$T/nscript.txt" fresh
(cd "$T/n" && cp "$T/nscript.txt" script.txt && "$EX" apply) > "$T/napply.out" 2> "$T/napply.err" \
    || fail "$(grep -m1 FAIL "$T/napply.err" || tail -1 "$T/napply.err")" driver_fresh_apply_failed
e2fsck -fn "$T/n/fs.img" > "$T/fsckn.log" 2>&1 \
    || fail "$(grep -v -E '^(e2fsck|Pass)' "$T/fsckn.log" | head -4 | tr '\n' ' ')" fresh_volume_not_clean
counts_ok "$T/n/fs.img" || fail "the superblock's free counts are not the groups' sum (fresh)" superblock_counts_wrong
dumpe2fs "$T/n/fs.img" 2> /dev/null | grep '^Group 1' | grep -q INODE_UNINIT \
    && fail "group 1 is still INODE_UNINIT: nothing used its inodes" fresh_volume_did_not_reach_group_1
python3 scripts/dev/ext4-tree.py apply "$T/ntree" fresh || fail "host apply (fresh)" host_apply_failed
mkdir "$T/ndump"
debugfs -R "rdump / $T/ndump" "$T/n/fs.img" > /dev/null 2>&1
python3 scripts/dev/ext4-tree.py compare "$T/ntree" "$T/ndump" "$T/n/fs.img" > "$T/ncompare.out" \
    || fail "$(head -3 "$T/ncompare.out" | tr '\n' ' ')" fresh_tree_differs

# ---- a journal e2fsprogs wrote (step 3) -------------------------------------
# The driver's own journal is replayed by e2fsck in verify-ext4-powercut.sh;
# this is the other direction. debugfs writes transactions into a volume's
# journal and leaves them unreplayed, RECOVER set, as a machine that stopped
# would: one that writes two blocks, one that revokes the second, and one
# that is never committed. The driver's mount must replay the first two and
# not the third, so the first block is the data, the second is what was
# there (the revoke covers the transaction before it), and the third too;
# and e2fsck, replaying the same image itself, must agree. Twice: the tags
# debugfs writes by default (64-bit, no checksums) and v3 checksums.
for v in plain v3; do
    J="$T/jnl-$v"
    mkdir -p "$J"
    truncate -s 16M "$J/fs.img"
    mkfs.ext4 -q -F -b 4096 -N 128 "$J/fs.img" > /dev/null 2>&1 || fail "mkfs ($v)" mkfs_journal_failed
    head -c 12288 /dev/urandom > "$J/data.bin"
    dd if="$J/fs.img" of="$J/before.bin" bs=4096 skip=3000 count=3 2> /dev/null
    if [ "$v" = v3 ]; then jo="jo -c -v 3"; else jo="jo"; fi
    printf '%s\njw -b 3000,3001 %s\njw -r 3001\njw -b 3002 -c %s\njc\n' "$jo" "$J/data.bin" "$J/data.bin" > "$J/cmds"
    debugfs -w -f "$J/cmds" "$J/fs.img" > "$J/debugfs.log" 2>&1
    dumpe2fs -h "$J/fs.img" 2> /dev/null | grep -q needs_recovery \
        || fail "debugfs left no transaction to replay ($v)" journal_fixture_empty
    if [ "$v" = v3 ]; then
        dumpe2fs -h "$J/fs.img" 2> /dev/null | grep -q 'Journal features:.*journal_checksum_v3' \
            || fail "debugfs did not write v3 checksums" journal_fixture_not_v3
    fi
    cp "$J/fs.img" "$J/theirs.img"
    (cd "$J" && "$EX" recover) > "$J/rec.out" 2> "$J/rec.err" \
        || fail "$(cat "$J/rec.err" "$J/rec.out" | head -1) ($v)" journal_replay_failed
    grep -q '^replayed=2 ' "$J/rec.out" || fail "$(cat "$J/rec.out") ($v), wanted 2" journal_replay_count
    e2fsck -fn "$J/fs.img" > "$J/fsck.log" 2>&1 \
        || fail "$(grep -v -E '^(e2fsck|Pass)' "$J/fsck.log" | head -4 | tr '\n' ' ') ($v)" journal_replay_not_clean
    dd if="$J/fs.img" of="$J/after.bin" bs=4096 skip=3000 count=3 2> /dev/null
    head -c 4096 "$J/data.bin" > "$J/want.bin"
    tail -c 8192 "$J/before.bin" >> "$J/want.bin"
    cmp -s "$J/after.bin" "$J/want.bin" || fail "blocks 3000-3002 after the replay are not data, before, before ($v)" journal_replay_wrong_blocks
    e2fsck -fy "$J/theirs.img" > "$J/fsy.log" 2>&1
    dd if="$J/theirs.img" of="$J/theirs.bin" bs=4096 skip=3000 count=3 2> /dev/null
    cmp -s "$J/after.bin" "$J/theirs.bin" || fail "e2fsck replayed blocks 3000-3002 differently ($v)" journal_replays_disagree
done

# ---- a write that fails (E4-009) ---------------------------------------------
# Two failures that leave e2fsck happy or nearly so, and the file wrong.
#
# A write whose data fits and whose extent leaf does not: an inode already
# holding its four extents, a volume with exactly four blocks free, and a
# fifth extent of four blocks. The data takes the four, the leaf finds none,
# and the blocks must go back - they were left marked in use and named by
# nothing.
L="$T/leak"
mkdir -p "$L"
truncate -s 8M "$L/fs.img"
mkfs.ext4 -q -F -b 1024 -N 64 -O ^has_journal "$L/fs.img" > /dev/null 2>&1 || fail "mkfs (leak)" mkfs_leak_failed
printf 'create\t/a\t644\nstride\t/a\t4\t8192\t1024\t1\ncreate\t/f\t644\nfillup\t/f\t3\n' > "$L/script.txt"
(cd "$L" && "$EX" apply) > "$L/a1.out" 2> "$L/a1.err" || fail "$(head -1 "$L/a1.err")" leak_setup_failed
fsize="$(debugfs -R "stat /f" "$L/fs.img" 2> /dev/null | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1)"
printf 'truncate\t/f\t%s\nfail\tENOSPC\twrite\t/a\t1048576\t4096\t2\n' $(( fsize - 4096 )) > "$L/script.txt"
(cd "$L" && "$EX" apply) > "$L/a2.out" 2> "$L/a2.err" || fail "$(head -1 "$L/a2.err")" leaf_enospc_not_refused
e2fsck -fn "$L/fs.img" > "$L/fsck.log" 2>&1 \
    || fail "$(grep -v -E '^(e2fsck|Pass)' "$L/fsck.log" | head -3 | tr '\n' ' ')" leaf_enospc_leaked_blocks

# A write that fails half-way through a run it took: the rest of the run must
# leave the file, or a file grown over it later reads whatever those blocks
# last held - here a deleted file's bytes. e2fsck calls both outcomes clean.
S="$T/stale"
mkdir -p "$S"
truncate -s 16M "$S/fs.img"
mkfs.ext4 -q -F -b 4096 -N 64 "$S/fs.img" > /dev/null 2>&1 || fail "mkfs (stale)" mkfs_stale_failed
printf 'create\t/old\t644\nwrite\t/old\t0\t163840\t5\nunlink\t/old\ncreate\t/b\t644\nfaildata\t10\nwriteonce\t/b\t0\t163840\t6\n' > "$S/script.txt"
(cd "$S" && "$EX" apply) > "$S/a1.out" 2> "$S/a1.err" || fail "$(head -1 "$S/a1.err")" stale_setup_failed
bsize="$(debugfs -R "stat /b" "$S/fs.img" 2> /dev/null | sed -n 's/.*Size: \([0-9]*\).*/\1/p' | head -1)"
[ -n "$bsize" ] && [ "$bsize" -gt 0 ] && [ "$bsize" -lt 163840 ] || fail "the failed write left /b at ${bsize:-?} bytes" stale_write_did_not_fail_half_way
printf 'truncate\t/b\t163840\n' > "$S/script.txt"
(cd "$S" && "$EX" apply) > "$S/a2.out" 2> "$S/a2.err" || fail "$(head -1 "$S/a2.err")" stale_truncate_failed
debugfs -R "cat /b" "$S/fs.img" 2> /dev/null | tail -c +$(( bsize + 1 )) | tr -d '\0' | head -c 1 > "$S/tail.bin"
[ ! -s "$S/tail.bin" ] || fail "/b past its failed write reads bytes that are not zero" failed_write_left_stale_data

# ---- the refusals -----------------------------------------------------------
# An inode whose checksum no longer matches: its low half set to zero.
mkdir -p "$T/csum" "$T/feat"
cp "$T/fs.img" "$T/csum/fs.img"
debugfs -w -R "sif /small.txt checksum 0" "$T/csum/fs.img" > /dev/null 2>&1
(cd "$T/csum" && "$EX" refused-file /small.txt) > "$T/csum.out" 2> "$T/csum.err" \
    || fail "$(cat "$T/csum.err" "$T/csum.out" | head -1)" bad_inode_checksum_read

# A feature this driver does not read: inline_data puts a file in its inode.
cp "$T/fs.img" "$T/feat/fs.img"
debugfs -w -R "feature inline_data" "$T/feat/fs.img" > /dev/null 2>&1
(cd "$T/feat" && "$EX" refused-mount) > "$T/feat.out" 2> "$T/feat.err" \
    || fail "$(cat "$T/feat.err" "$T/feat.out" | head -1)" unknown_feature_mounted
grep -q "features=0x8000" "$T/feat.out" || fail "$(cat "$T/feat.out")" refusal_names_wrong_feature

echo "ext4=ok entries=$entries written=$written"
