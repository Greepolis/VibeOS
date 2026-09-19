#!/usr/bin/env python3
"""Does every syscall run exactly the checks it declares?

C4 gave the kernel a vocabulary (include/vibeos/abi.h) in which each operation is
declared once with the checks that apply to it. A declaration nobody verifies is
the failure this project has produced most: a number written down and read by
nobody. This script reads it against the code, in both directions:

  - a check **declared and not run**: the handler no longer validates the
    pointer, or never did, and the table is promising something the kernel does
    not do;
  - a check **run and not declared**: the handler does something the table does
    not admit to, which is how "which calls touch user memory" stops being a
    question you can answer from one place;
  - an operation **declared and never dispatched**: an id no case reaches.

## How it decides "run"

The dispatcher (`vibeos_x86_64_linux_syscall`) is a `switch` on the operation.
For each `case VIBEOS_OP_X:` this takes the body, then follows every function it
names through the arch layer's own definitions - handlers call helpers that call
the choke points - and asks which choke points are reachable:

  USER_MEMORY   hw_user_range_ok, hw_user_range_why, hw_user_addr_ok
  SIGNAL_PERMIT hw_signal_permitted
  TASK_GUARD    hw_task_alloc_guarded

It is **reachability, not proof**. A handler that reaches `hw_user_range_ok` on a
path for a different argument satisfies the declaration without checking the
argument it should, and a function called through a pointer is not followed. Both
are stated rather than worked around: this catches a check that disappears
entirely (which is the failure that has actually occurred) and cannot certify that
the right pointer is the one checked. The choke-point counts in
check-chokepoints.py are the other half of the same argument.

Usage: check-syscall-checks.py [--list]
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

HEADER = os.path.join(ROOT, "include", "vibeos", "abi.h")
ARCH_DIR = os.path.join(ROOT, "kernel", "arch", "x86_64")
DISPATCHER = "vibeos_x86_64_linux_syscall"

CHOKEPOINTS = {
    "USER_MEMORY": ("hw_user_range_ok", "hw_user_range_why", "hw_user_addr_ok"),
    "SIGNAL_PERMIT": ("hw_signal_permitted",),
    "TASK_GUARD": ("hw_task_alloc_guarded",),
}

# Operations that call nothing which validates anything and say so by design:
# they are here so a reader sees the exemption where the check is enforced.
# (None today: an operation that declares NONE simply has none reachable.)

FUNC = re.compile(
    r"^(?:static\s+)?(?:[A-Za-z_][\w\s\*]*?)\b([A-Za-z_]\w*)\s*\(([^;{}]*)\)\s*\{",
    re.M)
IDENT = re.compile(r"\b[A-Za-z_]\w*\b")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read().replace("\r\n", "\n")


def declared():
    """id -> set of check names, from the X-macro list."""
    text = read(HEADER)
    start = text.index("#define VIBEOS_OP_LIST(X)")
    end = text.index("typedef enum vibeos_op_id")
    out = {}
    for m in re.finditer(r"X\((\w+),\s*\"[^\"]*\",\s*([^)]*)\)", text[start:end]):
        expr = m.group(2)
        out[m.group(1)] = set(
            n for n in ("USER_MEMORY", "SIGNAL_PERMIT", "TASK_GUARD")
            if "VIBEOS_CHECK_" + n in expr)
    return out


def function_bodies():
    """name -> body text, for every function defined in the arch layer."""
    bodies = {}
    for name in sorted(os.listdir(ARCH_DIR)):
        if not name.endswith(".c"):
            continue
        text = read(os.path.join(ARCH_DIR, name))
        for m in FUNC.finditer(text):
            fn = m.group(1)
            if fn in ("if", "for", "while", "switch", "return", "sizeof"):
                continue
            close = text.find("\n}\n", m.end())
            if close < 0:
                continue
            bodies.setdefault(fn, text[m.end():close])
    return bodies


def dispatcher_cases(bodies):
    """op id -> the text of its case body."""
    body = bodies.get(DISPATCHER)
    if body is None:
        sys.exit("check-syscall-checks: %s not found" % DISPATCHER)
    cases = {}
    labels = list(re.finditer(r"^        case VIBEOS_OP_(\w+):[^\n]*\n", body, re.M))
    ends = [m.start() for m in re.finditer(r"^        (?:case VIBEOS_OP_|default:)", body, re.M)]
    for m in labels:
        following = [e for e in ends if e > m.start()]
        stop = following[0] if following else len(body)
        cases.setdefault(m.group(1), "")
        cases[m.group(1)] += body[m.end():stop]
    return cases


def reachable_checks(text, bodies):
    seen = set()
    todo = [text]
    found = set()
    while todo:
        cur = todo.pop()
        for ident in IDENT.findall(cur):
            for check, names in CHOKEPOINTS.items():
                if ident in names:
                    found.add(check)
            if ident in bodies and ident not in seen and ident != DISPATCHER:
                seen.add(ident)
                todo.append(bodies[ident])
    return found


def main():
    decl = declared()
    bodies = function_bodies()
    cases = dispatcher_cases(bodies)
    listing = "--list" in sys.argv
    bad = []

    for op in sorted(decl):
        if op not in cases:
            bad.append("%s: declared but no case in the dispatcher reaches it" % op)
            continue
        actual = reachable_checks(cases[op], bodies)
        if listing:
            print("  %-16s declared=%-34s reachable=%s" % (
                op, ",".join(sorted(decl[op])) or "-", ",".join(sorted(actual)) or "-"))
        for c in sorted(decl[op] - actual):
            bad.append("%s: declares %s and no path from its handler runs it" % (op, c))
        for c in sorted(actual - decl[op]):
            bad.append("%s: runs %s and does not declare it" % (op, c))
    for op in sorted(cases):
        if op not in decl and op != "NONE":
            bad.append("%s: a dispatcher case with no declaration" % op)

    if bad:
        for line in bad:
            print("  " + line)
        print("syscall-checks=FAIL problems=%d" % len(bad))
        return 1
    print("syscall-checks=ok operations=%d" % len(decl))
    return 0


if __name__ == "__main__":
    sys.exit(main())
