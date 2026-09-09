#!/usr/bin/env python3
"""Do the security checks still have the number of call sites they declare?

These are choke points *because* they are called from one place per path and
defined once. Every security-relevant defect review has found in this project
was in `arch_hw.c`, and that is a consequence of its size: when everything lives
in one scope with no boundaries, a check that should apply everywhere is easy to
forget in one place and nothing notices.

The core refactor (docs/core/) moves four thousand lines of syscall handlers out
of that file. That is exactly the operation which turns one choke point into
two, and the second one is always the one that forgets a case. This check exists
so the move is visible while it is happening rather than afterwards.

## Why the count is asserted in both directions

The obvious failure is a *second* site: somebody copies a handler, copies its
guard, and the two drift. The count going up catches that.

The more likely one here is the count going *down*. `hw_user_range_ok` has 44
sites because every syscall taking a user pointer validates through it, and a
syscall that quietly stops validating is a hole, not a tidy-up. Nothing else in
the tree would notice that.

So a declared number that no longer matches is a failure either way. It is not a
prohibition - adding a syscall legitimately moves the count, and the fix is to
edit the number here, in the change that earns it. That is the same rule this
project already applies to the mm-layering allowance and the coverage baselines:
raise a limit as a decision, not as a reflex.

## What this cannot judge

It counts textual occurrences, not calls. A name inside a comment counts, and a
call reached through a function pointer does not. Both are stated rather than
worked around: a tool that guessed here would produce the confidently wrong
answer this project distrusts, and every one of these guards is called by name
today.

Usage: check-chokepoints.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

SCOPE = ("kernel",)

# name -> (occurrences, why this one is a choke point)
#
# The counts include the definition and any forward declaration, because
# separating those from calls needs a parser and the number's job is to move
# when something changes, not to be a call count.
CHOKEPOINTS = {
    "hw_signal_permitted": (
        5,
        "who may signal whom. Four callers and a definition; a fifth caller "
        "that forgot the check is a process signalling one it does not own."),
    "hw_user_range_ok": (
        44,
        "every syscall that takes a user pointer validates through it. This "
        "number going *down* is the failure that matters - a syscall that "
        "stopped checking - and nothing else in the tree would see it."),
    "hw_user_range_why": (
        6,
        "the same check with a reason attached. A refusal that names a "
        "mechanism instead of a situation cost a session once."),
    "hw_user_addr_ok": (
        2,
        "address policy for the two user windows. VibeOS programs link at "
        "0x8000000000 and Linux ones at 0x400000, inside the kernel's identity "
        "map, so 'is this a user address' is not a range test anybody should "
        "write twice."),
    "hw_task_alloc_guarded": (
        3,
        "the fork guard. A task table that can be exhausted by a loop is a "
        "denial of service with no privilege required."),
}


def sources():
    out = []
    for d in SCOPE:
        for base, _, names in os.walk(os.path.join(ROOT, d)):
            if "build" in base or ".git" in base:
                continue
            for n in names:
                if n.endswith((".c", ".h")):
                    out.append(os.path.join(base, n))
    return out


def main():
    files = sources()
    text = {}
    for f in files:
        try:
            text[f] = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            text[f] = ""

    bad = []
    for name, (want, why) in sorted(CHOKEPOINTS.items()):
        pat = re.compile(r"\b" + re.escape(name) + r"\s*\(")
        got = 0
        where = []
        for f, t in text.items():
            n = len(pat.findall(t))
            if n:
                got += n
                where.append("%s:%d" % (os.path.relpath(f, ROOT), n))
        if "--list" in sys.argv:
            print("  %-24s %2d (declared %2d)  %s"
                  % (name, got, want, " ".join(where)))
        if got != want:
            bad.append((name, got, want, why))

    if bad:
        for name, got, want, why in bad:
            direction = "more" if got > want else "FEWER"
            print("  %s: %d call sites, declared %d - %s than declared"
                  % (name, got, want, direction))
            print("      %s" % why)
        print("chokepoints=FAIL moved=%d" % len(bad))
        print("      If the change is deliberate, update the count in "
              "scripts/dev/check-chokepoints.py in the same commit.")
        return 1
    print("chokepoints=ok watched=%d" % len(CHOKEPOINTS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
