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

# ---- a node outlives its file (E4-012) ----------------------------------------
# A node held across its file's last unlink, once the inode number has gone to
# a new file: reading or writing through it must be refused, not done to the
# new file. The exerciser checks the number really was reused.
G="$T/stale-node"
mkdir -p "$G"
truncate -s 16M "$G/fs.img"
mkfs.ext4 -q -F -N 64 "$G/fs.img" > /dev/null 2>&1 || fail "mkfs (stale node)" mkfs_stale_node_failed
printf 'staleness
' > "$G/script.txt"
(cd "$G" && "$EX" apply) > "$G/a.out" 2> "$G/a.err"     || fail "$(grep -m1 FAIL "$G/a.err" || head -1 "$G/a.err")" stale_node_reached_another_file
e2fsck -fn "$G/fs.img" > "$G/fsck.log" 2>&1 || fail "$(head -3 "$G/fsck.log" | tr '
' ' ')" stale_node_volume_not_clean

# ---- a file open when its last name goes (E4-019) ---------------------------
# Held by a description, the file must outlive its name: readable through the
# description, and on the volume's orphan list so that a machine that stops
# before the close still frees it. Three ways out, each judged by e2fsck and by
# the free counts against the volume before the file existed:
#   closed  - the close frees it;
#   crashed - the image is left with the file still open, and the next mount
#             frees what the list names;
#   linux   - an orphan this driver did not make: debugfs puts a file on the
#             list the way Linux's own unlink would, and the mount frees it.
orph_free() { dumpe2fs -h "$1" 2>/dev/null | sed -n 's/^Free \(blocks\|inodes\): *//p' | tr '\n' ' '; }
orph_head() { dumpe2fs -h "$1" 2>/dev/null | sed -n 's/^First orphan inode: *//p'; }
O="$T/orphan"
mkdir -p "$O/closed" "$O/crashed" "$O/linux"
truncate -s 16M "$O/base.img"
mkfs.ext4 -q -F -N 64 "$O/base.img" > /dev/null 2>&1 || fail "mkfs (orphan)" mkfs_orphan_failed
before=$(orph_free "$O/base.img")
for k in closed crashed linux; do cp "$O/base.img" "$O/$k/fs.img"; done
printf 'orphankeep	/held	300000	7\norphanclose\n' > "$O/closed/script.txt"
(cd "$O/closed" && "$EX" apply) > "$O/closed/a.out" 2> "$O/closed/a.err" \
    || fail "$(grep -m1 FAIL "$O/closed/a.err" || head -1 "$O/closed/a.err")" orphan_open_file_lost
e2fsck -fn "$O/closed/fs.img" > "$O/closed/fsck.log" 2>&1 \
    || fail "$(head -3 "$O/closed/fsck.log" | tr '\n' ' ')" orphan_closed_volume_not_clean
[ "$(orph_free "$O/closed/fs.img")" = "$before" ] \
    || fail "free after close: $(orph_free "$O/closed/fs.img") wanted $before" orphan_close_leaked
[ -z "$(orph_head "$O/closed/fs.img")" ]     || fail "the closed file is still on the orphan list: $(orph_head "$O/closed/fs.img")" orphan_close_left_listed

printf 'orphankeep	/held	300000	7\n' > "$O/crashed/script.txt"
(cd "$O/crashed" && "$EX" apply) > "$O/crashed/a.out" 2> "$O/crashed/a.err" \
    || fail "$(grep -m1 FAIL "$O/crashed/a.err" || head -1 "$O/crashed/a.err")" orphan_open_file_lost
[ -n "$(orph_head "$O/crashed/fs.img")" ] || fail "an open file without names is on no orphan list" orphan_not_listed
(cd "$O/crashed" && "$EX" recover) > "$O/crashed/r.out" 2> "$O/crashed/r.err" \
    || fail "$(head -1 "$O/crashed/r.err")" orphan_recover_failed
e2fsck -fn "$O/crashed/fs.img" > "$O/crashed/fsck.log" 2>&1 \
    || fail "$(head -3 "$O/crashed/fsck.log" | tr '\n' ' ')" orphan_crashed_volume_not_clean
