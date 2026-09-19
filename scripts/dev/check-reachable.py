#!/usr/bin/env python3
"""Which of the portable kernel's functions does nothing reach?

This project has a specific and repeated failure: code that exists, compiles,
passes host tests and is never executed by the machine. A portable syscall
dispatcher no process reaches. A scheduler policy the boot path does not
consult. A page cache with no caller. Four filesystem drivers that had never
parsed a byte. Each looked finished and each was, in the way that matters,
absent.

Reviewing for it does not work - the code looks right, because it is right; it
is simply not run. What does work is asking the question mechanically: is this
function named anywhere outside the file that defines it?

That is a weaker question than "is it reached from the boot path", and
deliberately so. A call graph from `vibeos_x86_64_hw_early_init` would be more
accurate and would need to understand function pointers, which this kernel uses
for every registration seam - so it would report the frame layer's lock and the
task view as unreachable, which is exactly the kind of confidently wrong answer
that makes people ignore a check. "Nobody names it" is coarse, is almost never
wrong when it fires, and catches every case this project has actually had.

## What it cost, once, and what would fix it

kernel/mm/vm.c and kernel/core/interrupts.c were constructed by the live
vibeos_kmain on every boot and consulted by nothing - two whole subsystems in
the shape this project names as its most repeated defect - and **this check
never said a word**, because they *were* named outside their own files: by
kmain.c and by syscall.c, both of which are themselves unreached. The question
"is it named anywhere" cannot see "named only by code that is itself dead".

The stronger version is a transitive walk seeded from the arch layer's entry
points, treating any function whose address is taken as reached so the
registration seams stay safe. That is the next improvement to this file, and it
is written down here rather than in a plan because this is where somebody
looking at the number will be.

Ratcheted, like the nightly coverage check: the count may go down and not up.
A new unreachable function is a new second kernel, and this is the check that
says so on the day it is written rather than months later.

## The fence (C3)

C3 deleted the second syscall dispatcher, and its gate is absolute: nothing in
the second process model may become reachable from ring 3 before its known
defects (thread creation with no check on the caller, a process slot never
freed, a caller identity read from an argument) are closed. A baseline that only
goes down cannot enforce that - a new caller *lowers* the unreached count and
the check is happier. So the fence is separate and absolute: no symbol defined
by a fenced file may be named by the code ring 3 enters through (kernel/arch),
by the bootloader, or by user space. kernel/core/kmain.c and kernel/ipc/waitset.c
name a few of them today; that is the portable kernel constructing and
consulting its own table at boot, and it is not a path a syscall takes. The
fence lifts in C5, which makes process.c the one owner of what a task is.

Usage: check-reachable.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Where the portable kernel lives. The architecture layer is excluded on
# purpose: it is full of functions the assembly calls, which no C file names.
SCOPE = ("kernel/core", "kernel/proc", "kernel/sched", "kernel/mm",
         "kernel/io", "kernel/fs", "kernel/txn", "kernel/net", "kernel/ipc",
         "kernel/object", "kernel/time")

# Everything that could possibly name a symbol: the whole tree except the file
# under examination. Tests count - a function reached only by a host test is
# still a function the machine does not run, but that is a different complaint
# and this check is not the place to make it, so tests are included as callers
# and the distinction is left to the reader.
SEARCH = ("kernel", "include", "tests", "user", "bootloader", "boot")

# The baseline may only go down.
#
# 27 when this was written, and the two that matter are in kernel/core/syscall.c
# and kernel/proc/process.c - the second dispatcher no process reaches, which
# carries three known defects and is C1's whole subject. The rest are mostly
# accessors written for a caller that has not arrived yet; each is a small bet
# that it will.
BASELINE = 12

# path -> the places that must never name a symbol it defines.
FENCED = {
    "kernel/proc/process.c": ("kernel/arch", "boot", "user"),
}


def c_files(dirs):
    out = []
    for d in dirs:
        p = os.path.join(ROOT, d)
        for base, _, names in os.walk(p):
            if "build" in base or ".git" in base:
                continue
            for n in names:
                if n.endswith((".c", ".h", ".S", ".s")):
                    out.append(os.path.join(base, n))
    return out


DEF = re.compile(
    r"^(?!static)(?:[A-Za-z_][A-Za-z0-9_ \*]*?)\b([a-z_][a-z0-9_]*)\s*\([^;]*\)\s*\{",
    re.M)


def fence_violations(text):
    out = []
    for src, banned in sorted(FENCED.items()):
        body = text.get(os.path.join(ROOT, src), "")
        names = sorted(set(m.group(1) for m in DEF.finditer(body)))
        for g, t in text.items():
            rel = os.path.relpath(g, ROOT).replace("\\", "/")
            if not rel.startswith(tuple(b + "/" for b in banned)):
                continue
            for n in names:
                if re.search(r"\b" + re.escape(n) + r"\b", t):
                    out.append((rel, n, src))
    return out


def main():
    everything = c_files(SEARCH)
    text = {}
    for f in everything:
        try:
            text[f] = open(f, encoding="utf-8", errors="replace").read()
        except OSError:
            text[f] = ""

    unreached = []
    for f in c_files(SCOPE):
        if not f.endswith(".c"):
            continue
        body = text.get(f, "")
        for m in DEF.finditer(body):
            name = m.group(1)
            if name in ("if", "for", "while", "switch", "return", "sizeof"):
                continue
            named_elsewhere = False
            for g, t in text.items():
                if g == f:
                    continue
                if re.search(r"\b" + re.escape(name) + r"\b", t):
                    named_elsewhere = True
                    break
            if not named_elsewhere:
                unreached.append((os.path.relpath(f, ROOT), name))

    fenced = fence_violations(text)
    if fenced:
        for f, n, src in fenced:
            print("  fenced: %s names %s, defined in %s" % (f, n, src))
        print("reachable=FAIL fenced=%d (a syscall path may not reach %s until C5)"
              % (len(fenced), ", ".join(sorted(FENCED))))
        return 1

    unreached.sort()
    if "--list" in sys.argv:
        for f, n in unreached:
            print("  nobody names it: %s  (%s)" % (n, f))

    print("unreached=%d baseline=%d" % (len(unreached), BASELINE))
    if len(unreached) > BASELINE:
        for f, n in unreached:
            print("  nobody names it: %s  (%s)" % (n, f))
        print("reachable=FAIL unreached=%d baseline=%d"
              % (len(unreached), BASELINE))
        return 1
    print("reachable=ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
