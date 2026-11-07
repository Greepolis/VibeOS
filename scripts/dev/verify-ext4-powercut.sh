#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# ext4's journal against a power cut at every write (docs/filesystem-design/ext4.md
# step 3).
#
#     scripts/dev/verify-ext4-powercut.sh [build-dir] [stride]
#
# mkfs.ext4 makes a small volume, and a script of single operations - each one
# transaction - is run through the driver on a device that takes the first n
# sector writes and drops the rest without saying so: a machine that lost power
# after write n. For every n (or every stride-th, which is what check.sh runs;
# the nightly runs all of them), what the device kept is judged three ways, by
# tools that are not the driver:
#
#   - the driver mounts it - which replays the journal - and e2fsck -fn must
#     call the result clean;
#   - e2fsck -fy replays the same cut image itself, from jbd2's format alone,
#     and e2fsck -fn must then call that clean too;
#   - both trees, listed through the driver's reading operations, must be one
#     of the states the script passes through, and the same one: the state
#     before the operation the cut fell in or the state after it, never a
#     mixture, and no state the run had not yet reached.
#
# Prints ext4_powercut=ok points=<n> writes=<total>, =skip, or
# =FAILED reason=<why>.

B="${1:-build-gcc-Release}"
STRIDE="${2:-1}"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

fail() {
    echo "  $1"
    echo "ext4_powercut=FAILED reason=$2"
    exit 1
}

for tool in mkfs.ext4 e2fsck md5sum; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "ext4_powercut=skip reason=no_$tool"
        exit 0
    fi
done
EX="$(cd "$B" && pwd)/vibeos_ext4_exercise"
[ -x "$EX" ] || fail "$EX missing" exercise_not_built

# 16 MiB with 4 KiB blocks: the smallest volume whose journal mkfs makes the
# usual 1024 blocks, so the log is the shape a real one is.
truncate -s 16M "$T/base.img"
mkfs.ext4 -q -F -b 4096 -N 128 "$T/base.img" > "$T/mkfs.log" 2>&1 || fail "$(head -1 "$T/mkfs.log")" mkfs_failed

# Every line one call into the driver, so one transaction. The script makes,
# fills, moves, links and removes, and frees metadata blocks (a directory, an
# extent leaf) in the same operation that writes data - the case ordered mode
# has to log rather than write in place.
cat > "$T/script.txt" << 'EOF'
mkdir	/d
create	/d/a	644
write	/d/a	0	70000	3
mkdir	/d/sub
create	/d/sub/b	600
write	/d/sub/b	5000	9000	4
symlink	/d/a	/ln
link	/d/a	/hard
rename	/d/sub/b	/b2
write	/d/a	70000	300000	5
truncate	/d/a	4097
chmod	/b2	640
unlink	/hard
create	/many	644
write	/many	0	200000	6
writefile	/many	12000	7
create	/sp	644
write	/sp	0	4096	9
write	/sp	16384	4096	9
write	/sp	32768	4096	9
write	/sp	49152	4096	9
write	/sp	65536	4096	9
writefile	/sp	30000	10
rmdir	/d/sub
mkdir	/e
rename	/d	/e/d
create	/e/c	644
write	/e/c	0	200000	8
unlink	/e/c
EOF

cp "$T/base.img" "$T/states.img"
(cd "$T" && cp states.img fs.img && "$EX" states) > "$T/states.out" 2> "$T/states.err" \
    || fail "$(grep -m1 FAIL "$T/states.err" || tail -1 "$T/states.err")" states_failed
# One digest per state, in order.
awk -v d="$T" '/^STATE /{ f = d "/state." $2; printf "" > f; next } { print > f }' "$T/states.out"
nstates="$(grep -c '^STATE ' "$T/states.out")"
for ((k = 0; k < nstates; k++)); do
    md5sum < "$T/state.$k" | cut -c1-32
done > "$T/digests.txt"

(cd "$T" && cp base.img fs.img && "$EX" powercut 0) > "$T/full.out" 2> "$T/full.err" \
    || fail "$(grep -m1 FAIL "$T/full.err" || tail -1 "$T/full.err")" uncut_run_failed