[ -z "$(orph_head "$O/crashed/fs.img")" ] && [ "$(orph_free "$O/crashed/fs.img")" = "$before" ] \
    || fail "after the mount: free $(orph_free "$O/crashed/fs.img") wanted $before, list $(orph_head "$O/crashed/fs.img")" orphan_mount_did_not_free

head -c 300000 /dev/urandom > "$O/linux/payload"
debugfs -w -f - "$O/linux/fs.img" > /dev/null 2>&1 <<EOF2
write $O/linux/payload held
EOF2
lino=$(debugfs -R "stat /held" "$O/linux/fs.img" 2>/dev/null | sed -n 's/^Inode: *\([0-9]*\).*/\1/p')
[ -n "$lino" ] || fail "debugfs did not write the file" orphan_linux_setup_failed
debugfs -w -f - "$O/linux/fs.img" > /dev/null 2>&1 <<EOF2
unlink /held
sif <$lino> links_count 0
ssv last_orphan $lino
EOF2
[ "$(orph_head "$O/linux/fs.img")" = "$lino" ] || fail "debugfs did not list the orphan" orphan_linux_setup_failed
(cd "$O/linux" && "$EX" recover) > "$O/linux/r.out" 2> "$O/linux/r.err" \
    || fail "$(head -1 "$O/linux/r.err")" orphan_recover_failed
e2fsck -fn "$O/linux/fs.img" > "$O/linux/fsck.log" 2>&1 \
    || fail "$(head -3 "$O/linux/fsck.log" | tr '\n' ' ')" orphan_linux_volume_not_clean
[ -z "$(orph_head "$O/linux/fs.img")" ] && [ "$(orph_free "$O/linux/fs.img")" = "$before" ] \
    || fail "after the mount: free $(orph_free "$O/linux/fs.img") wanted $before" orphan_linux_not_freed

# ---- a release larger than one transaction (E4-018) -------------------------
# A file over hundreds of small groups: freeing it touches a bitmap and a
# descriptor block in each, more than one transaction may log. The unlink and
# the truncate must succeed - in pieces, committed between - and leave a clean
# volume with every block back. Then the power is cut at points through the
# unlink: whatever the cut left, the next mount must finish it from the orphan
# list - a clean volume, nothing listed, and the file either whole (cut before
# its name went) or gone with every block back.
B18="$T/bigfree"
mkdir -p "$B18"
truncate -s 400M "$B18/base.img"
mkfs.ext4 -q -F -b 1024 -g 768 -N 512 "$B18/base.img" > /dev/null 2>&1 || fail "mkfs (big release)" mkfs_bigfree_failed
free0=$(orph_free "$B18/base.img")
for op in unlink truncate; do
    cp "$B18/base.img" "$B18/fs.img"
    if [ "$op" = unlink ]; then last='unlink\t/big'; else last='truncate\t/big\t0'; fi
    printf "create\t/big\t644\nwrite\t/big\t0\t390000000\t3\n$last\n" > "$B18/script.txt"
    (cd "$B18" && "$EX" apply) > "$B18/a.out" 2> "$B18/a.err" \
        || fail "$op: $(grep -m1 FAIL "$B18/a.err" || head -1 "$B18/a.err")" big_release_refused
    e2fsck -fn "$B18/fs.img" > "$B18/fsck.log" 2>&1 \
        || fail "$op: $(head -3 "$B18/fsck.log" | tr '\n' ' ')" big_release_volume_not_clean
    want="$free0"
    [ "$op" = truncate ] && want=$(echo "$free0" | awk '{print $1, $2 - 1}')
    [ "$(orph_free "$B18/fs.img")" = "$want " ] || [ "$(orph_free "$B18/fs.img")" = "$want" ] \
        || fail "$op: free $(orph_free "$B18/fs.img") wanted $want" big_release_leaked
