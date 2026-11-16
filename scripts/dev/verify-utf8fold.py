#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The kernel's name folding (kernel/fs/utf8fold.c, E4-030) against Python's
own: NFD of the full case folding, which is what Linux's utf8 "nfdicf" is.

    scripts/dev/verify-utf8fold.py <build-dir> [ucd-12.1-dir]

Every code point Unicode 12.1 assigns, one at a time; then random names mixing
letters, precomposed and combining marks in shuffled order, Hangul, and the
characters whose folding expands (ß, ŉ, ǰ, ﬁ...). Python's data is a later
Unicode, so it is asked only about characters 12.1 assigns, and names with an
unassigned or invalid character are expected not to fold.

Prints utf8fold=ok checked=<n>, or utf8fold=FAILED with the first differences.
"""
import os
import random
import subprocess
import sys
import unicodedata


def assigned_12_1(ucd):
    out, first = [], None
    for line in open(os.path.join(ucd, "UnicodeData.txt"), encoding="utf-8"):
        f = line.split(";")
        cp, name = int(f[0], 16), f[1]
        if name.endswith(", First>"):
            first = cp
            continue
        out.append((first if name.endswith(", Last>") else cp, cp))
        first = None
    return out


def main():
    build = sys.argv[1] if len(sys.argv) > 1 else "build-gcc-Release"
    ucd = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser("~/vibeos-scratch/ucd-12.1")
    cli = os.path.join(build, "vibeos_utf8fold_cli")
    if not os.path.exists(cli):
        print("utf8fold=FAILED reason=cli_not_built")
        return 1
    if not os.path.exists(os.path.join(ucd, "UnicodeData.txt")):
        print("utf8fold=skip reason=no_ucd_12_1 (scripts/dev/make-utf8-fold.py says where it comes from)")
        return 0
    ranges = assigned_12_1(ucd)
    ok = set()
    for lo, hi in ranges:
        if hi - lo < 0x30000:
            ok.update(range(lo, hi + 1))
    singles = [cp for cp in sorted(ok) if not 0xD800 <= cp <= 0xDFFF]

    def expect(s):
        if any(ord(c) not in ok for c in s):
            return None
        return unicodedata.normalize("NFD", s.casefold())

    rng = random.Random(1)
    marks = [cp for cp in singles if unicodedata.combining(chr(cp))][:400]
    pool = ([ord(c) for c in "abcXYZßẞİıŉǰﬁﬀΣσςΆǅǄĳ"] + list(range(0xC0, 0x180)) +
            list(range(0xAC00, 0xAC40)) + list(range(0x1E00, 0x1E40)) + marks[:60])
    names = [chr(cp) for cp in singles]
    for _ in range(20000):
        n = rng.randrange(1, 12)
        names.append("".join(chr(rng.choice(pool)) for _ in range(n)))
    # Bytes that are not UTF-8, and a character 12.1 does not assign.
    raw = [b"\xff", b"a\xc0\xafb", b"\xed\xa0\x80"] + [chr(0x1FAD0).encode()]

    lines = [s.encode("utf-8", "surrogatepass").hex() for s in names] + [b.hex() for b in raw]
    r = subprocess.run([cli], input="\n".join(lines) + "\n", capture_output=True, text=True)
    got = r.stdout.split("\n")
    bad = []
    for s, g in zip(names, got):
        e = expect(s)
        want = "-" if e is None else e.encode("utf-8").hex()
        if g != want:
            bad.append("%r: got %s want %s" % (s, g, want))
    for b, g in zip(raw, got[len(names):]):
        if g != "-":
            bad.append("%r should not fold, got %s" % (b, g))
    if bad:
        print("\n".join(bad[:10]))
        print("utf8fold=FAILED differences=%d of %d" % (len(bad), len(names) + len(raw)))
        return 1
    print("utf8fold=ok checked=%d" % (len(names) + len(raw)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
