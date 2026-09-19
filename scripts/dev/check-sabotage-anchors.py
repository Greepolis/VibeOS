#!/usr/bin/env python3
"""Does every sabotage case still have something to break?

`scripts/dev/cases/*.txt` are the argument that a check works: each case removes
or bends a line of source and a named check must go red. A case whose anchor no
longer exists in its target tests nothing, and says so only when somebody runs
it - `sabotage.py` prints "anchor not found; this case tests nothing" - which for
a suite that runs rarely means months.

Moving code makes this certain rather than possible. C4 stage 3 lifted 3,900
lines out of `arch_hw.c`, and every case anchored in them silently became a
proof of nothing. This check finds those the day the code moves.

## What it reads

A case file names its target with a comment line

    # Target source: kernel/abi/linux/misc.c

(or, for two-target files, `Target source: a.c (cases 1-2); b.h for the rest`).
Every `--- old` block is searched for in each target the file names; a case is
resolved if its anchor is in **any** of them.

A case file that names no target is checked more weakly: its anchors are searched
for in the *whole* source tree. An anchor that moved to a different file still
resolves there, but one that exists nowhere - a rewritten or deleted line - is
found. Twenty-one files pre-date the convention; naming their targets is how that
weaker search becomes the exact one.

## What the first run found

Twenty cases with nothing left to break. Sixteen were already stale before C4:
`mm-vmspace.txt` (seven anchors, rewritten by earlier vmspace changes), `threads`,
`tlb-shootdown`, `stress`, `journal`, `ahci`, `ring3-fault`, `services` and
`storage`. Nobody knew, because `sabotage.py` only says so when somebody runs it.
The other four were this phase's own: two whose anchors moved with the handlers,
and two written against names renamed in the same change and never run.

Ratcheted: the number of unresolved cases and of case files with no declared
target may go down and not up. The sixteen are recorded as debt, not fixed here -
each needs the sabotage re-run against its new anchor to confirm it still goes red.

Usage: check-sabotage-anchors.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CASES = os.path.join(ROOT, "scripts", "dev", "cases")

# Today's measurement. Each is a debt, not a permission.
BASELINE_UNRESOLVED = 16   # found by this check on its first run, all older than C4 (see docs/core/phases.md)
BASELINE_NO_TARGET = 21   # files with no declared target; each is a place a move can hide


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().replace("\r\n", "\n")


def parse_cases(text):
    """[(label, old)] using sabotage.py's own format."""
    cases, label, old, mode = [], None, [], None
    for line in text.splitlines():
        if line.startswith("#") and not line.startswith("### "):
            continue
        if line.startswith("### "):
            if label is not None:
                cases.append((label, "\n".join(old)))
            label, old, mode = line[4:].strip(), [], None
        elif line.strip() == "--- old":
            mode = "old"
        elif line.strip() == "+++ new":
            mode = "new"
        elif mode == "old":
            old.append(line)
    if label is not None:
        cases.append((label, "\n".join(old)))
    return cases


def whole_tree():
    """Every source file's text, for case files that declare no target."""
    parts = []
    for top in ("kernel", "include", "user", "boot", "bootloader", "scripts", "tests", "cmake"):
        base = os.path.join(ROOT, top)
        for cur, dirs, names in os.walk(base):
            dirs[:] = [d for d in dirs if d not in (".git", "cases") and not d.startswith("build")]
            for n in names:
                if n.endswith((".c", ".h", ".S", ".s", ".py", ".sh", ".cmake", ".txt", ".ld")):
                    try:
                        parts.append(read(os.path.join(cur, n)))
                    except OSError:
                        pass
    return "\n".join(parts)


def targets_of(text):
    out = []
    for m in re.finditer(r"^#\s*Target source:\s*(.+)$", text, re.M):
        for p in re.findall(r"[\w./-]+\.(?:c|h|S|s|py|sh|txt|cmake)\b", m.group(1)):
            if p not in out:
                out.append(p)
    return out


def main():
    listing = "--list" in sys.argv
    unresolved, no_target = [], []
    tree = [None]
    checked = 0
    for name in sorted(os.listdir(CASES)):
        if not name.endswith(".txt"):
            continue
        text = read(os.path.join(CASES, name))
        cases = parse_cases(text)
        if not cases:
            continue
        tgts = targets_of(text)
        if not tgts:
            # No declared target: look everywhere. Weaker - an anchor that moved to a
            # different file still resolves - but it still catches an anchor that
            # exists nowhere, which is what a rewritten or deleted line looks like.
            no_target.append(name)
            if tree[0] is None:
                tree[0] = whole_tree()
            for label, old in cases:
                checked += 1
                if old and old not in tree[0]:
                    unresolved.append((name, label))
            continue
        sources = []
        for t in tgts:
            p = os.path.join(ROOT, t)
            if os.path.exists(p):
                sources.append(read(p))
            else:
                unresolved.append((name, "<target missing: %s>" % t))
        blob = "\n".join(sources)
        for label, old in cases:
            checked += 1
            if old and old not in blob:
                unresolved.append((name, label))

    if listing:
        for n in no_target:
            print("  no target declared: %s" % n)
    if unresolved:
        for n, label in unresolved:
            print("  %s: anchor not found - '%s' tests nothing" % (n, label))
    bad = len(unresolved) > BASELINE_UNRESOLVED or len(no_target) > BASELINE_NO_TARGET
    if bad:
        print("      A case whose code moved needs its anchor re-pointed at the "
              "new file, in the same change as the move.")
        print("sabotage-anchors=FAIL unresolved=%d/%d no_target=%d/%d"
              % (len(unresolved), BASELINE_UNRESOLVED, len(no_target), BASELINE_NO_TARGET))
        return 1
    print("sabotage-anchors=ok cases=%d unresolved=%d no_target=%d"
          % (checked, len(unresolved), len(no_target)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