done
# The cuts: the writes the first two lines take, then points through the last
# line. The file must come back whole - its size and every block - or as the
# operation left it: gone with every block back after an unlink, empty with no
# blocks after a truncate. Its size alone would not tell a truncate that freed
# blocks under the old size (a sparse file reads as zeros there, and e2fsck
# calls that clean).
bstat() { debugfs -R "stat /big" "$B18/fs.img" 2>/dev/null; }
cp "$B18/base.img" "$B18/fs.img"
printf 'create\t/big\t644\nwrite\t/big\t0\t390000000\t3\n' > "$B18/script.txt"
w_pre=$(cd "$B18" && "$EX" powercut 0 2>/dev/null | sed -n 's/^writes=\([0-9]*\).*/\1/p')
full_blocks=$(bstat | sed -n 's/.*Blockcount: *\([0-9]*\).*/\1/p')
size_big=390000000
for op in unlink truncate; do
    if [ "$op" = unlink ]; then last='unlink\t/big'; else last='truncate\t/big\t0'; fi
    cp "$B18/base.img" "$B18/fs.img"
    printf "create\t/big\t644\nwrite\t/big\t0\t390000000\t3\n$last\n" > "$B18/script.txt"
    w_all=$(cd "$B18" && "$EX" powercut 0 2>/dev/null | sed -n 's/^writes=\([0-9]*\).*/\1/p')
    [ -n "$w_pre" ] && [ -n "$w_all" ] && [ -n "$full_blocks" ] && [ "$w_all" -gt "$w_pre" ] \
        || fail "cannot count the $op's writes ($w_pre, $w_all, $full_blocks)" big_release_setup_failed
    for k in 1 2 3 4 5 6 7; do
        n=$((w_pre + (w_all - w_pre) * k / 8))
        cp "$B18/base.img" "$B18/fs.img"
        (cd "$B18" && "$EX" powercut "$n") > /dev/null 2>&1
        (cd "$B18" && "$EX" recover) > "$B18/r.out" 2> "$B18/r.err" \
            || fail "$op cut at $n: $(head -1 "$B18/r.err")" big_release_recover_failed
        e2fsck -fn "$B18/fs.img" > "$B18/fsck.log" 2>&1 \
            || fail "$op cut at $n: $(head -3 "$B18/fsck.log" | tr '\n' ' ')" big_release_cut_not_clean
        [ -z "$(orph_head "$B18/fs.img")" ] || fail "$op cut at $n: still listed $(orph_head "$B18/fs.img")" big_release_cut_left_listed
        got=$(bstat | sed -n 's/.*Size: *\([0-9]*\).*/\1/p' | head -1)
        blk=$(bstat | sed -n 's/.*Blockcount: *\([0-9]*\).*/\1/p')
        case "$op:$got:$blk" in
            *:$size_big:$full_blocks) ;;
            truncate:0:0) ;;
            unlink::)
                [ "$(orph_free "$B18/fs.img")" = "$free0" ] \
                    || fail "$op cut at $n: free $(orph_free "$B18/fs.img") wanted $free0" big_release_cut_leaked ;;
            *) fail "$op cut at $n: /big is size $got with $blk blocks (whole: $size_big, $full_blocks)" big_release_cut_half_file ;;
        esac
    done
done

# ---- block-mapped files on an upgraded volume (E4-008) ----------------------
# A volume made as ext3 and upgraded keeps the files written before the upgrade
# block-mapped: direct blocks and indirect trees, not extents. Removing every
# one of them - a file under a single indirect block, one deep enough for a
# double, a fast symlink, a block-mapped directory, and a file replaced by a
# rename - must leave the volume e2fsck calls clean with exactly the free
# blocks and inodes of the same volume made empty. The journal is made again
# after the upgrade: ext3's is block-mapped too, and this driver does not write
# one (E4-020).
M8="$T/blockmap"
mkdir -p "$M8/tree/d" "$M8/empty"
head -c 300000 /dev/urandom > "$M8/tree/d/one"       # direct and single indirect
head -c 5000000 /dev/urandom > "$M8/tree/d/two"      # and a double indirect, at 1 KiB
head -c 5000 /dev/urandom > "$M8/tree/d/small"
head -c 90000 /dev/urandom > "$M8/tree/over"
ln -s small "$M8/tree/d/link"
# 3,000 runs with a hole after each: more than the writer's list of extents
# holds, so its conversion has to lay the tree out leaf by leaf (two index
# levels on 1 KiB blocks).
python3 - "$M8/tree/d/frag" <<'PYEOF'
import sys
with open(sys.argv[1], "wb") as f:
    for k in range(3000):
        f.seek(k * 2048)
        f.write(bytes((k * 3 + j) & 255 for j in range(1024)))
