#!/usr/bin/env bash
# Does somebody else's FAT implementation read what this kernel's driver wrote?
#
#     scripts/dev/verify-fat-mtools.sh [build-dir]
#
# docs/abi/ L1 step 4 gave the FAT driver a write path: files written in place,
# truncated, renamed, long names created, directories grown. Its host tests
# (tests/kernel/fat_tests.c) format their own volume and read back what they
# wrote, which proves the writer and the reader agree and nothing more - the
# ISO9660 driver refused every ISO ever made while a test of exactly that shape
# passed (CLAUDE.md).
#
# So: mformat makes the volume and mcopy puts a file on it, the driver mounts it
# and works on it (tests/kernel/fat_exercise.c), and then mdir and mcopy are
# asked what is there. Three things have to hold:
#
#   - the names mtools lists are exactly the ones the driver believes exist -
#     every file and directory, in the case it was written, and nothing else;
#   - every file mtools copies out is byte for byte what the driver wrote;
#   - the free space mtools counts is the free space the driver's statfs
#     reports, so no cluster was lost and none is claimed twice.
#
# And one thing mtools cannot say, because it follows whatever the directory
# entries and the table tell it: whether the volume is consistent with itself.
# scripts/dev/fat-fsck.py checks that - lost and cross-linked clusters, chains
# against sizes, "..", orphaned long names, duplicate 8.3 names - and it is run
# on mformat's own volume first: a checker that faults what somebody else's tool
# made is wrong about the format, and has to be found out before it is believed.
#
# One line, like every check here: fat-mtools=ok, =FAIL with the reason, or
# =skip when mtools is not installed (CI installs it).
set -u
cd "$(dirname "$0")/../.." || exit 1

B="${1:-build-gcc-Release}"
for tool in mformat mmd mcopy mdir; do
    if ! command -v "$tool" > /dev/null 2>&1; then
        echo "fat-mtools=skip reason=no_$tool"
        exit 0
    fi
done
if [ ! -x "$B/vibeos_fat_exercise" ]; then
    echo "fat-mtools=FAIL reason=no_exercise_binary_in_$B"
    exit 1
fi

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fail() {
    echo "  $1"
    echo "fat-mtools=FAIL reason=$2"
    exit 1
}

# Fifteen megabytes with four-sector clusters is FAT16 by mformat's own choice,
# the same geometry as the image the boot mounts at /fatlong.
#
# The image starts full of the letter A. mformat writes the boot sector, the
# tables and the root and leaves the rest, so a cluster the driver hands out
# holds 'A's until something writes it: a gap the driver forgot to zero reads
# back as that, where on a fresh image it would read as the zeros it should
# have been, and a directory cluster it forgot to zero is sixty-four files
# called AAAAAAAA.AAA. The byte matters - the first one tried was 0xE7, whose
# attribute bits say "volume label", and every reader skips those.
head -c $((15 * 1024 * 1024)) /dev/zero | tr '\0' 'A' > "$T/fs.img"
mformat -i "$T/fs.img" -c 4 :: || fail "mformat failed" mformat
mmd -i "$T/fs.img" "::/made by mtools" || fail "mmd failed" mmd
for i in $(seq 1 40); do
    echo "written by mcopy, read and rewritten by the kernel driver"
done > "$T/seed.txt"
mcopy -i "$T/fs.img" "$T/seed.txt" "::/made by mtools/a file mtools wrote.txt" \
    || fail "mcopy failed" mcopy

if ! python3 scripts/dev/fat-fsck.py "$T/fs.img" > "$T/fsck0.log" 2>&1; then
    fail "$(head -1 "$T/fsck0.log")" checker_faults_a_volume_mtools_made
fi

# What follows a directory's end marker is not promised to be zero, and here it
# is made not to be (see fat-fsck.py --dirty). mtools must still read the
# volume as it did - it stops at the marker - and a driver that writes a name
# over the marker without a new one after it shows a directory of AAAAAAAA.AAA.
python3 scripts/dev/fat-fsck.py --dirty "$T/fs.img" > /dev/null \
    || fail "could not dirty the directories" dirty
mdir -i "$T/fs.img" -/ -b :: > "$T/mdir0.txt" 2>&1
if [ "$(grep -c . "$T/mdir0.txt")" != "2" ]; then
    fail "$(head -3 "$T/mdir0.txt" | tr '\n' ' ')" dirtying_changed_what_mtools_reads
fi

if ! "$B/vibeos_fat_exercise" "$T/fs.img" "$T" > "$T/exercise.log" 2>&1; then
    fail "$(grep -m1 FAIL "$T/exercise.log" || tail -1 "$T/exercise.log")" driver_refused
fi

# What mtools sees: files as "::/path", directories as "::/path/".
mdir -i "$T/fs.img" -/ -b :: > "$T/mdir.txt" 2> "$T/mdir.err" \
    || fail "$(head -1 "$T/mdir.err")" mdir_cannot_read_the_volume
sed -n 's#^::/\(.*[^/]\)$#\1#p' "$T/mdir.txt" | sort > "$T/files.seen"
sed -n 's#^::/\(.*\)/$#\1#p' "$T/mdir.txt" | sort > "$T/dirs.seen"
cut -d' ' -f2- "$T/manifest" | sort > "$T/files.want"
sort "$T/dirs" > "$T/dirs.want"
if ! diff "$T/files.want" "$T/files.seen" > "$T/files.diff"; then
    fail "$(head -3 "$T/files.diff" | tr '\n' ' ')" files_differ
fi
if ! diff "$T/dirs.want" "$T/dirs.seen" > "$T/dirs.diff"; then
    fail "$(head -3 "$T/dirs.diff" | tr '\n' ' ')" directories_differ
fi

files=0
while read -r number path; do
    if ! mcopy -n -i "$T/fs.img" "::/$path" "$T/out.bin" 2> "$T/mcopy.err"; then
        fail "$path: $(head -1 "$T/mcopy.err")" mcopy_cannot_read_a_file
    fi
    if ! cmp -s "$T/out.bin" "$T/$number.bin"; then
        fail "$path: $(cmp "$T/out.bin" "$T/$number.bin" 2>&1 | head -1)" contents_differ
    fi
    rm -f "$T/out.bin"
    files=$((files + 1))
done < "$T/manifest"

seen_free="$(mdir -i "$T/fs.img" :: | sed -n 's/ bytes free.*//p' | tr -cd '0-9')"
want_free="$(cat "$T/free")"
if [ "$seen_free" != "$want_free" ]; then
    fail "mtools counts $seen_free bytes free, the driver $want_free" free_space_differs
fi

if ! python3 scripts/dev/fat-fsck.py "$T/fs.img" > "$T/fsck.log" 2>&1; then
    fail "$(head -1 "$T/fsck.log")" "volume_inconsistent:$(sed -n 's/.*first=//p' "$T/fsck.log")"
fi

echo "fat-mtools=ok files=$files free=$want_free $(tail -1 "$T/fsck.log")"
