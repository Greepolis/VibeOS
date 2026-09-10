#!/usr/bin/env python3
"""Does any decision rest on the reverse map alone?

The reverse map is best effort by design. `vibeos_rmap_add` can fail - the node
pool is finite, and a frame outside the region it describes has no list at all -
and every caller ignores the result on purpose, because `arch_hw.c` chose to let
an unusual workload degrade reclaim rather than fail a mapping.

That choice is only safe because of one property:

    No decision rests on `vibeos_rmap_count` alone. Every consumer compares it
    against `vibeos_frame_owners` first and refuses when the two disagree.

`owners` is taken by the mapping itself and knows nothing about the node pool,
so an under-recorded holder makes the two disagree and the operation refuses. A
page that should be swapped out is not; a frame that could be compacted is not.
Reclaim degrades, and `exhausted`, `untracked` and `nodes_peak` say why.

Trusting `rmap_count` on its own turns that into corruption. Swap-out would
evict a frame a second address space still maps; compaction would move one and
leave the other mapping pointing at the old address. Both consumers exist today
and both cross-check - which is exactly the kind of property that holds until
somebody adds a third.

An external review found the ignored return value and asked what depends on the
map being complete. This is the answer, made checkable: a rule that lives only
in a comment erodes one reasonable-looking line at a time.

## How it decides

For every line calling `vibeos_rmap_count` or `vibeos_rmap_holders`, the
surrounding few lines must also mention `vibeos_frame_owners`. That is coarse,
and deliberately so: a data-flow analysis would need to understand the guards,
and a tool that guessed here would produce the confidently wrong answer this
project distrusts. Coarse and almost never wrong when it fires is the same trade
`check-reachable.py` makes.

Usage: check-rmap-crosscheck.py [--list]
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SCOPE = ("kernel",)
CONSUMERS = ("vibeos_rmap_count", "vibeos_rmap_holders")
GUARD = "vibeos_frame_owners"

# How far above and below a call the guard may sit. Both consumers today put it
# on the same line or the one before; the window is wider than that so a
# refactor that splits a condition does not fail for its formatting.
WINDOW = 6

# The reverse map's own file compares nothing: it *is* the count. Its tests
# check the layer in isolation and are allowed to read it directly.
EXEMPT = ("kernel/mm/rmap.c",)

# An escape hatch that has to be written down, not a silent exception.
#
# Not every read is a decision. A read whose only consequence is a refusal is
# safe by construction: an under-recorded holder makes the count smaller, so a
# bound passes more often and the real cross-check downstream still catches it.
# There is exactly one such site today and it says so on the lines above itself.
#
# Requiring the marker rather than widening the window is the trade
# check-chokepoints.py already makes: a second bare use is not forbidden, it is
# *noticed*, and somebody has to state why it is safe in the change that adds it.
MARKER = "rmap-bare-ok:"


def sources():
    out = []
    for d in SCOPE:
        for base, _, names in os.walk(os.path.join(ROOT, d)):
            if "build" in base or ".git" in base:
                continue
            for n in names:
                if n.endswith(".c"):
                    out.append(os.path.join(base, n))
    return out


def main():
    bad = []
    seen = 0

    for path in sorted(sources()):
        rel = os.path.relpath(path, ROOT).replace("\\", "/")
        if rel in EXEMPT:
            continue
        try:
            lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
        except OSError:
            continue
        for i, line in enumerate(lines):
            if not any(c in line for c in CONSUMERS):
                continue
            seen += 1
            lo = max(0, i - WINDOW)
            hi = min(len(lines), i + WINDOW + 1)
            near = "\n".join(lines[lo:hi])
            ok = GUARD in near or MARKER in near
            if "--list" in sys.argv:
                print("  %-28s %-4s %s" % (rel + ":" + str(i + 1),
                                           "ok" if ok else "BARE",
                                           line.strip()[:70]))
            if not ok:
                bad.append((rel, i + 1, line.strip()))

    if bad:
        for rel, ln, text in bad:
            print("  %s:%d decides on the reverse map alone" % (rel, ln))
            print("      %s" % text[:100])
        print("      The map is best effort: a holder can be missing without "
              "anything being wrong.")
        print("      Compare against %s and refuse when they disagree. "
              "See include/vibeos/rmap.h." % GUARD)
        print("rmap-crosscheck=FAIL bare=%d of %d" % (len(bad), seen))
        return 1
    print("rmap-crosscheck=ok consumers=%d" % seen)
    return 0


if __name__ == "__main__":
    sys.exit(main())