PYEOF
mk8() {   # tree image
    truncate -s 64M "$2"
    mkfs.ext3 -q -F -b 1024 -I 256 -N 64 ${1:+-d "$1"} "$2" > /dev/null 2>&1 &&
    tune2fs -O extent,metadata_csum,huge_file,dir_nlink,extra_isize "$2" > /dev/null 2>&1 &&
    e2fsck -fy "$2" > /dev/null 2>&1
    tune2fs -O ^has_journal "$2" > /dev/null 2>&1 && tune2fs -j "$2" > /dev/null 2>&1
}
mk8 "$M8/tree" "$M8/fs.img" || fail "cannot make the upgraded volume" blockmap_setup_failed
mk8 "" "$M8/empty.img" || fail "cannot make the empty upgraded volume" blockmap_setup_failed
debugfs -R "stat /d/two" "$M8/fs.img" 2>/dev/null | grep -q "(DIND)" \
    || fail "the fixture has no double indirect block" blockmap_setup_failed
debugfs -R "stat /d" "$M8/fs.img" 2>/dev/null | grep -q "Flags: 0x0" \
    || fail "the fixture's directory is not block-mapped" blockmap_setup_failed
printf 'create\t/new\t644\nwrite\t/new\t0\t7000\t4\nrename\t/new\t/over\nunlink\t/over\nunlink\t/d/one\nunlink\t/d/two\nunlink\t/d/small\nunlink\t/d/link\nunlink\t/d/frag\nrmdir\t/d\n' > "$M8/script.txt"
(cd "$M8" && "$EX" apply) > "$M8/a.out" 2> "$M8/a.err" \
    || fail "$(grep -m1 FAIL "$M8/a.err" || head -1 "$M8/a.err")" blockmap_remove_refused
e2fsck -fn "$M8/fs.img" > "$M8/fsck.log" 2>&1 \
    || fail "$(head -3 "$M8/fsck.log" | tr '\n' ' ')" blockmap_volume_not_clean
[ "$(orph_free "$M8/fs.img")" = "$(orph_free "$M8/empty.img")" ] \
    || fail "free $(orph_free "$M8/fs.img"), an empty volume has $(orph_free "$M8/empty.img")" blockmap_blocks_leaked

# Written and truncated: a block-mapped file becomes extent-mapped first (the
# same blocks, its indirect blocks given back - chattr +e on Linux), and then
# changes as any other. The expected bytes are made here by the exerciser's
# own pattern, applied to the files the volume was made from; debugfs reads
# what the driver left, and e2fsck says no block was lost on the way.
W8="$T/blockmap-write"
mkdir -p "$W8"
cp -r "$M8/tree" "$W8/want"
mk8 "$M8/tree" "$W8/fs.img" || fail "cannot make the upgraded volume" blockmap_setup_failed
printf 'write\t/d/one\t100000\t5000\t7\nwrite\t/d/one\t300000\t70000\t9\nwrite\t/d/two\t4990000\t30000\t8\ntruncate\t/d/two\t2000000\ntruncate\t/d/small\t1000\nwrite\t/d/frag\t3000000\t5000\t3\n' > "$W8/script.txt"
# And the block-mapped directory grows: entries past its one block.
for k in $(seq 1 40); do printf 'create\t/d/e%03d-%s\t644\n' "$k" xxxxxxxxxxxxxxxxxxxxxxxx >> "$W8/script.txt"; done
python3 - "$W8/want" <<'PYEOF'
import os, sys
root = sys.argv[1]
def pat(seed, p):    # pat() in tests/kernel/ext4_exercise.c
    return (seed * 31 + (p >> 8) * ((seed << 1) | 1) + (p & 255)) & 0xFF
def write(name, off, n, seed):
    path = os.path.join(root, name)
    with open(path, "r+b") as f:
        f.seek(off)
        f.write(bytes(pat(seed, off + i) for i in range(n)))
def trunc(name, size):
    os.truncate(os.path.join(root, name), size)
write("d/frag", 3000000, 5000, 3)
write("d/one", 100000, 5000, 7)
write("d/one", 300000, 70000, 9)
write("d/two", 4990000, 30000, 8)
trunc("d/two", 2000000)
trunc("d/small", 1000)
PYEOF
(cd "$W8" && "$EX" apply) > "$W8/a.out" 2> "$W8/a.err" \
    || fail "$(grep -m1 FAIL "$W8/a.err" || head -1 "$W8/a.err")" blockmap_write_refused
