#!/usr/bin/env bash
# Name a ring-3 address against the guest binary that was actually running it.
#
# Every Linux program here links at 0x400000, so an address on its own names
# nothing: one fault was attributed to `fflush` (the wrong binary), then to
# argv construction (the right binary, read as if it were startup code), and
# only then to musl's free list, which was the truth. The crash record names
# the executable; this resolves the address in that executable, as built now.
#
# The guest binaries on the boot medium are stripped, so this looks for the
# unstripped build products by name. Against a CI failure, point it at the
# nightly's artifact bundle instead: a local rebuild has a different layout.
#
# Usage: symbolize-user.sh <binary-name-pattern> <rip> [fault-va] [search-dir...]
#   e.g. symbolize-user.sh musl_threads 0x405982 0x406d10
#        symbolize-user.sh busybox 0x4123ab 0 /tmp/nightly-artifacts
set -u
cd "$(dirname "$0")/../.." || exit 1

if [ $# -lt 2 ]; then
    sed -n '2,/^set -u/p' "$0" | sed -e 's/^# \{0,1\}//' -e '/^set -u/d'
    exit 2
fi
name="$1"
rip="$2"
va="${3:-0}"
shift 2
[ $# -gt 0 ] && shift
dirs=("$@")
[ ${#dirs[@]} -eq 0 ] && dirs=(build-clang-Release build-gcc-Release build-clang-Debug build-gcc-Debug)

found=0
while IFS= read -r b; do
    [ -z "$b" ] && continue
    # Only ELF files: the search pattern also matches sources and logs.
    head -c 4 "$b" 2>/dev/null | grep -q 'ELF' || continue
    found=1
    echo "=== $b"
    ls -la --time-style=full-iso "$b"
    if ! readelf -S "$b" 2>/dev/null | grep -q '\.symtab'; then
        echo "    stripped - no symbols to name anything with; find the unstripped build"
        continue
    fi
    echo "--- rip $rip"
    addr2line -f -C -e "$b" "$rip"
    objdump -d --no-show-raw-insn --start-address=$((rip - 40)) --stop-address=$((rip + 8)) "$b" 2>/dev/null | tail -14
    if [ "$va" != "0" ]; then
        # In Python, as integers. The awk this replaced compared nm's addresses
        # as awk "numbers", and 0000000000403e00 is 403e00 - scientific
        # notation - so the window printed symbols a page away; its section
        # lookup needed gawk's strtonum, and Ubuntu's awk is mawk.
        python3 - "$b" "$va" <<'PY'
import subprocess, sys
b, va = sys.argv[1], int(sys.argv[2], 0)
print("--- symbols within 512 bytes below and 64 above %#x" % va)
for line in subprocess.run(["nm", "-n", b], capture_output=True, text=True).stdout.splitlines():
    f = line.split()
    if len(f) == 3 and va - 512 <= int(f[0], 16) <= va + 64:
        print("    " + line)
print("--- the section %#x falls in" % va)
for line in subprocess.run(["readelf", "-SW", b], capture_output=True, text=True).stdout.splitlines():
    if "]" not in line:
        continue
    f = line.split("]", 1)[1].split()
    if len(f) >= 5:
        try:
            base, size = int(f[2], 16), int(f[4], 16)
        except ValueError:
            continue
        if base and base <= va < base + size:
            print("    %s  %#x..%#x" % (f[0], base, base + size))
PY
    fi
done < <(find "${dirs[@]}" -type f -iname "*${name}*" 2>/dev/null)

if [ "$found" -eq 0 ]; then
    echo "no ELF matching *${name}* under ${dirs[*]}"
    exit 1
fi