total="$(sed -n 's/^writes=\([0-9]*\).*/\1/p' "$T/full.out")"
[ -n "$total" ] && [ "$total" -gt 0 ] || fail "$(cat "$T/full.out")" no_write_count
e2fsck -fn "$T/fs.img" > "$T/fsck0.log" 2>&1 || fail "$(head -4 "$T/fsck0.log" | tr '\n' ' ')" uncut_volume_not_clean
dumpe2fs -h "$T/fs.img" 2> /dev/null | grep -q 'Journal features:.*journal_checksum_v3' \
    || fail "the journal is not v3 after the driver wrote it" journal_not_v3

# The state a listing is: the first of the script's states, from `$2` on,
# whose listing it is, or -1. From `$2` on, because a tree can come back to
# one it had (a file made and removed again) and the cuts move forward.
state_of() {
    local d i
    d="$(md5sum < "$1" | cut -c1-32)"
    i="$(awk -v d="$d" -v from="$2" 'NR - 1 >= from && $0 == d { print NR - 1; exit }' "$T/digests.txt")"
    echo "${i:--1}"
}

points=0
last=-1
for ((n = 1; n <= total; n = (n + STRIDE <= total || n == total) ? n + STRIDE : total)); do
    points=$((points + 1))
    rm -rf "$T/c"
    mkdir "$T/c"
    cp "$T/base.img" "$T/c/fs.img"
    cp "$T/script.txt" "$T/c/script.txt"
    (cd "$T/c" && "$EX" powercut "$n") > "$T/c/cut.out" 2> "$T/c/cut.err" \
        || fail "n=$n $(grep -m1 FAIL "$T/c/cut.err" || tail -1 "$T/c/cut.err")" cut_run_failed
    cp "$T/c/fs.img" "$T/c/raw.img"

    (cd "$T/c" && "$EX" recover) > "$T/c/rec.out" 2> "$T/c/rec.err" \
        || fail "n=$n $(cat "$T/c/rec.err" "$T/c/rec.out" | head -2 | tr '\n' ' ')" driver_replay_failed
    e2fsck -fn "$T/c/fs.img" > "$T/c/fsck.log" 2>&1 \
        || fail "n=$n $(grep -v -E '^(e2fsck|Pass)' "$T/c/fsck.log" | head -4 | tr '\n' ' ')" driver_replay_not_clean
    (cd "$T/c" && "$EX" walk) > "$T/c/mine.txt" 2> "$T/c/walk.err" \
        || fail "n=$n $(head -1 "$T/c/walk.err")" driver_replay_unreadable

    e2fsck -fy "$T/c/raw.img" > "$T/c/fsy.log" 2>&1
    [ $? -le 1 ] || fail "n=$n $(head -4 "$T/c/fsy.log" | tr '\n' ' ')" e2fsck_replay_failed
    e2fsck -fn "$T/c/raw.img" > "$T/c/fsn.log" 2>&1 \
        || fail "n=$n $(grep -v -E '^(e2fsck|Pass)' "$T/c/fsn.log" | head -4 | tr '\n' ' ')" e2fsck_replay_not_clean
    mkdir "$T/c/e" && cp "$T/c/raw.img" "$T/c/e/fs.img"
    (cd "$T/c/e" && "$EX" walk) > "$T/c/theirs.txt" 2> "$T/c/walk2.err" \
        || fail "n=$n $(head -1 "$T/c/walk2.err")" e2fsck_replay_unreadable

    cmp -s "$T/c/mine.txt" "$T/c/theirs.txt" \
        || fail "n=$n $(diff "$T/c/mine.txt" "$T/c/theirs.txt" | head -3 | tr '\n' ' ')" replays_disagree
    from=$((last < 0 ? 0 : last))
    s="$(state_of "$T/c/mine.txt" "$from")"
    # The cuts are in order, so the state reached can only stay or move on:
    # one not found from the last one on is a mixture, or a state undone.
    [ "$s" -ge 0 ] || fail "n=$n $(diff "$T/state.$from" "$T/c/mine.txt" | head -4 | tr '\n' ' ')" not_a_state
    last="$s"
done
[ "$last" -eq $((nstates - 1)) ] || fail "the last cut reached state $last of $((nstates - 1))" sweep_did_not_finish
echo "ext4_powercut=ok points=$points writes=$total states=$nstates"