e2fsck -fn "$W8/fs.img" > "$W8/fsck.log" 2>&1 \
    || fail "$(head -3 "$W8/fsck.log" | tr '\n' ' ')" blockmap_write_volume_not_clean
for f in d/one d/two d/small d/frag; do
    debugfs -R "dump /$f $W8/got" "$W8/fs.img" > /dev/null 2>&1
    cmp -s "$W8/got" "$W8/want/$f" || fail "/$f is not what the writes made it" blockmap_write_wrong_bytes
    debugfs -R "stat /$f" "$W8/fs.img" 2>/dev/null | grep -q "Flags: 0x80000" \
        || fail "/$f was changed and is still block-mapped" blockmap_not_migrated
done
debugfs -R "ex /d/frag" "$W8/fs.img" 2>/dev/null | grep -q "^ 1/ 2" \
    || fail "/d/frag's conversion did not make a tree two index levels deep" blockmap_frag_not_deep
[ "$(debugfs -R "ls /d" "$W8/fs.img" 2>/dev/null | tr -s ' ' '\n' | grep -c '^e[0-9]')" = 40 ] \
    || fail "the block-mapped directory did not take its 40 new entries" blockmap_dir_not_grown
debugfs -R "stat /d" "$W8/fs.img" 2>/dev/null | grep -q "Flags: 0x80000" \
    || fail "/d grew and is still block-mapped" blockmap_dir_not_migrated

# ---- trees with index levels (E4-008) ---------------------------------------
# On 1 KiB blocks a leaf holds 84 extents and an index block 84 leaves, so a
# file of 3,000 one-block extents with a hole after each is two index levels
# deep - mkfs.ext4 -d builds it, Linux's own tree. The driver changes it in the
# middle and at the end, cuts it in the middle, and the bytes debugfs reads are
# held to the same changes made here with the exerciser's pattern. Then the
# driver grows a tree of its own past what one index block holds (8,000
# separate pieces: the root becomes an index level, and an index block splits),
# and both files go - truncated to nothing, then unlinked - leaving the free
# counts of the same volume made with nothing in it.
D8="$T/deep"
mkdir -p "$D8/tree" "$D8/emptytree"
python3 - "$D8/tree/frag" <<'PYEOF'
import sys
with open(sys.argv[1], "wb") as f:
    for k in range(3000):
        f.seek(k * 2048)
        f.write(bytes((k * 7 + j) & 255 for j in range(1024)))
PYEOF
cp "$D8/tree/frag" "$D8/want"
truncate -s 64M "$D8/fs.img" "$D8/empty.img"
mkfs.ext4 -q -F -b 1024 -N 64 -d "$D8/tree" "$D8/fs.img" > /dev/null 2>&1 || fail "mkfs (deep tree)" deep_setup_failed
mkfs.ext4 -q -F -b 1024 -N 64 -d "$D8/emptytree" "$D8/empty.img" > /dev/null 2>&1 || fail "mkfs (deep tree)" deep_setup_failed
debugfs -R "ex /frag" "$D8/fs.img" 2>/dev/null | grep -q "^ 1/ 2" \
    || fail "mkfs.ext4 did not make /frag two index levels deep" deep_setup_failed
printf 'write\t/frag\t1500000\t3000\t5\nwrite\t/frag\t6142000\t9000\t6\nwrite\t/frag\t1000\t100\t2\nwrite\t/frag\t2200000\t1000000\t4\nwrite\t/frag\t2200000\t1000000\t8\ntruncate\t/frag\t4000000\n' > "$D8/script.txt"
python3 - "$D8/want" <<'PYEOF'
import os, sys
path = sys.argv[1]
def pat(seed, p):    # pat() in tests/kernel/ext4_exercise.c
    return (seed * 31 + (p >> 8) * ((seed << 1) | 1) + (p & 255)) & 0xFF
