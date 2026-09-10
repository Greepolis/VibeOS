#!/usr/bin/env python3
"""Is every MUSTBEZERO the kernel prints actually read by the boot gate?

Two checks already watch the checks. `check-counters-produced.py` matches every
reported counter against the `++` sites that could move it, and
`check-assertions-covered.py` matches every `reason=` the gate can emit against
the sabotage cases. Neither covers this axis, and the gap has a name in
CLAUDE.md already:

> **A line in the serial log is not a check.** The ring-3 ABI self-test printed
> "abi: mmap/mprotect/munmap wrong" for an entire session while the boot gate
> stayed green, because the line was collected into the summary and never
> asserted on.

`MUSTBEZERO` is the strongest claim this kernel makes about itself: the word
means the gate fails if the number is not zero. A counter that carries the word
and is read by nobody is worse than one that does not, because the word is what
a reader trusts instead of checking.

## What found this

`[GUI] MUSTBEZERO guard_broken=` was added during C0's GUI work, printed on
every boot, and never asserted. It was written by the same person who had just
quoted the rule above, in the same session. Neither existing check saw it:
`guard_broken` has a `++`, so counters-produced was satisfied, and it never
appears in a `reason=`, so assertions-covered had nothing to match.

## How it is measured

Every `MUSTBEZERO <name>=` string literal emitted anywhere under `kernel/`, and
every `MUSTBEZERO <name>=` pattern the gate searches for. A name in the first
set and not the second is the failure.

It cannot tell whether the gate, having matched the name, actually compares it
to zero - that is one line below the regex and stays in review. What it does
cover is the shape that has occurred twice: the line exists, the gate never
looks at it, and everything is green.

Usage: check-mustbezero-asserted.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

GATE = os.path.join(ROOT, "scripts", "qemu-cli-smoke-linux.py")

NAME = re.compile(r"MUSTBEZERO\s+([a-z_][a-z0-9_]*)=")


def emitted():
    """name -> the file that prints it."""
    out = {}
    for base, _, names in os.walk(os.path.join(ROOT, "kernel")):
        if "build" in base or ".git" in base:
            continue
        for n in names:
            if not n.endswith(".c"):
                continue
            p = os.path.join(base, n)
            text = open(p, encoding="utf-8", errors="replace").read()
            # Only string literals: the word also appears in the prose comments
            # that explain why a counter exists, and a comment is not a print.
            for line in text.splitlines():
                if '"' not in line:
                    continue
                for lit in re.findall(r'"([^"]*)"', line):
                    m = NAME.search(lit)
                    if m:
                        out.setdefault(m.group(1),
                                       os.path.relpath(p, ROOT).replace("\\", "/"))
    return out


def asserted():
    text = open(GATE, encoding="utf-8", errors="replace").read()
    return set(NAME.findall(text))


def main():
    emit = emitted()
    seen = asserted()

    missing = sorted(n for n in emit if n not in seen)
    # The other direction is a different defect and belongs to
    # check-counters-produced.py, which already owns "the gate looks for a
    # number nothing produces" - that is how VIBEOS_BLK_TIMEOUT was found.
    if "--list" in sys.argv:
        for n in sorted(emit):
            mark = "ok " if n in seen else "NOT"
            print("  %s %-24s %s" % (mark, n, emit[n]))

    if missing:
        for n in missing:
            print("  %s printed by %s and asserted by nobody" % (n, emit[n]))
        print("mustbezero-asserted=FAIL unread=%d" % len(missing))
        print("      MUSTBEZERO means the gate fails if it is not zero. A "
              "counter carrying the word and read by nobody is worse than one "
              "without it: the word is what a reader trusts instead of "
              "checking.")
        return 1
    print("mustbezero-asserted=ok counters=%d" % len(emit))
    return 0


if __name__ == "__main__":
    sys.exit(main())
