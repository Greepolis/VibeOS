#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# ext4 past 2^32 blocks (docs/implementation_progress/fs_ext4.md, E4-004).
#
#   scripts/dev/verify-ext4-huge.sh [build-dir] [scratch-dir]
#
# An extent's start has a high 16 bits, and nothing else ever needed them: every
# other fixture is a few megabytes. This one is 8 TiB - a sparse file of about
# 500 MB real - with 2 KiB blocks, the size at which mkfs.ext4 needs no meta_bg
# (which this driver does not read) and the block count passes 2^32. A file's
# one block is put above 2^32 by hand (debugfs has no set_bmap here, so the
# extent goes into the inode's root directly) and its bytes are written there.
# The driver must read them, and a write after them must allocate past 2^32 too;
# e2fsck must call the volume clean after.
#
# Not part of check.sh, and it says why it is careful. An exerciser that wrote a
# mapped volume back in full once turned this image into 333 GB of real data and
# took WSL down with it. So a watchdog runs beside every step and, past 2 GiB of
# real blocks, kills the exerciser and removes the image at once; and the image
# lives under a scratch directory (default ~/vibeos-scratch), never in /tmp,
# which a WSL restart empties half-way through a run.
#
# Prints ext4_huge=ok, =skip reason=..., or ext4_huge=FAILED reason=...

set -u
B="${1:-build-gcc-Release}"
S="${2:-$HOME/vibeos-scratch/ext4-huge}"
cd "$(dirname "$0")/../.." || exit 2
EX="$(cd "$B" && pwd)/vibeos_ext4_exercise"
LIMIT_KIB=2097152

fail() {
    echo "  $1"
    echo "ext4_huge=FAILED reason=$2"
    exit 1
}
for tool in mkfs.ext4 debugfs e2fsck dumpe2fs python3; do
    command -v "$tool" > /dev/null 2>&1 || { echo "ext4_huge=skip reason=no_$tool"; exit 0; }
done
[ -x "$EX" ] || fail "no exerciser at $EX" exercise_not_built
mkdir -p "$S" || fail "cannot make $S" scratch_unavailable
cd "$S" || exit 2
rm -f fs.img

( while sleep 2; do
      [ -f fs.img ] || continue
      if [ "$(du -k fs.img | cut -f1)" -gt "$LIMIT_KIB" ]; then
          echo "  WATCHDOG: the image passed 2 GiB of real blocks; exerciser killed, image removed"
          pkill -f "$EX"
          rm -f fs.img
          exit 0
      fi
  done ) &
WD=$!
trap 'kill $WD 2>/dev/null; rm -f fs.img payload cmds script.txt' EXIT

truncate -s 8200G fs.img
mkfs.ext4 -q -F -b 2048 -N 4096 -O 64bit -E lazy_itable_init=1,lazy_journal_init=1 fs.img > /dev/null 2>&1 \
    || fail "mkfs.ext4 refused the volume" mkfs_failed
dumpe2fs -h fs.img 2>/dev/null | grep -q "meta_bg" && fail "mkfs.ext4 used meta_bg" fixture_has_meta_bg
HI=4294967400
LO=$((HI - 4294967296))
G=$((HI / 16384))
FREEG=$(dumpe2fs fs.img 2>/dev/null | sed -n "/^Group $G:/,/free blocks/p" | sed -n 's/^ *\([0-9]*\) free blocks.*/\1/p' | head -1)
FREES=$(dumpe2fs -h fs.img 2>/dev/null | sed -n 's/^Free blocks: *//p')
[ -n "$FREEG" ] && [ -n "$FREES" ] || fail "cannot read group $G's free count" fixture_unreadable
head -c 2048 /dev/urandom > payload
cat > cmds <<EOF2
write /dev/null hi
sif hi block[0] 0x0001f30a
sif hi block[1] 4
sif hi block[2] 0
sif hi block[3] 0
sif hi block[4] 0x00010001
sif hi block[5] $LO
setb $HI
sif hi size 2048
sif hi blocks 4
set_bg $G free_blocks_count $((FREEG - 1))
set_bg $G flags 0
set_bg $G checksum calc
ssv free_blocks_count $((FREES - 1))
EOF2
debugfs -w -f cmds fs.img > /dev/null 2>&1
dd if=payload of=fs.img bs=2048 seek=$HI conv=notrunc status=none
e2fsck -fn fs.img > fsck0.log 2>&1 || fail "$(grep -v '^Pass\|^e2fsck' fsck0.log | head -2 | tr '\n' ' ')" fixture_not_clean
debugfs -R "ex /hi" fs.img 2>/dev/null | grep -q " $HI - $HI " || fail "the fixture's extent is not at $HI" fixture_wrong

want=$(python3 -c "import zlib; print('%08x' % (zlib.crc32(open('payload','rb').read()) & 0xffffffff))")
got=$("$EX" walk 2> /dev/null | awk '$1 == "/hi" {print $4}')
[ "$got" = "$want" ] || fail "read /hi above 2^32 as crc $got, the bytes are $want" read_above_2_32_wrong

printf 'write\t/hi\t0\t100\t5\nwrite\t/hi\t2048\t2048\t6\n' > script.txt
"$EX" apply > apply.out 2>&1 || fail "$(grep -m1 FAIL apply.out || head -1 apply.out)" write_refused
[ -f fs.img ] || fail "the watchdog removed the image" image_grew
e2fsck -fn fs.img > fsck1.log 2>&1 || fail "$(grep -v '^Pass\|^e2fsck' fsck1.log | head -2 | tr '\n' ' ')" written_not_clean
debugfs -R "ex /hi" fs.img 2>/dev/null | grep -q " 0 -     1 $HI - $((HI + 1)) " \
    || fail "the block written after it is not at $((HI + 1)): $(debugfs -R "ex /hi" fs.img 2>/dev/null | tail -1)" allocation_not_above_2_32
real=$(du -k fs.img | cut -f1)
echo "ext4_huge=ok blocks_above_2_32=2 real_kib=$real"