with open(path, "r+b") as f:
    # The last two run through far more leaves than a window holds. The first
    # fills the holes there, and is cut short by the fresh runs one call may
    # take long before it leaves a window; the second, over blocks that are
    # all there now, takes none, and crosses window after window.
    for off, n, seed in ((1500000, 3000, 5), (6142000, 9000, 6), (1000, 100, 2), (2200000, 1000000, 4),
                         (2200000, 1000000, 8)):
        f.seek(off)
        f.write(bytes(pat(seed, off + i) for i in range(n)))
os.truncate(path, 4000000)
PYEOF
(cd "$D8" && "$EX" apply) > "$D8/a.out" 2> "$D8/a.err" \
    || fail "$(grep -m1 FAIL "$D8/a.err" || head -1 "$D8/a.err")" deep_change_refused
e2fsck -fn "$D8/fs.img" > "$D8/fsck.log" 2>&1 \
    || fail "$(head -3 "$D8/fsck.log" | tr '\n' ' ')" deep_volume_not_clean
debugfs -R "dump /frag $D8/got" "$D8/fs.img" > /dev/null 2>&1
cmp -s "$D8/got" "$D8/want" || fail "/frag is not what the writes and the cut made it" deep_wrong_bytes
printf 'create\t/grown\t644\nstride\t/grown\t8000\t2048\t1024\t9\n' > "$D8/script.txt"
(cd "$D8" && "$EX" apply) > "$D8/a.out" 2> "$D8/a.err" \
    || fail "$(grep -m1 FAIL "$D8/a.err" || head -1 "$D8/a.err")" deep_grow_refused
e2fsck -fn "$D8/fs.img" > "$D8/fsck.log" 2>&1 \
    || fail "$(head -3 "$D8/fsck.log" | tr '\n' ' ')" deep_grown_not_clean
debugfs -R "ex /grown" "$D8/fs.img" 2>/dev/null | grep -q "^ 1/ 2" \
    || fail "8,000 pieces did not make /grown two index levels deep" deep_not_grown
[ "$(debugfs -R "ex /grown" "$D8/fs.img" 2>/dev/null | grep -c '^ 0/ 2')" -ge 2 ] \
    || fail "no index block was split under /grown's root" deep_not_split
python3 - "$D8/gwant" <<'PYEOF'
import sys
def pat(seed, p):
    return (seed * 31 + (p >> 8) * ((seed << 1) | 1) + (p & 255)) & 0xFF
with open(sys.argv[1], "wb") as f:
    for k in range(8000):
        f.seek(k * 2048)
        f.write(bytes(pat(9, k * 2048 + i) for i in range(1024)))
PYEOF
debugfs -R "dump /grown $D8/got" "$D8/fs.img" > /dev/null 2>&1
cmp -s "$D8/got" "$D8/gwant" || fail "/grown is not what its 8,000 writes made it" deep_grown_wrong_bytes
printf 'truncate\t/frag\t0\nunlink\t/frag\nunlink\t/grown\n' > "$D8/script.txt"
(cd "$D8" && "$EX" apply) > "$D8/a.out" 2> "$D8/a.err" \
    || fail "$(grep -m1 FAIL "$D8/a.err" || head -1 "$D8/a.err")" deep_release_refused
e2fsck -fn "$D8/fs.img" > "$D8/fsck.log" 2>&1 \
    || fail "$(head -3 "$D8/fsck.log" | tr '\n' ' ')" deep_released_not_clean
[ "$(orph_free "$D8/fs.img")" = "$(orph_free "$D8/empty.img")" ] \
    || fail "free $(orph_free "$D8/fs.img"), an empty volume has $(orph_free "$D8/empty.img")" deep_blocks_leaked

