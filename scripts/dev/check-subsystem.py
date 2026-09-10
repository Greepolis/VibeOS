#!/usr/bin/env python3
"""Does every module have the parts a module here is supposed to have?

`docs/core/architecture.md` lists seven, and they were written down because
nobody had written them down: eight registration seams exist in this tree, each
invented separately by somebody solving one problem, and several modules arrived
missing a piece. A page cache with no lock ran for months on the accident of
having exactly one caller, and adding a second caller returned one file's pages
under another file's key.

This script enforces the parts a script can judge, and says which those are
rather than implying it covers all seven.

## What is checked

1. **One header.** `include/vibeos/<module>.h` exists.
2. **State confined to one .c.** The module exports no global *data* symbol.
   This is measured with `nm` against the built objects, not with a regex: a
   lowercase symbol class is a `static`, an uppercase one is not, and the
   linker's opinion is the only one that matters here. A module whose state has
   a name another translation unit can write is not a module.
3. **Mutable state that names no lock at all.** Not "has no *registered* lock" -
   the first version of this check asked that, and called `blkdev.c` unlocked
   while it holds its own. What is measured is narrower and is the shape that
   actually cost months: a module with file-scope mutable state (lowercase `b`
   or `d` from `nm`) whose source does not mention a lock anywhere. `backing.c`
   was exactly that, and it was correct for months on the accident of having one
   caller - adding a second returned one file's pages under another file's key.

   Such a module is **not necessarily wrong**: it may be called under a lock its
   caller holds, which is a real design and several here use it. That is why
   this is ratcheted rather than forbidden. What the number says is how many
   modules would have to be reasoned about, one at a time, if somebody added a
   caller - and it is 4,000 lines of arch code away from where that decision
   gets made.
4. **A sabotage case file.** Three checks in this tree have shipped unable to
   fail. `scripts/dev/cases/` is where the argument that a check works lives.

## What is deliberately not checked, and by whom instead

- **A must-be-zero counter per module.** That is C2's step 3, which enumerates
  the modules that have none and adds them before the check exists. Writing the
  check first would mean shipping it red against thirty modules, which is the
  state `check-mm-layering.sh` sat in for a phase - "a check red since before
  you arrived is a check nobody reads".
- **An init that names its reason**, and **whether the registration point is
  the right one**. Both are review, and this file says so instead of pretending.

## Ratcheted

Today's violations are the baseline, per property, and may only go down. A
baseline is raised as a decision, in the change that earns it - never as a
reflex.

Usage: check-subsystem.py [--list] [<build-dir>]
"""

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# kernel/arch/ is excluded on purpose: arch_hw.c is the *subject* of this
# refactor, not a module that has failed to become one. Measuring it here would
# add one enormous violation that says nothing the plan does not already say.
AREAS = ("core", "exec", "fs", "io", "ipc", "mm", "net", "object", "proc",
         "sched", "time", "txn")

# Today's measurement. Each number is a debt, not a permission.
#
# exported_state is **already zero** across all 47 modules, which is the one
# part of the contract this tree got right without being told to - and it is
# worth stating rather than assuming, because it is the property the other three
# rest on: no module's state has a name another file can write.
BASELINE = {
    "no_header": 15,
    "exported_state": 0,
    "state_without_lock": 3,
    "no_case": 35,
}


def modules():
    out = []
    for a in AREAS:
        d = os.path.join(ROOT, "kernel", a)
        if not os.path.isdir(d):
            continue
        for n in sorted(os.listdir(d)):
            if n.endswith(".c"):
                out.append((a, n[:-2], os.path.join(d, n)))
    return out


def objects(build):
    """module source path -> object file, from the kernel core object tree."""
    found = {}
    base = os.path.join(ROOT, build, "CMakeFiles", "vibeos_kernel_core.dir")
    if not os.path.isdir(base):
        return found
    for cur, _, names in os.walk(base):
        for n in names:
            if n.endswith(".c.o"):
                rel = os.path.relpath(os.path.join(cur, n), base)
                found[rel.replace("\\", "/")] = os.path.join(cur, n)
    return found


def data_symbols(obj):
    """(exported, mutable_static) data symbols.

    Uppercase B/D/G is a linker-visible definition; lowercase b/d is a
    file-scope `static` that is written. Lowercase `r` is rodata and is not
    state, so it is left out of both.
    """
    try:
        out = subprocess.run(["nm", "--defined-only", obj],
                             capture_output=True, text=True,
                             check=False).stdout
    except OSError:
        return None, None
    exported, mutable = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 3:
            continue
        if parts[1] in ("B", "D", "G"):
            exported.append(parts[2])
        elif parts[1] in ("b", "d"):
            mutable.append(parts[2])
    return exported, mutable


def has_case(cases, area, name):
    for c in cases:
        stem = c[:-4] if c.endswith(".txt") else c
        if stem == name or stem == "%s-%s" % (area, name):
            return True
        if stem.startswith("%s-" % area) and name in stem:
            return True
    return False


def main():
    listing = "--list" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    build = args[0] if args else "build-clang-Release"

    objs = objects(build)
    if not objs:
        print("subsystem=skip reason=no_objects_in_%s" % build)
        return 0

    cases = set(os.listdir(os.path.join(ROOT, "scripts", "dev", "cases")))

    counts = dict((k, 0) for k in BASELINE)
    detail = []

    for area, name, path in modules():
        rel = "kernel/%s/%s.c" % (area, name)
        problems = []

        hdr = os.path.join(ROOT, "include", "vibeos", name + ".h")
        if not os.path.exists(hdr):
            problems.append("no_header")

        obj = objs.get(rel + ".o")
        syms, mutable = data_symbols(obj) if obj else (None, None)
        if syms:
            problems.append("exported_state")

        src = open(path, encoding="utf-8", errors="replace").read()
        if mutable and "lock" not in src:
            problems.append("state_without_lock")

        if not has_case(cases, area, name):
            problems.append("no_case")

        for p in problems:
            counts[p] += 1
        if problems:
            detail.append((rel, problems, syms or []))

    if listing:
        for rel, problems, syms in detail:
            extra = (" [%s]" % " ".join(syms[:4])) if syms else ""
            print("  %-34s %s%s" % (rel, ",".join(problems), extra))

    bad = [(k, counts[k], BASELINE[k]) for k in sorted(counts)
           if counts[k] > BASELINE[k]]
    if bad:
        for k, got, want in bad:
            print("  %s: %d modules, baseline %d" % (k, got, want))
        # Advice first, verdict last: check.sh reads this with `| tail -1`.
        print("      Raise a baseline in this file only as a decision, in the "
              "same commit that earns it.")
        print("subsystem=FAIL " +
              " ".join("%s=%d/%d" % (k, counts[k], BASELINE[k])
                       for k in sorted(counts)))
        return 1
    print("subsystem=ok modules=%d " % len(modules()) +
          " ".join("%s=%d" % (k, counts[k]) for k in sorted(counts)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
