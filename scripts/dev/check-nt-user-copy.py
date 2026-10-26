#!/usr/bin/env python3
"""Does the NT personality touch a program's memory only through nt_copy_in/out?

A row's pointer check covers the arguments it declares. A pointer found inside
a structure the program passed - a UNICODE_STRING's buffer, a rename's name -
is the program's to choose, and vibeos_uaccess_copy is fault-safe, not
range-checked: ring 0 reads any mapped page. NtQueryDirectoryFile read its
mask through such a pointer unjudged, and the listing became an oracle on
kernel memory a byte at a time (v7 review, 2026-10-26).

So in kernel/abi/nt every copy goes through nt_copy_in and nt_copy_out
(dispatch.c), which judge the range as the calling program's first, and the
only vibeos_uaccess_copy in the directory is inside those two. A copy that
bypasses them fails here; the check is the rule, not a reminder of it.

Usage: check-nt-user-copy.py
"""

import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AREA = os.path.join(ROOT, "kernel", "abi", "nt")
ALLOWED = {("dispatch.c", "nt_copy_in"), ("dispatch.c", "nt_copy_out")}
FUNC = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*$")


def main():
    bad = []
    sites = 0
    for path in sorted(glob.glob(os.path.join(AREA, "*.[ch]"))):
        name = os.path.basename(path)
        func = None
        with open(path, encoding="utf-8") as f:
            for n, line in enumerate(f, 1):
                m = FUNC.match(line)
                if m and not line.startswith((" ", "\t", "#", "/", "*")):
                    func = m.group(1)
                if "vibeos_uaccess_copy(" in line:
                    sites += 1
                    if (name, func) not in ALLOWED:
                        bad.append("%s:%d in %s" % (name, n, func))
    if bad or sites != 2:
        for b in bad:
            print("  a copy that bypasses nt_copy_in/nt_copy_out: " + b)
        print("nt-user-copy=FAIL sites=%d bypassing=%d" % (sites, len(bad)))
        return 1
    print("nt-user-copy=ok sites=%d" % sites)
    return 0


if __name__ == "__main__":
    sys.exit(main())