# ---- what Linux keeps as it goes (E4-013) ------------------------------------
# A mount is counted and dated, the superblock says when it was last written
# and how much the volume has taken in its life, and a read sets atime the way
# relatime does: once after a change, not again until the next change or a
# day. The exerciser's clock starts at the same second every run, so the second
# run's read is earlier than the first's atime - not a day later - and must
# leave it alone.
A13="$T/sbtimes"
mkdir -p "$A13"
truncate -s 16M "$A13/fs.img"
mkfs.ext4 -q -F -N 64 "$A13/fs.img" > /dev/null 2>&1 || fail "mkfs (superblock times)" mkfs_sbtimes_failed
sbf() { dumpe2fs -h "$A13/fs.img" 2>/dev/null | sed -n "s/^$1: *//p"; }
mc0=$(sbf "Mount count"); lw0=$(sbf "Lifetime writes")
printf 'create\t/t\t644\nwrite\t/t\t0\t300000\t9\nread\t/t\n' > "$A13/script.txt"
(cd "$A13" && "$EX" apply) > "$A13/a.out" 2> "$A13/a.err" || fail "$(head -1 "$A13/a.err")" sbtimes_apply_failed
[ "$(sbf "Mount count")" = "$((mc0 + 1))" ] || fail "mount count $(sbf "Mount count"), was $mc0" mount_not_counted
[ "$(sbf "Lifetime writes")" != "$lw0" ] || fail "lifetime writes still $lw0" lifetime_writes_not_kept
case "$(sbf "Last write time")" in *2023*) ;; *) fail "last write time $(sbf "Last write time")" write_time_not_kept ;; esac
case "$(sbf "Last mount time")" in *2023*) ;; *) fail "last mount time $(sbf "Last mount time")" mount_time_not_kept ;; esac
itime() { debugfs -R "stat /t" "$A13/fs.img" 2>/dev/null | sed -n "s/^ *$1: *0x\([0-9a-f]*\):.*/\1/p"; }
at1=$(itime atime); mt1=$(itime mtime)
[ -n "$at1" ] && [ $((0x$at1)) -gt $((0x$mt1)) ] || fail "atime $at1 not after mtime $mt1 after a read" atime_not_set_by_read
printf 'read\t/t\n' > "$A13/script.txt"
(cd "$A13" && "$EX" apply) > "$A13/a.out" 2> "$A13/a.err" || fail "$(head -1 "$A13/a.err")" sbtimes_apply_failed
[ "$(itime atime)" = "$at1" ] || fail "atime moved from $at1 to $(itime atime) on a read with nothing changed" atime_not_relatime
e2fsck -fn "$A13/fs.img" > "$A13/fsck.log" 2>&1 || fail "$(head -3 "$A13/fsck.log" | tr '\n' ' ')" sbtimes_volume_not_clean

# ---- a damaged directory block (E4-002) -------------------------------------
# debugfs has no command that breaks a directory leaf's checksum, so one byte
# is changed by hand - in the name of an entry the lookup does not ask for.
# The name asked for is intact, so only the checksum can refuse it: a reader
# that skipped the check would find /cdir/wanted and read it.
D2="$T/dircsum"
mkdir -p "$D2/tree/cdir"
printf 'wanted\n' > "$D2/tree/cdir/wanted"
printf 'other\n' > "$D2/tree/cdir/zzzzother"
truncate -s 16M "$D2/fs.img"
mkfs.ext4 -q -F -N 64 -d "$D2/tree" "$D2/fs.img" > /dev/null 2>&1 || fail "mkfs (dir checksum)" mkfs_dircsum_failed
(cd "$D2" && "$EX" refused-file /cdir/wanted) > /dev/null 2>&1 \
    && fail "a healthy directory was refused" dircsum_healthy_refused
dbs=$(dumpe2fs -h "$D2/fs.img" 2>/dev/null | sed -n 's/^Block size: *//p')
dblk=$(debugfs -R "bmap /cdir 0" "$D2/fs.img" 2>/dev/null)
dd if="$D2/fs.img" of="$D2/block" bs="$dbs" skip="$dblk" count=1 2> /dev/null
doff=$(grep -obUa zzzzother "$D2/block" | head -1 | cut -d: -f1)
[ -n "$dbs" ] && [ -n "$dblk" ] && [ -n "$doff" ] || fail "cannot find /cdir's leaf block" dircsum_setup_failed
printf 'Y' | dd of="$D2/fs.img" bs=1 seek=$((dblk * dbs + doff)) conv=notrunc 2> /dev/null
debugfs -R "ls /cdir" "$D2/fs.img" 2>&1 | grep -qi "checksum" \
    || fail "debugfs does not call the damaged block bad: the damage missed" dircsum_setup_failed
(cd "$D2" && "$EX" refused-file /cdir/wanted) > "$D2/out" 2> "$D2/err" \
    || fail "$(cat "$D2/err" "$D2/out" | head -1)" bad_dir_block_checksum_read

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
