#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every global table a program can fill says how it is shared out.

    check-pool-quota.py [--list]

A fixed table in a personality - a slot array of objects programs create:
APCs, NT objects, registry keys, epoll entries, futex waiters - is one
machine-wide pool. Without a share per process, one program takes all of it
and every other one is refused; the v8 review found the registry's pool, the
APC table and the thread slots in that state, the family M-110 and M-127
had been before them (M-134). The reviewer asked for the rule to be a gate
rather than a sentence: a table declares its quota where it is defined.

A table is a non-const file-scope array under kernel/, but for the
architecture's, whose elements are
a type of the file's own (`..._t`). Its declaration, or the comment block right
above it, says `quota:` - how much one process, thread or user may hold, and
where that is enforced - or `quota: none - <why>` when sharing it out is not
the answer (a table each process has exactly one entry in, say). Buffers and
lookup tables of plain integers are not pools and are not asked.

The tables that predate the gate are counted, and the count may only go down:
each one annotated, or given a real share, lowers the baseline in the same
change.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOP = os.path.join(ROOT, "kernel")
SKIP = os.path.join(ROOT, "kernel", "arch")
TABLE = re.compile(r"^static\s+(?!const\b)(?:volatile\s+)?([a-z_0-9]+_t)\s+\**g_[a-z0-9_]+\s*\[")

# The baseline may only go down.
BASELINE = 11


def tables():
    found = []
    for d, _, files in os.walk(TOP):
        if d == SKIP or d.startswith(SKIP + os.sep):
            continue
        for name in sorted(files):
            if not name.endswith(".c"):
                continue
            path = os.path.join(d, name)
            with open(path, encoding="utf-8") as f:
                lines = f.read().split("\n")
            for i, line in enumerate(lines):
                m = TABLE.match(line)
                if not m or m.group(1) in ("uint8_t", "uint16_t", "uint32_t", "uint64_t"):
                    continue
                # The declaration's own line, or the comment block right above it:
                # a neighbour's annotation four lines up is not this table's.
                near = [line]
                j = i - 1
                while j >= 0 and (lines[j].lstrip().startswith(("/*", "*")) or lines[j].rstrip().endswith("*/")):
                    near.append(lines[j])
                    j -= 1
                found.append((os.path.relpath(path, ROOT), i + 1, line.strip(), "quota:" in "\n".join(near)))
    return found


def main():
    rows = tables()
    missing = [r for r in rows if not r[3]]
    if "--list" in sys.argv:
        for path, n, line, ok in rows:
            print("%s %s:%d  %s" % ("ok  " if ok else "NONE", path, n, line))
    for path, n, line, _ in missing:
        print("  no quota declared: %s:%d  %s" % (path, n, line))
    if len(missing) > BASELINE:
        print("pool-quota=FAIL undeclared=%d baseline=%d" % (len(missing), BASELINE))
        print("      A new table names its per-process share beside it - `quota:` - or says why"
              " it has none.")
        return 1
    if len(missing) < BASELINE:
        print("pool-quota=FAIL undeclared=%d baseline=%d" % (len(missing), BASELINE))
        print("      Fewer than the baseline: lower BASELINE in this script to %d, in this change."
              % len(missing))
        return 1
    print("pool-quota=ok tables=%d undeclared=%d" % (len(rows), len(missing)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
